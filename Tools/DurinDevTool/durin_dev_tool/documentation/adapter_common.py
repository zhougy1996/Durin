from __future__ import annotations

import argparse
from pathlib import Path

from ..errors import DevToolError


def output_format(namespace: argparse.Namespace, *, interactive: bool) -> str:
    return namespace.output_format or ("terminal" if interactive else "markdown")


def document_under(value: str | None) -> Path | None:
    if value is None:
        return None
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise DevToolError("--under must be a repository-relative path")
    if not path.parts or path.parts[0] != "Documentation":
        raise DevToolError('--under must select a path inside "Documentation"')
    return path
