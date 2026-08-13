"""Worktree command application services."""

from __future__ import annotations

from pathlib import Path

from ..context import CommandIO, RepositoryContext
from . import git, transactions
from .models import WorktreeToolError


def add_worktree(
    target: Path,
    *,
    branch: str | None,
    detach: bool,
    commit_ish: str | None,
    source_value: str | None,
    link_type: str,
    repository: RepositoryContext,
    command_io: CommandIO,
) -> None:
    command = ["worktree", "add"]
    if branch:
        command.extend(["-b", branch])
    if detach:
        command.append("--detach")
    command.append(str(target))
    if commit_ish:
        command.append(commit_ish)
    result = git.git_command(
        command,
        repository=repository,
        command_io=command_io,
        capture_output=False,
    )
    git.require_git_success(result, "Adding Git worktree")
    try:
        transactions.prepare_worktree(
            target,
            source_value=source_value,
            link_type=link_type,
            dry_run=False,
            repository=repository,
            command_io=command_io,
        )
    except WorktreeToolError as exc:
        raise WorktreeToolError(
            f'Worktree was created at "{target}", but preparation failed.\n{exc}\n'
            f'Fix the reported problem and run DevTool worktree prepare "{target}".'
        ) from exc
    command_io.out(f'Created and prepared worktree: "{target}"')
