#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import platform
import signal
import shlex
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
PROFILE_FILE = SCRIPT_DIR / "AgentBuildProfiles.json"
PRESET_FILE = REPO_ROOT / "CMakePresets.json"
LOCAL_CONFIG_FILE = REPO_ROOT / ".agents" / "build-config.json"
AGENT_BUILD_STATE_DIR = REPO_ROOT / "Build" / ".agent-state"
AGENT_BUILD_LOCK_DIR = REPO_ROOT / "Build" / ".agent-locks"

PROFILE_ENV_VAR = "DURIN_AGENT_BUILD_PROFILE"
JOBS_ENV_VAR = "DURIN_AGENT_JOBS"
CMAKE_ENV_VARS = ("DURIN_CMAKE_COMMAND", "DURIN_CMAKE_PATH")
SUPPORTED_ENVIRONMENT_PROVIDERS = {"inherit", "script", "visual-studio"}


class AgentBuildError(RuntimeError):
    pass


class AgentBuildInterruptedError(AgentBuildError):
    pass


def state_file_component(value: str) -> str:
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    return "".join(character if character in allowed else "_" for character in value)


def lock_file_path(root: Path = AGENT_BUILD_LOCK_DIR) -> Path:
    # Presets can share generated metadata and final outputs, so ownership belongs to the checkout.
    return root / "checkout.lock"


def interruption_marker_path(preset: str, root: Path = AGENT_BUILD_STATE_DIR) -> Path:
    return root / f"{state_file_component(preset)}.interrupted.json"


def read_state_description(path: Path) -> str:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return f'Existing state file: "{path}"'
    if not isinstance(value, dict):
        return f'Existing state file: "{path}"'

    fields = []
    displayed_fields = (
        ("pid", "PID"),
        ("profile", "profile"),
        ("preset", "preset"),
        ("action", "action"),
        ("target", "target"),
        ("startedAt", "started"),
    )
    for key, label in displayed_fields:
        if value.get(key) not in (None, ""):
            fields.append(f"{label}={value[key]}")
    return ", ".join(fields) if fields else f'Existing state file: "{path}"'


