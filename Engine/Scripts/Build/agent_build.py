#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
PROFILE_FILE = SCRIPT_DIR / "AgentBuildProfiles.json"
LOCAL_CONFIG_FILE = REPO_ROOT / ".agents" / "build-config.json"

PROFILE_ENV_VAR = "DURIN_AGENT_BUILD_PROFILE"
CMAKE_ENV_VARS = ("DURIN_CMAKE_COMMAND", "DURIN_CMAKE_PATH")
SUPPORTED_ENVIRONMENT_PROVIDERS = {"inherit", "script", "visual-studio"}


class AgentBuildError(RuntimeError):
    pass


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
            "environmentSetup": {"script": "", "arguments": []},
        }

    config = load_json_object(path, label="Agent build config")
    allowed_keys = {"cmakeCommand", "defaultBuildProfile", "environmentSetup"}
    unknown_keys = sorted(set(config) - allowed_keys)
    if unknown_keys:
        raise AgentBuildError(f'Agent build config contains unknown field(s): {", ".join(unknown_keys)}.')

    cmake_command = optional_string(config, "cmakeCommand", label="Agent build config")
    default_profile = optional_string(config, "defaultBuildProfile", label="Agent build config")
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
        "environmentSetup": {"script": script, "arguments": arguments},
    }


def load_profiles(path: Path = PROFILE_FILE) -> dict[str, dict[str, Any]]:
    manifest = load_json_object(path, label="Agent build profile manifest")
    if manifest.get("version") != 1:
        raise AgentBuildError('Agent build profile manifest field "version" must be 1.')

    profiles = manifest.get("profiles")
    if not isinstance(profiles, dict) or not profiles:
        raise AgentBuildError('Agent build profile manifest field "profiles" must be a non-empty object.')

    validated: dict[str, dict[str, Any]] = {}
    for name, raw_profile in profiles.items():
        if not isinstance(name, str) or not name or not isinstance(raw_profile, dict):
            raise AgentBuildError("Each Agent build profile must have a non-empty name and object value.")

        profile_host = optional_string(raw_profile, "host", label=f'Agent build profile "{name}"')
        preset = optional_string(raw_profile, "preset", label=f'Agent build profile "{name}"')
        provider = optional_string(raw_profile, "environmentProvider", label=f'Agent build profile "{name}"')
        test_dir = optional_string(raw_profile, "testBinaryDirectory", label=f'Agent build profile "{name}"')
        suffix = optional_string(raw_profile, "testExecutableSuffix", label=f'Agent build profile "{name}"')
        is_default = raw_profile.get("default", False)
        required_commands = raw_profile.get("requiredCommands", [])

        if not profile_host or not preset or not provider or not test_dir:
            raise AgentBuildError(
                f'Agent build profile "{name}" requires host, preset, environmentProvider, '
                "and testBinaryDirectory."
            )
        if provider not in SUPPORTED_ENVIRONMENT_PROVIDERS:
            raise AgentBuildError(f'Agent build profile "{name}" uses unsupported environment provider "{provider}".')
        if not isinstance(is_default, bool):
            raise AgentBuildError(f'Agent build profile "{name}" field "default" must be a boolean.')
        if not isinstance(required_commands, list) or not all(
            isinstance(command, str) and command for command in required_commands
        ):
            raise AgentBuildError(f'Agent build profile "{name}" field "requiredCommands" must be an array of names.')

        validated[name] = {
            "host": profile_host,
            "preset": preset,
            "environmentProvider": provider,
            "testBinaryDirectory": test_dir,
            "testExecutableSuffix": suffix,
            "default": is_default,
            "requiredCommands": required_commands,
        }

    return validated


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
            f'No isolated Agent build profile is registered for host "{detected_host}". '
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


def run_command(command: Sequence[str], *, environment: Mapping[str, str]) -> None:
    print("+ " + subprocess.list2cmdline(command), flush=True)
    result = subprocess.run(command, cwd=REPO_ROOT, env=dict(environment), check=False)
    if result.returncode != 0:
        raise AgentBuildError(f'Command failed with exit code {result.returncode}: {command[0]}')


def validate_target(target: str, *, action: str) -> None:
    if not target:
        raise AgentBuildError(f"{action} requires --target <target-name>.")
    allowed = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.+-")
    if any(character not in allowed for character in target):
        raise AgentBuildError(f'Build target contains unsupported characters: "{target}"')


def cache_is_usable(cache_file: Path) -> bool:
    if not cache_file.is_file():
        return False
    try:
        content = cache_file.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return "CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND" not in content


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
    cmake = resolve_cmake_command(args.cmake, config["cmakeCommand"])
    environment = build_environment(profile, config["environmentSetup"], current_host=current_host)
    ensure_required_commands(profile, environment)

    preset = profile["preset"]
    build_directory = REPO_ROOT / "Build" / preset
    cache_file = build_directory / "CMakeCache.txt"
    print(f'Agent build profile: "{profile_name}"')
    print(f'CMake command: "{cmake}"')

    if args.action == "Configure":
        run_command([cmake, "--fresh", "--preset", preset], environment=environment)
        return

    validate_target(args.target, action=args.action)
    if not cache_is_usable(cache_file):
        run_command([cmake, "--fresh", "--preset", preset], environment=environment)
    run_command(
        [cmake, "--build", str(build_directory), "--target", args.target, "-j", str(args.jobs)],
        environment=environment,
    )

    if args.action == "Test":
        test_executable = (
            REPO_ROOT
            / profile["testBinaryDirectory"]
            / f'{args.target}{profile["testExecutableSuffix"]}'
        )
        if not test_executable.is_file():
            raise AgentBuildError(f'Test target "{args.target}" did not produce "{test_executable}".')
        test_command = [str(test_executable)]
        if args.filter:
            test_command.append(f"--gtest_filter={args.filter}")
        run_command(test_command, environment=environment)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Configure, build, or test an isolated Durin Agent build profile.")
    parser.add_argument("action", choices=("Configure", "Build", "Test"))
    parser.add_argument("--target", default="")
    parser.add_argument("--jobs", type=int, choices=range(1, 257), default=14, metavar="1..256")
    parser.add_argument("--filter", default="")
    parser.add_argument("--profile", default="")
    parser.add_argument("--cmake", default="")
    parser.add_argument("--environment-setup", default="")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    try:
        execute(parse_args(argv))
    except AgentBuildError as exc:
        print(exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"Operating system error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
