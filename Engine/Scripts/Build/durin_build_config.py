from __future__ import annotations

import json
import os
import platform
import shutil
from dataclasses import dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Any, Mapping


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
PROFILE_FILE = SCRIPT_DIR / "AgentBuildProfiles.json"
PRESET_FILE = REPO_ROOT / "CMakePresets.json"
LOCAL_CONFIG_FILE = REPO_ROOT / ".agents" / "build-config.json"
STATE_DIR = REPO_ROOT / "Build" / ".agent-state"
LOCK_DIR = REPO_ROOT / "Build" / ".agent-locks"

PROFILE_ENV_VAR = "DURIN_AGENT_BUILD_PROFILE"
JOBS_ENV_VAR = "DURIN_AGENT_JOBS"
CMAKE_ENV_VARS = ("DURIN_CMAKE_COMMAND", "DURIN_CMAKE_PATH")


class BuildToolError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        command: list[str] | None = None,
        exit_code: int | None = None,
        recovery: str = "",
    ):
        super().__init__(message)
        self.command = command
        self.exit_code = exit_code
        self.recovery = recovery


class BuildToolInterruptedError(BuildToolError):
    pass


class Action(str, Enum):
    SHELL = "shell"
    CONFIGURE = "configure"
    BUILD = "build"
    CLEAN = "clean"
    REBUILD = "rebuild"
    TEST = "test"
    PURGE = "purge"
    RUN = "run"


class EnvironmentProvider(str, Enum):
    INHERIT = "inherit"
    SCRIPT = "script"
    VISUAL_STUDIO = "visual-studio"


@dataclass(frozen=True)
class EnvironmentSetup:
    script: str = ""
    arguments: tuple[str, ...] = ()


@dataclass(frozen=True)
class LocalConfig:
    cmake_command: str = ""
    default_build_profile: str = ""
    jobs: int = 0
    environment_setup: EnvironmentSetup = EnvironmentSetup()

    def with_environment_script(self, script: str) -> "LocalConfig":
        return replace(
            self,
            environment_setup=replace(self.environment_setup, script=script),
        )


@dataclass(frozen=True)
class BuildProfile:
    name: str
    host: str
    default_preset: str
    presets: tuple[str, ...]
    environment_provider: EnvironmentProvider
    platform: str
    test_executable_suffix: str
    is_default: bool
    required_commands: tuple[str, ...]


@dataclass(frozen=True)
class ConfigurePreset:
    name: str
    values: Mapping[str, Any]


@dataclass(frozen=True)
class CommandRequest:
    action: Action
    target: str = ""
    jobs: int | None = None
    test_filter: str = ""
    run_arguments: tuple[str, ...] = ()
    profile: str = ""
    preset: str = ""
    cmake: str = ""
    environment_setup: str = ""
    all_presets: bool = False
    yes: bool = False
    plain: bool = False


@dataclass
class BuildContext:
    request: CommandRequest
    config: LocalConfig
    profile: BuildProfile
    presets: Mapping[str, ConfigurePreset]
    preset: ConfigurePreset
    current_host: str
    cmake: str = ""
    jobs: int = 0
    environment: dict[str, str] | None = None

    @property
    def target(self) -> str:
        if self.request.action is Action.REBUILD and not self.request.target:
            return "all"
        return self.request.target


def host_name(system_name: str | None = None) -> str:
    normalized = (system_name or platform.system()).lower()
    return {"darwin": "macos", "linux": "linux", "windows": "windows"}.get(normalized, normalized)


