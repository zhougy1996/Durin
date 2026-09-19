"""Windows Terminal layout and launch behavior for worktrees."""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Sequence

from ..build.config_io import load_local_config
from ..build.errors import BuildToolError
from ..context import CommandIO, RepositoryContext
from .git import get_worktrees
from .models import Worktree, WorktreeToolError


def display_worktrees(worktrees: Sequence[Worktree], *, command_io: CommandIO) -> None:
    command_io.out(f"Durin worktrees ({len(worktrees)}):")
    for worktree in worktrees:
        branch = worktree.branch or "detached"
        locked = " [locked]" if worktree.locked else ""
        command_io.out(f"  [{branch}] {worktree.path}{locked}")


def ordered_worktrees(worktrees: Sequence[Worktree]) -> list[Worktree]:
    return [
        *(worktree for worktree in worktrees if worktree.branch == "main"),
        *(worktree for worktree in worktrees if worktree.branch == "dev"),
        *(worktree for worktree in worktrees if worktree.branch not in {"main", "dev"}),
    ]


def environment_arguments(
    worktree: Path,
    repository: RepositoryContext,
    command_io: CommandIO,
) -> list[str]:
    config_path = worktree / repository.config.paths.local_build_config
    if not config_path.is_file():
        command_io.error(
            f"WARNING: Agent config is missing for worktree '{worktree}'. "
            "Opening it without a configured environment; "
            "run 'DevTool worktree prepare' there."
        )
        return []
    try:
        config = load_local_config(config_path)
    except (BuildToolError, OSError) as exc:
        raise WorktreeToolError(f'Could not read Agent config "{config_path}": {exc}') from exc
    setup_script = config.environment_setup.script
    if not setup_script:
        return []
    setup_path = Path(setup_script)
    if not setup_path.is_file():
        raise WorktreeToolError(f'Environment setup script does not exist: "{setup_path}"')
    return [str(setup_path), *config.environment_setup.arguments]


def add_terminal_tab_arguments(
    arguments: list[str],
    *,
    worktree: Path,
    environment: Sequence[str],
) -> None:
    arguments.extend([
        "new-tab",
        "--startingDirectory", str(worktree), "--title", worktree.name,
        "cmd.exe", "/k", *environment,
    ])


def terminal_arguments(
    worktrees: Sequence[Worktree],
    repository: RepositoryContext,
    command_io: CommandIO,
) -> list[str]:
    arguments = ["-w", "new", "--maximized"]
    environments = {
        worktree.path: environment_arguments(worktree.path, repository, command_io)
        for worktree in worktrees
    }
    for index, worktree in enumerate(worktrees):
        if index:
            arguments.append(";")
        add_terminal_tab_arguments(
            arguments, worktree=worktree.path, environment=environments[worktree.path],
        )
    return arguments


def open_worktree_terminals(
    repository: RepositoryContext,
    command_io: CommandIO,
    *,
    dry_run: bool,
) -> None:
    worktrees = ordered_worktrees(get_worktrees(repository, command_io))
    display_worktrees(worktrees, command_io=command_io)
    arguments = terminal_arguments(worktrees, repository, command_io)
    command_io.out("Layout: maximized window with one tab per worktree.")
    if dry_run:
        command_io.out("Dry run complete; Windows Terminal was not opened.")
        return
    try:
        result = subprocess.run(["wt.exe", *arguments], check=False)
    except OSError as exc:
        raise WorktreeToolError(
            "wt.exe was not found. Install Windows Terminal or enable its app execution alias."
        ) from exc
    if result.returncode != 0:
        raise WorktreeToolError(f"Windows Terminal exited with code {result.returncode}.")
