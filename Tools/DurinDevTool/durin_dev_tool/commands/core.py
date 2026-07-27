from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from ..registry import CommandRegistry


def show_help(
    _namespace: argparse.Namespace,
    *,
    registry: CommandRegistry,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
) -> int:
    del repository_root, stderr
    print(registry.format_help(), file=stdout)
    return 0


def open_shell(
    _namespace: argparse.Namespace,
    *,
    registry: CommandRegistry,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
) -> int:
    from ..shell import run_shell

    return run_shell(
        registry=registry,
        repository_root=repository_root,
        stdout=stdout,
        stderr=stderr,
    )
