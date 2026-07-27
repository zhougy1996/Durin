"""Unified registry handler for worktree commands."""

from __future__ import annotations

import argparse
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import TextIO

from ..errors import DevToolError
from . import services


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    **_: object,
) -> int:
    if (
        namespace.worktree_action == "add"
        and namespace.branch
        and namespace.detach
    ):
        raise DevToolError("--branch and --detach cannot be used together")
    try:
        with (
            services.repository_paths(repository_root),
            redirect_stdout(stdout),
            redirect_stderr(stderr),
        ):
            if namespace.worktree_action == "open":
                services.open_worktree_terminals(dry_run=namespace.dry_run)
            elif namespace.worktree_action == "list":
                services.display_worktrees(services.get_worktrees())
            elif namespace.worktree_action == "add":
                services.add_worktree(namespace)
            elif namespace.worktree_action == "prepare":
                services.prepare_worktree_command(namespace)
            elif namespace.worktree_action == "remove":
                services.remove_worktree(namespace)
            else:
                raise services.WorktreeToolError(
                    f"Unsupported worktree action: {namespace.worktree_action}"
                )
    except (services.WorktreeToolError, OSError, ValueError) as exc:
        raise DevToolError(str(exc)) from exc
    return 0
