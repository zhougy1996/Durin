"""Unified registry handler for worktree commands."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Callable, TextIO

from ..context import CommandIO, RepositoryContext
from ..errors import DevToolError
from . import git as worktree_git
from . import terminal
from .models import WorktreeToolError
from . import transactions
from . import application


WorktreeAction = Callable[[argparse.Namespace, RepositoryContext, CommandIO], None]


def _open(namespace: argparse.Namespace, repository: RepositoryContext, io: CommandIO) -> None:
    terminal.open_worktree_terminals(repository, io, dry_run=namespace.dry_run)


def _list(namespace: argparse.Namespace, repository: RepositoryContext, io: CommandIO) -> None:
    del namespace
    terminal.display_worktrees(worktree_git.get_worktrees(repository, io), command_io=io)


def _add(namespace: argparse.Namespace, repository: RepositoryContext, io: CommandIO) -> None:
    if namespace.branch and namespace.detach:
        raise DevToolError("--branch and --detach cannot be used together")
    application.add_worktree(
        Path(namespace.path).expanduser().absolute(),
        branch=namespace.branch,
        detach=namespace.detach,
        commit_ish=namespace.commit_ish,
        source_value=namespace.source,
        link_type=namespace.link_type,
        repository=repository,
        command_io=io,
    )


def _prepare(namespace: argparse.Namespace, repository: RepositoryContext, io: CommandIO) -> None:
    target = Path(namespace.path).expanduser().absolute() if namespace.path else repository.root
    transactions.prepare_worktree(
        target,
        source_value=namespace.source,
        link_type=namespace.link_type,
        dry_run=namespace.dry_run,
        repository=repository,
        command_io=io,
    )


def _remove(namespace: argparse.Namespace, repository: RepositoryContext, io: CommandIO) -> None:
    transactions.remove_worktree(
        Path(namespace.path),
        force=namespace.force,
        dry_run=namespace.dry_run,
        repository=repository,
        command_io=io,
    )


_ACTIONS: dict[str, WorktreeAction] = {
    "open": _open,
    "list": _list,
    "add": _add,
    "prepare": _prepare,
    "remove": _remove,
}


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    repository_context: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
    **_: object,
) -> int:
    repository = repository_context or RepositoryContext.load(repository_root)
    io = command_io or CommandIO(stdout, stderr, plain=getattr(namespace, "plain", False))
    try:
        action = _ACTIONS.get(namespace.worktree_action)
        if action is None:
            raise WorktreeToolError(
                f"Unsupported worktree action: {namespace.worktree_action}"
            )
        action(namespace, repository, io)
    except WorktreeToolError:
        raise
    except (OSError, ValueError) as exc:
        raise DevToolError(str(exc)) from exc
    return 0
