"""Shared selection, location, project, and process policy for runtime programs."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, Sequence

from .build.config_io import load_configure_presets, load_local_config, load_profiles
from .build.errors import BuildToolError
from .build.models import BuildProfile, ConfigurePreset
from .build.selection import select_preset, select_profile
from .build.settings import BuildPaths
from .build.locations import resolve_location
from .build.output import BuildOutput
from .build.process import run_command
from .build.runtime import runtime_executable_path
from .context import RepositoryContext
from .errors import DevToolError


@dataclass(frozen=True)
class RuntimeSelection:
    repository: RepositoryContext
    profile: BuildProfile
    preset: ConfigurePreset

    @property
    def paths(self) -> BuildPaths:
        return BuildPaths.from_repository(self.repository)


@dataclass(frozen=True)
class ExecutableDescription:
    label: str
    build_target: str
    file_name: str | None = None


@dataclass(frozen=True)
class RuntimeProcessPolicy:
    interruption_message: str
    timeout_seconds: int | None = None
    wait_for_descendants: bool = False
    show_heartbeat: bool = False
    colorize_log_levels: bool = False
    capture_output: bool = False


def select_runtime(
    repository: RepositoryContext,
    *,
    profile_name: str = "",
    preset_name: str = "",
) -> RuntimeSelection:
    paths = BuildPaths.from_repository(repository)
    config = load_local_config(paths.local_config_file)
    profile = select_profile(
        load_profiles(paths.profile_file),
        requested=profile_name,
        configured=config.default_build_profile,
        profile_file=paths.profile_file,
    )
    preset = select_preset(
        profile,
        load_configure_presets(paths.preset_file),
        requested=preset_name,
        preset_file=paths.preset_file,
    )
    return RuntimeSelection(repository, profile, preset)


def resolve_project(repository: RepositoryContext, value: Path) -> Path:
    project = value if value.is_absolute() else repository.root / value
    project = project.resolve()
    if project.suffix.casefold() != ".dproject":
        raise DevToolError(f'Project descriptor must use the .dproject extension: "{project}".')
    if not project.is_file():
        raise DevToolError(f'Project descriptor was not found: "{project}".')
    return project


def locate_executable(
    selection: RuntimeSelection,
    description: ExecutableDescription,
) -> Path:
    if description.file_name is None:
        executable = runtime_executable_path(
            selection.profile,
            selection.preset,
            root=selection.repository.root,
            repository=selection.repository,
        )
    else:
        directory = resolve_location(
            "runtime",
            profile=selection.profile,
            preset=selection.preset,
            root=selection.repository.root,
            runtime_binaries_directory=selection.repository.config.paths.runtime_binaries_directory,
            state_directory=selection.repository.config.paths.state_directory,
        ).path
        executable = directory / f"{description.file_name}{selection.profile.test_executable_suffix}"
    if not executable.is_file():
        raise DevToolError(
            f'{description.label} executable was not found: "{executable}". '
            f"Build it with 'DevTool build --target {description.build_target}'."
        )
    return executable


def invoke_runtime_program(
    selection: RuntimeSelection,
    description: ExecutableDescription,
    arguments: Sequence[str],
    *,
    output: BuildOutput,
    policy: RuntimeProcessPolicy,
    environment: Mapping[str, str] | None = None,
    executable_override: Path | None = None,
    command_runner: Callable[..., str] | None = None,
) -> str:
    executable = executable_override or locate_executable(selection, description)
    command_runner = command_runner or run_command
    try:
        return command_runner(
            [str(executable), *arguments],
            environment=environment or os.environ,
            output=output,
            colorize_log_levels=policy.colorize_log_levels,
            recovery_required_on_interrupt=False,
            interruption_message=policy.interruption_message,
            timeout_seconds=policy.timeout_seconds,
            wait_for_descendants=policy.wait_for_descendants,
            show_heartbeat=policy.show_heartbeat,
            cwd=selection.repository.root,
            state_directory=selection.paths.state_directory,
            capture_output=policy.capture_output,
        )
    except BuildToolError:
        raise
