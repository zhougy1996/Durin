from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

from ..context import RepositoryContext
from ..toolchain import find_command
from .errors import BuildToolError, BuildToolInterruptedError
from .models import (
    Action,
    BuildProfile,
    ConfigurePreset,
    CreateKind,
    EnvironmentProvider,
    EnvironmentSetup,
    LinkType,
    LocalConfig,
    ModuleKind,
    OutputMode,
    TestGranularity,
    TestMode,
)

PROFILE_ENV_VAR = "DURIN_AGENT_BUILD_PROFILE"
JOBS_ENV_VAR = "DURIN_AGENT_JOBS"
CMAKE_ENV_VARS = ("DURIN_CMAKE_COMMAND", "DURIN_CMAKE_PATH")


@dataclass(frozen=True)
class BuildPaths:
    root: Path
    profile_file: Path
    preset_file: Path
    local_config_file: Path
    state_directory: Path
    lock_directory: Path

    @classmethod
    def from_repository(cls, repository: RepositoryContext) -> "BuildPaths":
        paths = repository.config.paths
        return cls(
            root=repository.root,
            profile_file=repository.resolve(paths.build_profiles),
            preset_file=repository.resolve(paths.cmake_presets),
            local_config_file=repository.resolve(paths.local_build_config),
            state_directory=repository.resolve(paths.state_directory),
            lock_directory=repository.resolve(paths.lock_directory),
        )


def default_build_paths() -> BuildPaths:
    """Call-time compatibility paths for direct service consumers."""
    return BuildPaths.from_repository(RepositoryContext.load())


from .requests import (
    ConcreteRequest,
    OutputOptions,
    RequestContext,
)



from .build_context import BuildContext


def host_name(system_name: str | None = None) -> str:
    from .selection import host_name as resolve_host_name

    return resolve_host_name(system_name)


def load_local_config(path: Path | None = None) -> LocalConfig:
    from .config_io import load_local_config as decode_local_config

    return decode_local_config(path or default_build_paths().local_config_file)



def load_profiles(path: Path | None = None) -> dict[str, BuildProfile]:
    from .config_io import load_profiles as decode_profiles

    return decode_profiles(path or default_build_paths().profile_file)



def load_configure_presets(path: Path | None = None) -> dict[str, ConfigurePreset]:
    from .config_io import load_configure_presets as decode_configure_presets

    return decode_configure_presets(path or default_build_paths().preset_file)



def select_profile(
    profiles: Mapping[str, BuildProfile],
    *,
    requested: str = "",
    configured: str = "",
    environment: Mapping[str, str] | None = None,
    current_host: str | None = None,
    profile_file: Path | None = None,
) -> BuildProfile:
    from .selection import select_profile as resolve_profile

    return resolve_profile(
        profiles,
        requested=requested,
        configured=configured,
        environment=environment,
        current_host=current_host,
        profile_file=profile_file or default_build_paths().profile_file,
    )


def select_preset(
    profile: BuildProfile,
    presets: Mapping[str, ConfigurePreset],
    *,
    requested: str = "",
    preset_file: Path | None = None,
) -> ConfigurePreset:
    from .selection import select_preset as resolve_preset

    return resolve_preset(
        profile,
        presets,
        requested=requested,
        preset_file=preset_file or default_build_paths().preset_file,
    )


def preset_cache_string(preset: ConfigurePreset, name: str, *, required: bool = True) -> str:
    from .selection import preset_cache_string as decode_cache_string

    return decode_cache_string(preset, name, required=required)


def preset_cache_bool(preset: ConfigurePreset, name: str) -> bool:
    from .selection import preset_cache_bool as decode_cache_bool

    return decode_cache_bool(preset, name)


def expand_preset_path(
    value: Any,
    preset: ConfigurePreset,
    *,
    root: Path | None = None,
) -> Path:
    from .selection import expand_preset_path as resolve_preset_path

    return resolve_preset_path(value, preset, root=root or default_build_paths().root)


def preset_build_directory(
    preset: ConfigurePreset,
    *,
    root: Path | None = None,
) -> Path:
    from .selection import preset_build_directory as resolve_build_directory

    return resolve_build_directory(preset, root=root or default_build_paths().root)


def preset_install_directory(
    preset: ConfigurePreset,
    *,
    root: Path | None = None,
) -> Path | None:
    from .selection import preset_install_directory as resolve_install_directory

    return resolve_install_directory(preset, root=root or default_build_paths().root)


def preset_output_configuration(preset: ConfigurePreset) -> str:
    from .selection import preset_output_configuration as resolve_output_configuration

    return resolve_output_configuration(preset)



def resolve_cmake_command(
    requested: str,
    configured: str,
    *,
    environment: Mapping[str, str] | None = None,
    local_config_file: Path | None = None,
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
    detected = find_command(command, environment)
    if not detected:
        raise BuildToolError(
            f'CMake command "{command}" was not found. Set --cmake, DURIN_CMAKE_COMMAND, '
            f'or cmake.command in "{local_config_file or default_build_paths().local_config_file}".'
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
