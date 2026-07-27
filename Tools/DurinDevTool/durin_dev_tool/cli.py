from __future__ import annotations

import sys
from pathlib import Path
from typing import Sequence, TextIO

from .errors import DevToolError
from .registry import CommandRegistry
from .repository import discover_repository_root


def run(
    arguments: Sequence[str],
    *,
    registry: CommandRegistry | None = None,
    repository_root: Path | None = None,
    stdout: TextIO | None = None,
    stderr: TextIO | None = None,
) -> int:
    active_registry = registry or CommandRegistry()
    output = stdout or sys.stdout
    errors = stderr or sys.stderr
    root = repository_root or discover_repository_root()
    spec, namespace = active_registry.parse(arguments)
    return active_registry.execute(
        spec,
        namespace,
        repository_root=root,
        stdout=output,
        stderr=errors,
    )


def main(arguments: Sequence[str] | None = None) -> int:
    try:
        return run(sys.argv[1:] if arguments is None else arguments)
    except DevToolError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Cancelled.", file=sys.stderr)
        return 130
    except OSError as exc:
        print(f"Error: operating system failure: {exc}", file=sys.stderr)
        return 1
