from __future__ import annotations

import sys
from pathlib import Path
from typing import Sequence, TextIO

from .context import CommandIO, RepositoryContext
from .configuration import RepositoryConfigError
from .errors import DevToolError
from .registry import CommandRegistry


def run(
    arguments: Sequence[str],
    *,
    registry: CommandRegistry | None = None,
    repository_root: Path | None = None,
    stdout: TextIO | None = None,
    stderr: TextIO | None = None,
) -> int:
    output = stdout or sys.stdout
    errors = stderr or sys.stderr
    try:
        repository = RepositoryContext.load(repository_root)
    except RepositoryConfigError:
        if repository_root is None:
            raise
        repository = RepositoryContext.load().at_root(repository_root)
    active_registry = registry or CommandRegistry()
    spec, namespace = active_registry.parse(arguments)
    command_io = CommandIO(
        output,
        errors,
        plain=bool(getattr(namespace, "plain", False)),
    )
    return active_registry.execute(
        spec,
        namespace,
        repository_context=repository,
        command_io=command_io,
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
