from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from ..context import CommandIO, RepositoryContext
from ..registry import CommandRegistry


def show_help(
    namespace: argparse.Namespace,
    *,
    registry: CommandRegistry,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    session_state: dict[str, object] | None = None,
    repository_context: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
) -> int:
    del repository_root, stderr, session_state, repository_context, command_io
    print(registry.format_command_help(namespace.command_path), file=stdout)
    return 0


def open_shell(
    _namespace: argparse.Namespace,
    *,
    registry: CommandRegistry,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    session_state: dict[str, object] | None = None,
    repository_context: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
) -> int:
    del session_state, command_io
    from ..shell import run_shell

    return run_shell(
        registry=registry,
        repository_root=repository_root,
        repository_context=repository_context,
        stdout=stdout,
        stderr=stderr,
    )
