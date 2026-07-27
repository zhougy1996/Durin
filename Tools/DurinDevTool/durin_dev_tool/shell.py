from __future__ import annotations

import argparse
import shlex
from pathlib import Path
from typing import Callable, TextIO

from .errors import DevToolError
from .registry import CommandRegistry


def run_shell(
    *,
    registry: CommandRegistry,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    input_func: Callable[[str], str] = input,
) -> int:
    print("Durin Developer Tool shell", file=stdout)
    print("Type help for available commands.", file=stdout)
    while True:
        try:
            line = input_func("DurinDevTool> ").strip()
        except EOFError:
            print("", file=stdout)
            return 0
        except KeyboardInterrupt:
            print("\nUse exit to leave the shell.", file=stderr)
            continue
        if not line:
            continue
        try:
            parts = shlex.split(line, posix=False)
        except ValueError as exc:
            print(f"Error: invalid command: {exc}", file=stderr)
            continue
        command = parts[0].lower()
        if command in {"exit", "quit"}:
            return 0
        if command in {"?", "/?", "/help"}:
            parts = ["help"]
        try:
            spec, namespace = registry.parse(parts)
            registry.execute(
                spec,
                namespace,
                repository_root=repository_root,
                stdout=stdout,
                stderr=stderr,
            )
        except DevToolError as exc:
            print(f"Error: {exc}", file=stderr)
        except SystemExit:
            continue