def write_json_state(path: Path, value: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(dict(value), indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


class AgentBuildLock:
    def __init__(self, path: Path, metadata: Mapping[str, Any]):
        self.path = path
        self.metadata = dict(metadata)
        self.handle: Any = None

    def __enter__(self) -> "AgentBuildLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = self.path.open("a+b")
        if self.path.stat().st_size == 0:
            self.handle.write(b"\0")
            self.handle.flush()
        self.handle.seek(0)

        try:
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(self.handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            self.handle.close()
            self.handle = None
            raise AgentBuildError(
                "Another BuildTool operation already owns this checkout. "
                + read_state_description(self.path)
            ) from exc

        self.handle.seek(0)
        self.handle.truncate()
        self.handle.write((json.dumps(self.metadata, indent=2) + "\n").encode("utf-8"))
        self.handle.flush()
        return self

    def __exit__(self, exc_type: Any, exc: Any, traceback: Any) -> None:
        if self.handle is None:
            return
        try:
            self.handle.seek(0)
            if os.name == "nt":
                import msvcrt

                msvcrt.locking(self.handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
        finally:
            self.handle.close()
            self.handle = None


def host_name(system_name: str | None = None) -> str:
    normalized = (system_name or platform.system()).lower()
    return {"darwin": "macos", "linux": "linux", "windows": "windows"}.get(normalized, normalized)


def load_json_object(path: Path, *, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise AgentBuildError(f'{label} was not found: "{path}"') from exc
    except json.JSONDecodeError as exc:
        raise AgentBuildError(
            f'{label} contains invalid JSON at line {exc.lineno}, column {exc.colno}: "{path}"'
        ) from exc
    except OSError as exc:
        raise AgentBuildError(f'Could not read {label.lower()} "{path}": {exc}') from exc

    if not isinstance(value, dict):
        raise AgentBuildError(f'{label} must contain a JSON object: "{path}"')
    return value


def optional_string(container: Mapping[str, Any], key: str, *, label: str) -> str:
    value = container.get(key, "")
    if not isinstance(value, str):
        raise AgentBuildError(f'{label} field "{key}" must be a string.')
    return value.strip()


def load_local_config(path: Path = LOCAL_CONFIG_FILE) -> dict[str, Any]:
    if not path.is_file():
        return {
            "cmakeCommand": "",
            "defaultBuildProfile": "",
            "jobs": 0,
            "environmentSetup": {"script": "", "arguments": []},
        }

    config = load_json_object(path, label="Agent build config")
    allowed_keys = {"cmakeCommand", "defaultBuildProfile", "jobs", "environmentSetup"}
    unknown_keys = sorted(set(config) - allowed_keys)
    if unknown_keys:
        raise AgentBuildError(f'Agent build config contains unknown field(s): {", ".join(unknown_keys)}.')

    cmake_command = optional_string(config, "cmakeCommand", label="Agent build config")
    default_profile = optional_string(config, "defaultBuildProfile", label="Agent build config")
    jobs = config.get("jobs", 0)
    if isinstance(jobs, bool) or not isinstance(jobs, int) or not 0 <= jobs <= 256:
        raise AgentBuildError('Agent build config field "jobs" must be an integer from 0 to 256.')
    environment_setup = config.get("environmentSetup", {})
    if not isinstance(environment_setup, dict):
        raise AgentBuildError('Agent build config field "environmentSetup" must be an object.')

    unknown_environment_keys = sorted(set(environment_setup) - {"script", "arguments"})
    if unknown_environment_keys:
        raise AgentBuildError(
            "Agent build config environmentSetup contains unknown field(s): "
            + ", ".join(unknown_environment_keys)
            + "."
        )

    script = optional_string(environment_setup, "script", label="Agent build config environmentSetup")
    arguments = environment_setup.get("arguments", [])
    if not isinstance(arguments, list) or not all(isinstance(item, str) for item in arguments):
        raise AgentBuildError('Agent build config field "environmentSetup.arguments" must be an array of strings.')

    return {
        "cmakeCommand": cmake_command,
        "defaultBuildProfile": default_profile,
        "jobs": jobs,
        "environmentSetup": {"script": script, "arguments": arguments},
    }


def load_profiles(path: Path = PROFILE_FILE) -> dict[str, dict[str, Any]]:
    manifest = load_json_object(path, label="Agent build profile manifest")
    if manifest.get("version") != 2:
        raise AgentBuildError('Agent build profile manifest field "version" must be 2.')

    profiles = manifest.get("profiles")
    if not isinstance(profiles, dict) or not profiles:
        raise AgentBuildError('Agent build profile manifest field "profiles" must be a non-empty object.')

    validated: dict[str, dict[str, Any]] = {}
    for name, raw_profile in profiles.items():
        if not isinstance(name, str) or not name or not isinstance(raw_profile, dict):
            raise AgentBuildError("Each Agent build profile must have a non-empty name and object value.")

        profile_host = optional_string(raw_profile, "host", label=f'Agent build profile "{name}"')
        default_preset = optional_string(raw_profile, "defaultPreset", label=f'Agent build profile "{name}"')
        provider = optional_string(raw_profile, "environmentProvider", label=f'Agent build profile "{name}"')
        platform_name = optional_string(raw_profile, "platform", label=f'Agent build profile "{name}"')
        suffix = optional_string(raw_profile, "testExecutableSuffix", label=f'Agent build profile "{name}"')
        is_default = raw_profile.get("default", False)
        required_commands = raw_profile.get("requiredCommands", [])
        presets = raw_profile.get("presets", [])

        if not profile_host or not default_preset or not provider or not platform_name:
            raise AgentBuildError(
                f'Agent build profile "{name}" requires host, defaultPreset, environmentProvider, and platform.'
            )
        if provider not in SUPPORTED_ENVIRONMENT_PROVIDERS:
            raise AgentBuildError(f'Agent build profile "{name}" uses unsupported environment provider "{provider}".')
        if not isinstance(is_default, bool):
            raise AgentBuildError(f'Agent build profile "{name}" field "default" must be a boolean.')
        if not isinstance(required_commands, list) or not all(
            isinstance(command, str) and command for command in required_commands
        ):
            raise AgentBuildError(f'Agent build profile "{name}" field "requiredCommands" must be an array of names.')
        if not isinstance(presets, list) or not presets or not all(
            isinstance(preset, str) and preset for preset in presets
        ):
            raise AgentBuildError(f'Agent build profile "{name}" field "presets" must be a non-empty array of names.')
        if len(set(presets)) != len(presets):
            raise AgentBuildError(f'Agent build profile "{name}" field "presets" contains duplicate names.')
        if default_preset not in presets:
            raise AgentBuildError(
                f'Agent build profile "{name}" defaultPreset must also appear in its presets list.'
            )

        validated[name] = {
            "host": profile_host,
            "defaultPreset": default_preset,
            "presets": presets,
            "environmentProvider": provider,
            "platform": platform_name,
            "testExecutableSuffix": suffix,
            "default": is_default,
            "requiredCommands": required_commands,
        }

    return validated


def load_configure_presets(path: Path = PRESET_FILE) -> dict[str, dict[str, Any]]:
    manifest = load_json_object(path, label="CMake preset manifest")
    raw_presets = manifest.get("configurePresets")
    if not isinstance(raw_presets, list):
        raise AgentBuildError('CMake preset manifest field "configurePresets" must be an array.')

    by_name: dict[str, dict[str, Any]] = {}
    for raw_preset in raw_presets:
        if not isinstance(raw_preset, dict):
            raise AgentBuildError("Each CMake configure preset must be an object.")
        name = optional_string(raw_preset, "name", label="CMake configure preset")
        if not name or name in by_name:
            raise AgentBuildError("Each CMake configure preset must have a unique non-empty name.")
        by_name[name] = raw_preset

    resolved: dict[str, dict[str, Any]] = {}
    resolving: set[str] = set()

    def resolve(name: str) -> dict[str, Any]:
        if name in resolved:
            return resolved[name]
        if name in resolving:
            raise AgentBuildError(f'CMake configure preset inheritance contains a cycle at "{name}".')
        raw_preset = by_name.get(name)
        if raw_preset is None:
            raise AgentBuildError(f'CMake configure preset "{name}" was not found.')

        resolving.add(name)
        merged: dict[str, Any] = {"cacheVariables": {}}
        inherits = raw_preset.get("inherits", [])
        if isinstance(inherits, str):
            inherits = [inherits]
        if not isinstance(inherits, list) or not all(isinstance(item, str) and item for item in inherits):
            raise AgentBuildError(f'CMake configure preset "{name}" has an invalid inherits field.')
        # CMake gives earlier entries in a multiple-inheritance list higher precedence.
        for parent_name in reversed(inherits):
            parent = resolve(parent_name)
            for key, value in parent.items():
                if key == "cacheVariables":
                    merged[key].update(value)
                else:
                    merged[key] = value

        for key, value in raw_preset.items():
            if key == "cacheVariables":
                if not isinstance(value, dict):
                    raise AgentBuildError(f'CMake configure preset "{name}" cacheVariables must be an object.')
                merged[key].update(value)
            elif key != "inherits":
                merged[key] = value
        merged["name"] = name
        resolving.remove(name)
        resolved[name] = merged
        return merged

    for preset_name in by_name:
        resolve(preset_name)
    return resolved


def preset_cache_string(preset: Mapping[str, Any], name: str, *, required: bool = True) -> str:
    value = preset.get("cacheVariables", {}).get(name)
    if isinstance(value, dict):
        value = value.get("value")
    if value is None and not required:
        return ""
    if not isinstance(value, (str, int, float, bool)):
        raise AgentBuildError(f'CMake preset "{preset["name"]}" must define {name}.')
    return str(value)


def preset_cache_bool(preset: Mapping[str, Any], name: str) -> bool:
    value = preset_cache_string(preset, name, required=False).strip().lower()
    return value in {"1", "on", "true", "yes", "y"}


def select_preset(
    profile: Mapping[str, Any],
    presets: Mapping[str, dict[str, Any]],
    *,
    requested: str = "",
) -> tuple[str, dict[str, Any]]:
    selected_name = requested or profile["defaultPreset"]
    if selected_name not in profile["presets"]:
        raise AgentBuildError(
            f'Preset "{selected_name}" is not enabled for this Agent build profile. '
            f'Available presets: {", ".join(profile["presets"])}.'
        )
    if selected_name not in presets:
        raise AgentBuildError(f'Enabled CMake preset "{selected_name}" was not found in "{PRESET_FILE}".')
    return selected_name, presets[selected_name]


def preset_build_directory(preset: Mapping[str, Any]) -> Path:
    binary_dir = preset.get("binaryDir")
    if not isinstance(binary_dir, str) or not binary_dir:
        raise AgentBuildError(f'CMake preset "{preset["name"]}" must define binaryDir.')
    expanded = binary_dir.replace("${sourceDir}", str(REPO_ROOT)).replace("${presetName}", preset["name"])
    path = Path(expanded).resolve()
    try:
        path.relative_to(REPO_ROOT.resolve())
    except ValueError as exc:
        raise AgentBuildError(f'CMake preset build directory must stay inside the checkout: "{path}"') from exc
    return path


def select_profile(
    profiles: Mapping[str, dict[str, Any]],
    *,
    requested: str = "",
    environment: Mapping[str, str] | None = None,
    configured: str = "",
    current_host: str | None = None,
) -> tuple[str, dict[str, Any]]:
    if environment is None:
        environment = os.environ
    selected_name = requested or environment.get(PROFILE_ENV_VAR, "").strip() or configured
    detected_host = current_host or host_name()

    if selected_name:
        if selected_name not in profiles:
            raise AgentBuildError(
                f'Unknown Agent build profile "{selected_name}". Available profiles: {", ".join(sorted(profiles))}.'
            )
        selected = profiles[selected_name]
        if selected["host"] != detected_host:
            raise AgentBuildError(
                f'Agent build profile "{selected_name}" targets host "{selected["host"]}", '
                f'but the current host is "{detected_host}".'
            )
        return selected_name, selected

    host_profiles = [(name, value) for name, value in profiles.items() if value["host"] == detected_host]
    default_profiles = [(name, value) for name, value in host_profiles if value["default"]]
    candidates = default_profiles or host_profiles
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise AgentBuildError(
            f'No Agent build profile is registered for host "{detected_host}". '
            f'Add a profile to "{PROFILE_FILE}" before building on this platform.'
        )
    raise AgentBuildError(
        f'Multiple Agent build profiles are available for host "{detected_host}"; '
        "select one with --profile or DURIN_AGENT_BUILD_PROFILE."
    )


def resolve_cmake_command(
    requested: str,
    configured: str,
    *,
    environment: Mapping[str, str] | None = None,
) -> str:
    if environment is None:
        environment = os.environ
    command = requested
    if not command:
        for variable in CMAKE_ENV_VARS:
            if environment.get(variable, "").strip():
                command = environment[variable].strip()
                break
    command = command or configured or "cmake"

    if Path(command).is_absolute() or any(separator in command for separator in ("/", "\\")):
        command_path = Path(command).expanduser()
        if not command_path.is_file():
            raise AgentBuildError(f'CMake command does not exist: "{command_path}"')
        return str(command_path.resolve())

    detected = shutil.which(command)
    if not detected:
        raise AgentBuildError(
            f'CMake command "{command}" was not found. Set --cmake, DURIN_CMAKE_COMMAND, '
            f'or cmakeCommand in "{LOCAL_CONFIG_FILE}".'
        )
    return detected


def resolve_jobs(
    requested: int | None,
    configured: int,
    *,
    environment: Mapping[str, str] | None = None,
    cpu_count: int | None = None,
) -> int:
    if environment is None:
        environment = os.environ
    if requested is not None:
        return requested

    environment_value = environment.get(JOBS_ENV_VAR, "").strip()
    if environment_value:
        try:
            environment_jobs = int(environment_value)
        except ValueError as exc:
            raise AgentBuildError(f"{JOBS_ENV_VAR} must be an integer from 1 to 256.") from exc
        if not 1 <= environment_jobs <= 256:
            raise AgentBuildError(f"{JOBS_ENV_VAR} must be an integer from 1 to 256.")
        return environment_jobs

    if configured:
        return configured
    detected = os.cpu_count() if cpu_count is None else cpu_count
    return max(1, min((detected or 1) - 2, 256))


def parse_environment_output(output: str, *, case_insensitive: bool = False) -> dict[str, str]:
    environment: dict[str, str] = {}
    for entry in output.replace("\r\n", "\n").split("\0" if "\0" in output else "\n"):
        if "=" not in entry:
            continue
        name, value = entry.split("=", 1)
        if name:
            environment[name] = value
    if not case_insensitive:
        return environment

    normalized: dict[str, str] = {}
    for name, value in environment.items():
        normalized_name = name.upper()
        if normalized_name not in normalized or name == normalized_name:
            normalized[normalized_name] = value
    return normalized


def capture_setup_environment(script: Path, arguments: Sequence[str], *, current_host: str) -> dict[str, str]:
    if not script.is_file():
        raise AgentBuildError(f'Environment setup script does not exist: "{script}"')

    if current_host == "windows":
        if script.suffix.lower() not in {".bat", ".cmd"}:
            raise AgentBuildError("Windows environment setup scripts must use the .bat or .cmd extension.")
        command = [
            os.environ.get("COMSPEC", "cmd.exe"),
            "/d",
            "/s",
            "/c",
            "call",
            str(script),
            *arguments,
            ">nul",
            "&&",
            "set",
        ]
    else:
        argument_text = " ".join(shlex.quote(item) for item in [str(script), *arguments])
        command = ["/bin/sh", "-c", f". {argument_text} >/dev/null && env -0"]

    result = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        details = result.stderr.strip()
        suffix = f"\n{details}" if details else ""
        raise AgentBuildError(
            f'Environment setup script failed with exit code {result.returncode}: "{script}"{suffix}'
        )
    return parse_environment_output(result.stdout, case_insensitive=current_host == "windows")


def find_vsdevcmd(environment: Mapping[str, str] | None = None) -> Path:
    if environment is None:
        environment = os.environ
    candidates: list[Path] = []
    for variable in ("ProgramFiles(x86)", "ProgramFiles"):
        root = environment.get(variable)
        if root:
            candidates.append(Path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe")

    vswhere = next((candidate for candidate in candidates if candidate.is_file()), None)
    if vswhere is None:
        raise AgentBuildError(
            "Visual Studio environment could not be detected because vswhere.exe was not found. "
            f'Set environmentSetup.script in "{LOCAL_CONFIG_FILE}".'
        )

    result = subprocess.run(
        [
            str(vswhere),
            "-latest",
            "-products",
            "*",
            "-requires",
            "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property",
            "installationPath",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    installation_path = result.stdout.strip()
    if result.returncode != 0 or not installation_path:
        raise AgentBuildError("vswhere.exe could not find a Visual Studio installation with the C++ toolchain.")

    vsdevcmd = Path(installation_path) / "Common7" / "Tools" / "VsDevCmd.bat"
    if not vsdevcmd.is_file():
        raise AgentBuildError(f'Visual Studio environment script does not exist: "{vsdevcmd}"')
    return vsdevcmd


def build_environment(
    profile: Mapping[str, Any],
    environment_setup: Mapping[str, Any],
    *,
    current_host: str,
) -> dict[str, str]:
    provider = profile["environmentProvider"]
    configured_script = environment_setup["script"]
    configured_arguments = list(environment_setup["arguments"])

    if provider == "inherit" and not configured_script:
        return dict(os.environ)
    if provider == "visual-studio":
        if current_host != "windows":
            raise AgentBuildError('The "visual-studio" environment provider is only supported on Windows.')
        script = Path(configured_script) if configured_script else find_vsdevcmd()
        arguments = configured_arguments or ["-arch=x64", "-host_arch=x64"]
        return capture_setup_environment(script, arguments, current_host=current_host)
    if provider == "script" or configured_script:
        if not configured_script:
            raise AgentBuildError(
                f'Profile environment provider "{provider}" requires environmentSetup.script in "{LOCAL_CONFIG_FILE}".'
            )
        return capture_setup_environment(Path(configured_script).expanduser(), configured_arguments, current_host=current_host)
    return dict(os.environ)


def environment_value(environment: Mapping[str, str], name: str) -> tuple[str, str]:
    for existing_name, value in environment.items():
        if existing_name.lower() == name.lower():
            return existing_name, value
    return name, ""


def ensure_required_commands(profile: Mapping[str, Any], environment: dict[str, str]) -> None:
    path_name, search_path = environment_value(environment, "PATH")
    for command in profile.get("requiredCommands", []):
        if shutil.which(command, path=search_path):
            continue

        if profile["environmentProvider"] == "visual-studio" and command.lower() == "ninja":
            _, visual_studio_root = environment_value(environment, "VSINSTALLDIR")
            bundled_ninja = (
                Path(visual_studio_root)
                / "Common7"
                / "IDE"
                / "CommonExtensions"
                / "Microsoft"
                / "CMake"
                / "Ninja"
                / "ninja.exe"
            )
            if visual_studio_root and bundled_ninja.is_file():
                environment[path_name] = str(bundled_ninja.parent) + os.pathsep + search_path
                search_path = environment[path_name]
                continue

        raise AgentBuildError(
            f'Required command "{command}" was not found for Agent build profile. '
            f'Initialize it in environmentSetup.script in "{LOCAL_CONFIG_FILE}".'
        )


def terminate_process_tree(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return

    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            cwd=REPO_ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return

    try:
        process.wait(timeout=10)
        return
    except subprocess.TimeoutExpired:
        pass

    if os.name == "nt":
        process.kill()
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            return
    process.wait()


def run_command(command: Sequence[str], *, environment: Mapping[str, str]) -> None:
    print("+ " + subprocess.list2cmdline(command), flush=True)
    popen_options: dict[str, Any] = {}
    if os.name == "nt":
        popen_options["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        popen_options["start_new_session"] = True

    try:
        process = subprocess.Popen(command, cwd=REPO_ROOT, env=dict(environment), **popen_options)
    except OSError as exc:
        raise AgentBuildError(f'Could not start command "{command[0]}": {exc}') from exc

    try:
        return_code = process.wait()
    except KeyboardInterrupt as exc:
        terminate_process_tree(process)
        raise AgentBuildInterruptedError(
            "BuildTool was interrupted. Confirm that the old build process tree has exited, "
            "then run rebuild --target all with the affected preset."
        ) from exc

    if return_code != 0:
        raise AgentBuildError(f'Command failed with exit code {return_code}: {command[0]}')


def validate_target(target: str, *, action: str) -> None:
    if not target:
        raise AgentBuildError(f"{action} requires --target <target-name>.")
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    if any(character not in allowed for character in target):
        raise AgentBuildError(f'Build target contains unsupported characters: "{target}"')


def validate_action_request(
    args: argparse.Namespace,
    *,
    preset: str,
    preset_metadata: Mapping[str, Any],
) -> str:
    if args.action in {"configure", "clean"}:
        return ""
    target = args.target or ("all" if args.action == "rebuild" else "")
    validate_target(target, action=args.action)
    if args.action == "test" and not preset_cache_bool(preset_metadata, "BUILD_TESTING"):
        raise AgentBuildError(f'Preset "{preset}" does not enable BUILD_TESTING.')
    return target


def cache_is_usable(cache_file: Path) -> bool:
    if not cache_file.is_file():
        return False
    try:
        content = cache_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return "CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND" not in content


def restore_state_file(path: Path, previous_content: bytes | None) -> None:
    if previous_content is None:
        try:
            path.unlink()
        except FileNotFoundError:
            pass
        return

    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    temporary.write_bytes(previous_content)
    os.replace(temporary, path)


def execute_with_recovery_marker(
    *,
    action: str,
    marker_file: Path,
    metadata: Mapping[str, Any],
    operation: Callable[[], None],
) -> None:
    try:
        previous_content = marker_file.read_bytes()
    except FileNotFoundError:
        previous_content = None
    except OSError as exc:
        raise AgentBuildError(f'Could not read Agent build recovery state "{marker_file}": {exc}') from exc

    if previous_content is not None and action in {"build", "test"}:
        raise AgentBuildError(
            "The previous BuildTool operation did not return normally. "
            + read_state_description(marker_file)
            + ". Confirm that its old process tree has exited, then run rebuild --target all."
        )

    write_json_state(marker_file, metadata)
    try:
        operation()
    except AgentBuildInterruptedError:
        # The current marker must survive cancellation so the next invocation cannot assume the build is consistent.
        raise
    except AgentBuildError:
        # A child process that returned a normal failure leaves Ninja/CMake aware of the failed edge.
        restore_state_file(marker_file, previous_content)
        raise
    except BaseException:
        # Unknown failures are treated conservatively because they may have bypassed normal child cleanup.
        raise
    else:
        if action == "rebuild" or previous_content is None:
            restore_state_file(marker_file, None)
        else:
            # Clean and configure are useful diagnostics, but cannot certify recovery from an earlier interruption.
            restore_state_file(marker_file, previous_content)


def perform_action(
    args: argparse.Namespace,
    *,
    cmake: str,
    jobs: int,
    environment: Mapping[str, str],
    build_directory: Path,
    cache_file: Path,
    preset: str,
    profile: Mapping[str, Any],
    preset_metadata: Mapping[str, Any],
) -> None:
    if args.action == "configure":
        run_command([cmake, "--fresh", "--preset", preset], environment=environment)
        return

    if args.action == "clean":
        if cache_is_usable(cache_file):
            run_command([cmake, "--build", str(build_directory), "--target", "clean"], environment=environment)
        else:
            print(f'Agent build tree is already clean or unconfigured: "{build_directory}"')
        return

    target = validate_action_request(args, preset=preset, preset_metadata=preset_metadata)

    if args.action == "rebuild":
        if cache_is_usable(cache_file):
            run_command([cmake, "--build", str(build_directory), "--target", "clean"], environment=environment)
        else:
            print(f'Skipping clean because the Agent build tree is unconfigured: "{build_directory}"')
        run_command([cmake, "--fresh", "--preset", preset], environment=environment)
    elif not cache_is_usable(cache_file):
        run_command([cmake, "--fresh", "--preset", preset], environment=environment)

    run_command(
        [cmake, "--build", str(build_directory), "--target", target, "-j", str(jobs)],
        environment=environment,
    )

    if args.action == "test":
        configuration = preset_cache_string(preset_metadata, "CMAKE_BUILD_TYPE")
        runtime_profile = preset_cache_string(preset_metadata, "DURIN_PROFILE_NAME")
        test_executable = (
            REPO_ROOT
            / "Engine"
            / "Binaries"
            / profile["platform"]
            / configuration
            / "Tests"
            / runtime_profile
            / "Bin"
            / f'{target}{profile["testExecutableSuffix"]}'
        )
        if not test_executable.is_file():
            raise AgentBuildError(f'Test target "{target}" did not produce "{test_executable}".')
        test_command = [str(test_executable)]
        if args.filter:
            test_command.append(f"--gtest_filter={args.filter}")
        run_command(test_command, environment=environment)


def execute(args: argparse.Namespace) -> None:
    config = load_local_config()
    if args.environment_setup:
        config["environmentSetup"]["script"] = args.environment_setup
    profiles = load_profiles()
    current_host = host_name()
    profile_name, profile = select_profile(
        profiles,
        requested=args.profile,
        configured=config["defaultBuildProfile"],
        current_host=current_host,
    )
    presets = load_configure_presets()
    preset, preset_metadata = select_preset(profile, presets, requested=args.preset)
    target = validate_action_request(args, preset=preset, preset_metadata=preset_metadata)
    cmake = resolve_cmake_command(args.cmake, config["cmakeCommand"])
    jobs = resolve_jobs(args.jobs, config["jobs"])
    environment = build_environment(profile, config["environmentSetup"], current_host=current_host)
    ensure_required_commands(profile, environment)

    build_directory = preset_build_directory(preset_metadata)
    cache_file = build_directory / "CMakeCache.txt"
    print(f'Agent build profile: "{profile_name}"')
    print(f'CMake preset: "{preset}"')
    print(f'CMake command: "{cmake}"')
    print(f"Parallel jobs: {jobs}")

    operation_metadata = {
        "pid": os.getpid(),
        "profile": profile_name,
        "preset": preset,
        "action": args.action,
        "target": target,
        "startedAt": datetime.now(timezone.utc).isoformat(),
    }
    with AgentBuildLock(lock_file_path(), operation_metadata):
        execute_with_recovery_marker(
            action=args.action,
            marker_file=interruption_marker_path(preset),
            metadata=operation_metadata,
            operation=lambda: perform_action(
                args,
                cmake=cmake,
                jobs=jobs,
                environment=environment,
                build_directory=build_directory,
                cache_file=cache_file,
                preset=preset,
                profile=profile,
                preset_metadata=preset_metadata,
            ),
        )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="BuildTool",
        description="Build Durin through one-shot commands or an interactive shell."
    )
    parser.add_argument(
        "action",
        nargs="?",
        default="shell",
        type=str.lower,
        choices=("shell", "configure", "build", "clean", "rebuild", "test"),
        help="operation to run; omit it to enter the interactive shell",
    )
    parser.add_argument("--target", default="", help="CMake target for build, rebuild, or test")
    parser.add_argument(
        "--jobs",
        type=int,
        choices=range(1, 257),
        default=None,
        metavar="1..256",
        help="parallel build job limit",
    )
    parser.add_argument("--filter", default="", help="GoogleTest filter for test")
    parser.add_argument("--profile", default="", help="Agent host environment profile")
    parser.add_argument("--preset", default="", help="registered CMake configure preset")
    parser.add_argument("--cmake", default="", help="CMake executable override")
    parser.add_argument("--environment-setup", default="", help="toolchain environment script override")
    return parser.parse_args(argv)


def shell_command_args(
    base_args: argparse.Namespace,
    action: str,
    *,
    target: str = "",
    test_filter: str = "",
) -> argparse.Namespace:
    return argparse.Namespace(
        action=action,
        target=target,
        jobs=base_args.jobs,
        filter=test_filter,
        profile=base_args.profile,
        preset=base_args.preset,
        cmake=base_args.cmake,
        environment_setup=base_args.environment_setup,
    )


def print_shell_help() -> None:
    print(
        "Commands:\n"
        "  /presets                  List available presets\n"
        "  /preset <name-or-number>  Select the current preset\n"
        "  /status                   Show the current preset\n"
        "  /configure                Configure the current preset\n"
        "  /build [target]           Build a target (default: all)\n"
        "  /clean                    Clean the current preset\n"
        "  /rebuild [target]         Clean, configure, and build (default: all)\n"
        "  /test <target> [filter]   Build and run a native test target\n"
        "  /help                     Show this help\n"
        "  /exit                     Leave the shell"
    )


def shell_presets(profile: Mapping[str, Any], current_preset: str) -> None:
    for index, preset in enumerate(profile["presets"], start=1):
        markers = []
        if preset == profile["defaultPreset"]:
            markers.append("default")
        if preset == current_preset:
            markers.append("current")
        suffix = f' [{", ".join(markers)}]' if markers else ""
        print(f"  {index:>2}  {preset}{suffix}")


def resolve_shell_preset(value: str, profile: Mapping[str, Any]) -> str:
    if value.isdigit():
        index = int(value)
        if 1 <= index <= len(profile["presets"]):
            return profile["presets"][index - 1]
    matches = [preset for preset in profile["presets"] if preset.lower() == value.lower()]
    if len(matches) == 1:
        return matches[0]
    raise AgentBuildError(f'Unknown preset selection "{value}". Use /presets to list available presets.')


def run_shell(args: argparse.Namespace) -> None:
    config = load_local_config()
    profiles = load_profiles()
    profile_name, profile = select_profile(
        profiles,
        requested=args.profile,
        configured=config["defaultBuildProfile"],
        current_host=host_name(),
    )
    presets = load_configure_presets()
    current_preset, _ = select_preset(profile, presets, requested=args.preset)

    print("Durin BuildTool shell")
    print(f'Agent build profile: "{profile_name}"')
    print(f'CMake preset: "{current_preset}"')
    print("Type /help for available commands.")

    while True:
        try:
            line = input("build> ").strip()
        except EOFError:
            print()
            return
        except KeyboardInterrupt:
            print("\nUse /exit to leave the shell.")
            continue
        if not line:
            continue

        try:
            parts = shlex.split(line)
        except ValueError as exc:
            print(f"Invalid command: {exc}", file=sys.stderr)
            continue
        command = parts[0].lower()
        values = parts[1:]
        if command.startswith("/"):
            command = command[1:]

        try:
            if command in {"exit", "quit"}:
                return
            if command in {"help", "?"}:
                print_shell_help()
                continue
            if command == "presets":
                shell_presets(profile, current_preset)
                continue
            if command == "status":
                print(f'CMake preset: "{current_preset}"')
                continue
            if command == "preset":
                if len(values) != 1:
                    raise AgentBuildError("/preset requires one preset name or number.")
                current_preset = resolve_shell_preset(values[0], profile)
                print(f'CMake preset: "{current_preset}"')
                continue
            if command in {"configure", "clean"}:
                if values:
                    raise AgentBuildError(f"/{command} does not accept positional arguments.")
                request = shell_command_args(args, command)
                request.preset = current_preset
                execute(request)
                continue
            if command in {"build", "rebuild"}:
                if len(values) > 1:
                    raise AgentBuildError(f"/{command} accepts at most one target.")
                request = shell_command_args(args, command, target=values[0] if values else "all")
                request.preset = current_preset
                execute(request)
                continue
            if command == "test":
                if not 1 <= len(values) <= 2:
                    raise AgentBuildError("/test requires a target and accepts an optional GoogleTest filter.")
                request = shell_command_args(
                    args, "test", target=values[0], test_filter=values[1] if len(values) == 2 else ""
                )
                request.preset = current_preset
                execute(request)
                continue
            raise AgentBuildError(f'Unknown shell command "{parts[0]}". Type /help for available commands.')
        except AgentBuildError as exc:
            print(exc, file=sys.stderr)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(argv)
        if args.action == "shell":
            run_shell(args)
        else:
            execute(args)
    except AgentBuildError as exc:
        print(exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"Operating system error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
