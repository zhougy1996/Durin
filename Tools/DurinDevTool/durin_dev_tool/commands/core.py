from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from ..registry import CommandRegistry


def show_help(
    namespace: argparse.Namespace,
    *,
    registry: CommandRegistry,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    session_state: dict[str, object] | None = None,
) -> int:
    del repository_root, stderr, session_state
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
) -> int:
    del session_state
    from ..shell import run_shell

    return run_shell(
        registry=registry,
        repository_root=repository_root,
        stdout=stdout,
        stderr=stderr,
    )
