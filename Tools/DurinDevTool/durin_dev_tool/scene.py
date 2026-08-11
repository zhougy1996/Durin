"""Repository-native scene authoring command orchestration."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
from typing import Callable, Sequence, TextIO

from .build.config import (
    BuildToolError,
    OutputMode,
    load_configure_presets,
    load_local_config,
    load_profiles,
    select_preset,
    select_profile,
)
from .build.output import BuildOutput
from .build.process import run_command
from .build.runtime import runtime_executable_path
from .errors import DevToolError


def _editor_executable(namespace: argparse.Namespace, repository_root: Path) -> Path:
    config = load_local_config()
    profile = select_profile(
        load_profiles(),
        requested=str(getattr(namespace, "profile", "") or ""),
        configured=config.default_build_profile,
    )
    preset = select_preset(
        profile,
        load_configure_presets(),
        requested=str(getattr(namespace, "preset", "") or ""),
    )
    return runtime_executable_path(profile, preset, root=repository_root)


def _command_arguments(namespace: argparse.Namespace, project: Path) -> list[str]:
    arguments = [
        f"--project={project}",
        "--hidden-window",
        "--startup-command=graybox-build",
        f"--startup-command-arg=--output={namespace.mounted_output}",
        "--startup-command-arg=--preset=open-arena",
    ]
    values = (
        ("width", "--width"),
        ("depth", "--depth"),
        ("floor_thickness", "--floor-thickness"),
        ("wall_height", "--wall-height"),
        ("wall_thickness", "--wall-thickness"),
    )
    for attribute, flag in values:
        value = float(getattr(namespace, attribute))
        if value < 0.1 or value > 10000.0:
            raise DevToolError(f"{flag} must be in the range 0.1 through 10000.")
        arguments.append(f"--startup-command-arg={flag}={value:g}")
    if namespace.ceiling:
        arguments.append("--startup-command-arg=--ceiling")
    return arguments


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    executable_resolver: Callable[[argparse.Namespace, Path], Path] = _editor_executable,
    command_runner: Callable[..., None] = run_command,
    **_kwargs: object,
) -> int:
    executable = executable_resolver(namespace, repository_root)
    if not executable.is_file():
        raise DevToolError(
            f'Editor executable was not found: "{executable}". '
            "Build it with 'DevTool build --target all'."
        )
    project = Path(namespace.project_path)
    if not project.is_absolute():
        project = repository_root / project
    project = project.resolve()
    if not project.is_file():
        raise DevToolError(f'Project descriptor was not found: "{project}".')
    output_path = str(namespace.mounted_output)
    if not output_path.startswith("/") or output_path.endswith("/"):
        raise DevToolError("--output must be a complete mounted Level path.")

    output_mode = OutputMode(str(getattr(namespace, "output_mode", None) or "auto"))
    output = BuildOutput(
        plain=bool(getattr(namespace, "plain", False)),
        output_mode=output_mode,
        stdout=stdout,
        stderr=stderr,
    )
    command: Sequence[str] = [
        str(executable),
        *_command_arguments(namespace, project),
    ]
    try:
        command_runner(
            command,
            environment=os.environ,
            output=output,
            colorize_log_levels=True,
            recovery_required_on_interrupt=False,
            interruption_message="Graybox build was cancelled.",
            timeout_seconds=int(namespace.timeout),
            wait_for_descendants=True,
            show_heartbeat=True,
        )
    except BuildToolError as error:
        print(str(error), file=stderr)
        if error.output_excerpt:
            print(error.output_excerpt, file=stderr)
        if error.log_path:
            print(f'Full output: "{error.log_path}"', file=stderr)
        return error.exit_code or 1
    print(f"Graybox Level created: {output_path}", file=stdout)
    return 0
