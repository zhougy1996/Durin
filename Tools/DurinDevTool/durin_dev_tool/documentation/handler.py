"""Documentation command-family router."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from ..errors import DevToolError
from . import document_adapter, lifecycle_adapter, task_adapter
from .archive import ArchiveError
from .lifecycle import PLAN_LIFECYCLE, ROADMAP_LIFECYCLE


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    session_state: dict[str, object] | None = None,
    **_: object,
) -> int:
    """Route one CLI request to its feature-owned documentation adapter."""
    interactive = session_state is not None
    try:
        if getattr(namespace, "document_action", ""):
            return document_adapter.run(
                namespace,
                repository_root=repository_root,
                interactive=interactive,
                stdout=stdout,
            )
        if getattr(namespace, "task_action", ""):
            return task_adapter.run(
                namespace,
                repository_root=repository_root,
                interactive=interactive,
                stdout=stdout,
                stderr=stderr,
            )
        if getattr(namespace, "plan_action", ""):
            return lifecycle_adapter.run(
                namespace,
                repository_root=repository_root,
                interactive=interactive,
                stdout=stdout,
                stderr=stderr,
                config=PLAN_LIFECYCLE,
            )
        if getattr(namespace, "roadmap_action", ""):
            return lifecycle_adapter.run(
                namespace,
                repository_root=repository_root,
                interactive=interactive,
                stdout=stdout,
                stderr=stderr,
                config=ROADMAP_LIFECYCLE,
            )
    except ArchiveError:
        raise
    except (OSError, ValueError) as exc:
        raise DevToolError(str(exc)) from exc
    raise DevToolError("a documentation command is required")