def load_json_object(path: Path, *, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise BuildToolError(f'{label} was not found: "{path}"') from exc
    except json.JSONDecodeError as exc:
        raise BuildToolError(
            f'{label} contains invalid JSON at line {exc.lineno}, column {exc.colno}: "{path}"'
        ) from exc
    except OSError as exc:
        raise BuildToolError(f'Could not read {label.lower()} "{path}": {exc}') from exc
    if not isinstance(value, dict):
        raise BuildToolError(f'{label} must contain a JSON object: "{path}"')
    return value


def optional_string(container: Mapping[str, Any], key: str, *, label: str) -> str:
    value = container.get(key, "")
    if not isinstance(value, str):
        raise BuildToolError(f'{label} field "{key}" must be a string.')
    return value.strip()


def load_local_config(path: Path = LOCAL_CONFIG_FILE) -> LocalConfig:
    if not path.is_file():
        return LocalConfig()
    raw = load_json_object(path, label="BuildTool config")
    unknown = sorted(set(raw) - {"cmakeCommand", "defaultBuildProfile", "jobs", "environmentSetup"})
    if unknown:
        raise BuildToolError(f'BuildTool config contains unknown field(s): {", ".join(unknown)}.')
    jobs = raw.get("jobs", 0)
    if isinstance(jobs, bool) or not isinstance(jobs, int) or not 0 <= jobs <= 256:
        raise BuildToolError('BuildTool config field "jobs" must be an integer from 0 to 256.')
    setup = raw.get("environmentSetup", {})
    if not isinstance(setup, dict):
        raise BuildToolError('BuildTool config field "environmentSetup" must be an object.')
    unknown_setup = sorted(set(setup) - {"script", "arguments"})
    if unknown_setup:
        raise BuildToolError(
            "BuildTool config environmentSetup contains unknown field(s): " + ", ".join(unknown_setup) + "."
        )
    arguments = setup.get("arguments", [])
    if not isinstance(arguments, list) or not all(isinstance(item, str) for item in arguments):
        raise BuildToolError('BuildTool config field "environmentSetup.arguments" must be an array of strings.')
    return LocalConfig(
        cmake_command=optional_string(raw, "cmakeCommand", label="BuildTool config"),
        default_build_profile=optional_string(raw, "defaultBuildProfile", label="BuildTool config"),
        jobs=jobs,
        environment_setup=EnvironmentSetup(
            script=optional_string(setup, "script", label="BuildTool config environmentSetup"),
            arguments=tuple(arguments),
        ),
    )


def load_profiles(path: Path = PROFILE_FILE) -> dict[str, BuildProfile]:
    manifest = load_json_object(path, label="Build profile manifest")
    if manifest.get("version") != 2:
        raise BuildToolError('Build profile manifest field "version" must be 2.')
    raw_profiles = manifest.get("profiles")
    if not isinstance(raw_profiles, dict) or not raw_profiles:
        raise BuildToolError('Build profile manifest field "profiles" must be a non-empty object.')

    profiles: dict[str, BuildProfile] = {}
    for name, raw in raw_profiles.items():
        if not isinstance(name, str) or not name or not isinstance(raw, dict):
            raise BuildToolError("Each build profile must have a non-empty name and object value.")
        host = optional_string(raw, "host", label=f'Build profile "{name}"')
        default_preset = optional_string(raw, "defaultPreset", label=f'Build profile "{name}"')
        provider_value = optional_string(raw, "environmentProvider", label=f'Build profile "{name}"')
        platform_name = optional_string(raw, "platform", label=f'Build profile "{name}"')
        suffix = optional_string(raw, "testExecutableSuffix", label=f'Build profile "{name}"')
        is_default = raw.get("default", False)
        required_commands = raw.get("requiredCommands", [])
        presets = raw.get("presets", [])
        if not host or not default_preset or not provider_value or not platform_name:
            raise BuildToolError(
                f'Build profile "{name}" requires host, defaultPreset, environmentProvider, and platform.'
            )
        try:
            provider = EnvironmentProvider(provider_value)
        except ValueError as exc:
            raise BuildToolError(
                f'Build profile "{name}" uses unsupported environment provider "{provider_value}".'
            ) from exc
        if not isinstance(is_default, bool):
            raise BuildToolError(f'Build profile "{name}" field "default" must be a boolean.')
        if not isinstance(required_commands, list) or not all(
            isinstance(command, str) and command for command in required_commands
        ):
            raise BuildToolError(f'Build profile "{name}" field "requiredCommands" must be an array of names.')
        if not isinstance(presets, list) or not presets or not all(
            isinstance(preset, str) and preset for preset in presets
        ):
            raise BuildToolError(f'Build profile "{name}" field "presets" must be a non-empty array of names.')
        if len(set(presets)) != len(presets):
            raise BuildToolError(f'Build profile "{name}" field "presets" contains duplicate names.')
        if default_preset not in presets:
            raise BuildToolError(f'Build profile "{name}" defaultPreset must also appear in its presets list.')
        profiles[name] = BuildProfile(
            name=name,
            host=host,
            default_preset=default_preset,
            presets=tuple(presets),
            environment_provider=provider,
            platform=platform_name,
            test_executable_suffix=suffix,
            is_default=is_default,
            required_commands=tuple(required_commands),
        )
    return profiles


def load_configure_presets(path: Path = PRESET_FILE) -> dict[str, ConfigurePreset]:
    manifest = load_json_object(path, label="CMake preset manifest")
    raw_presets = manifest.get("configurePresets")
    if not isinstance(raw_presets, list):
        raise BuildToolError('CMake preset manifest field "configurePresets" must be an array.')
    by_name: dict[str, dict[str, Any]] = {}
    for raw in raw_presets:
        if not isinstance(raw, dict):
            raise BuildToolError("Each CMake configure preset must be an object.")
        name = optional_string(raw, "name", label="CMake configure preset")
        if not name or name in by_name:
            raise BuildToolError("Each CMake configure preset must have a unique non-empty name.")
        by_name[name] = raw

    resolved: dict[str, ConfigurePreset] = {}
    resolving: set[str] = set()

    def resolve(name: str) -> ConfigurePreset:
        if name in resolved:
            return resolved[name]
        if name in resolving:
            raise BuildToolError(f'CMake configure preset inheritance contains a cycle at "{name}".')
        raw = by_name.get(name)
        if raw is None:
            raise BuildToolError(f'CMake configure preset "{name}" was not found.')
        resolving.add(name)
        merged: dict[str, Any] = {"cacheVariables": {}}
        inherits = raw.get("inherits", [])
        if isinstance(inherits, str):
            inherits = [inherits]
        if not isinstance(inherits, list) or not all(isinstance(item, str) and item for item in inherits):
            raise BuildToolError(f'CMake configure preset "{name}" has an invalid inherits field.')
        for parent_name in reversed(inherits):
            parent = resolve(parent_name).values
            for key, value in parent.items():
                if key == "cacheVariables":
                    merged[key].update(value)
                else:
                    merged[key] = value
        for key, value in raw.items():
            if key == "cacheVariables":
                if not isinstance(value, dict):
                    raise BuildToolError(f'CMake configure preset "{name}" cacheVariables must be an object.')
                merged[key].update(value)
            elif key != "inherits":
                merged[key] = value
        merged["name"] = name
        resolving.remove(name)
        resolved[name] = ConfigurePreset(name, merged)
        return resolved[name]

    for preset_name in by_name:
        resolve(preset_name)
    return resolved


def select_profile(
    profiles: Mapping[str, BuildProfile],
    *,
    requested: str = "",
    configured: str = "",
    environment: Mapping[str, str] | None = None,
    current_host: str | None = None,
) -> BuildProfile:
    environment = os.environ if environment is None else environment
    detected_host = current_host or host_name()
    selected_name = requested or environment.get(PROFILE_ENV_VAR, "").strip() or configured
    if selected_name:
        selected = profiles.get(selected_name)
        if selected is None:
            raise BuildToolError(
                f'Unknown build profile "{selected_name}". Available profiles: {", ".join(sorted(profiles))}.'
            )
        if selected.host != detected_host:
            raise BuildToolError(
                f'Build profile "{selected_name}" targets host "{selected.host}", '
                f'but the current host is "{detected_host}".'
            )
        return selected
    host_profiles = [profile for profile in profiles.values() if profile.host == detected_host]
    defaults = [profile for profile in host_profiles if profile.is_default]
    candidates = defaults or host_profiles
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise BuildToolError(
            f'No build profile is registered for host "{detected_host}". '
            f'Add a profile to "{PROFILE_FILE}" before building on this platform.'
        )
    raise BuildToolError(
        f'Multiple build profiles are available for host "{detected_host}"; '
        "select one with --profile or DURIN_AGENT_BUILD_PROFILE."
    )


def select_preset(
    profile: BuildProfile,
    presets: Mapping[str, ConfigurePreset],
    *,
    requested: str = "",
) -> ConfigurePreset:
    selected_name = requested or profile.default_preset
    if selected_name not in profile.presets:
        raise BuildToolError(
            f'Preset "{selected_name}" is not enabled for this build profile. '
            f'Available presets: {", ".join(profile.presets)}.'
        )
    if selected_name not in presets:
        raise BuildToolError(f'Enabled CMake preset "{selected_name}" was not found in "{PRESET_FILE}".')
    return presets[selected_name]


def preset_cache_string(preset: ConfigurePreset, name: str, *, required: bool = True) -> str:
    value = preset.values.get("cacheVariables", {}).get(name)
    if isinstance(value, dict):
        value = value.get("value")
    if value is None and not required:
        return ""
    if not isinstance(value, (str, int, float, bool)):
        raise BuildToolError(f'CMake preset "{preset.name}" must define {name}.')
    return str(value)


def preset_cache_bool(preset: ConfigurePreset, name: str) -> bool:
    return preset_cache_string(preset, name, required=False).strip().lower() in {
        "1", "on", "true", "yes", "y"
    }


def expand_preset_path(value: Any, preset: ConfigurePreset, *, root: Path = REPO_ROOT) -> Path:
    if not isinstance(value, str) or not value:
        raise BuildToolError(f'CMake preset "{preset.name}" contains an invalid path.')
    expanded = value.replace("${sourceDir}", str(root)).replace("${presetName}", preset.name)
    path = Path(expanded).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError as exc:
        raise BuildToolError(f'CMake preset build directory must stay inside the checkout: "{path}"') from exc
    return path


def preset_build_directory(preset: ConfigurePreset, *, root: Path = REPO_ROOT) -> Path:
    binary_dir = preset.values.get("binaryDir")
    if not isinstance(binary_dir, str) or not binary_dir:
        raise BuildToolError(f'CMake preset "{preset.name}" must define binaryDir.')
    return expand_preset_path(binary_dir, preset, root=root)


def preset_install_directory(preset: ConfigurePreset, *, root: Path = REPO_ROOT) -> Path | None:
    install_prefix = preset_cache_string(preset, "CMAKE_INSTALL_PREFIX", required=False)
    return expand_preset_path(install_prefix, preset, root=root) if install_prefix else None


def preset_output_configuration(preset: ConfigurePreset) -> str:
    configuration = preset_cache_string(preset, "CMAKE_BUILD_TYPE")
    identifier = preset_cache_string(preset, "DURIN_BUILD_IDENTIFIER", required=False)
    return f"{configuration}-{identifier}" if identifier else configuration


def resolve_cmake_command(
    requested: str,
    configured: str,
    *,
    environment: Mapping[str, str] | None = None,
) -> str:
    environment = os.environ if environment is None else environment
    command = requested
    if not command:
        command = next(
            (environment[name].strip() for name in CMAKE_ENV_VARS if environment.get(name, "").strip()),
            "",
        )
    command = command or configured or "cmake"
    if Path(command).is_absolute() or any(separator in command for separator in ("/", "\\")):
        path = Path(command).expanduser()
        if not path.is_file():
            raise BuildToolError(f'CMake command does not exist: "{path}"')
        return str(path.resolve())
    detected = shutil.which(command)
    if not detected:
        raise BuildToolError(
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
    environment = os.environ if environment is None else environment
    if requested is not None:
        return requested
    raw_jobs = environment.get(JOBS_ENV_VAR, "").strip()
    if raw_jobs:
        try:
            jobs = int(raw_jobs)
        except ValueError as exc:
            raise BuildToolError(f"{JOBS_ENV_VAR} must be an integer from 1 to 256.") from exc
        if not 1 <= jobs <= 256:
            raise BuildToolError(f"{JOBS_ENV_VAR} must be an integer from 1 to 256.")
        return jobs
    if configured:
        return configured
    detected = os.cpu_count() if cpu_count is None else cpu_count
    return max(1, min((detected or 1) - 2, 256))
