"""Repository-native scene authoring command orchestration."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Callable, TextIO

from .build.config import BuildToolError, OutputMode
from .build.output import BuildOutput
from .build.process import run_command
from .context import RepositoryContext
from .errors import DevToolError
from .runtime_program import (
    ExecutableDescription,
    RuntimeProcessPolicy,
    invoke_runtime_program,
    locate_executable,
    resolve_project,
    select_runtime,
)


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
    repository_context: RepositoryContext | None = None,
    stdout: TextIO,
    stderr: TextIO,
    executable_resolver: Callable[[argparse.Namespace, Path], Path] | None = None,
    command_runner: Callable[..., None] = run_command,
    **_kwargs: object,
) -> int:
    repository = repository_context or RepositoryContext.load(repository_root)
    selection = select_runtime(
        repository,
        profile_name=str(getattr(namespace, "profile", "") or ""),
        preset_name=str(getattr(namespace, "preset", "") or ""),
    )
    executable = (
        executable_resolver(namespace, repository.root)
        if executable_resolver
        else locate_executable(selection, ExecutableDescription("Editor", "all"))
    )
    project = resolve_project(repository, Path(namespace.project_path))
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
    try:
        invoke_runtime_program(
            selection,
            ExecutableDescription("Editor", "all"),
            _command_arguments(namespace, project),
            output=output,
            policy=RuntimeProcessPolicy(
                interruption_message="Graybox build was cancelled.",
                timeout_seconds=int(namespace.timeout),
                wait_for_descendants=True,
                show_heartbeat=True,
                colorize_log_levels=True,
            ),
            executable_override=executable,
            command_runner=command_runner,
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
