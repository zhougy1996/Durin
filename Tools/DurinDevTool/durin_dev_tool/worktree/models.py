"""Worktree domain values."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from ..errors import DevToolError


class WorktreeToolError(DevToolError):
    pass


@dataclass(frozen=True)
class Worktree:
    path: Path
    branch: str | None = None
    locked: bool = False


@dataclass(frozen=True)
class DetachedLink:
    path: Path
    target: Path
    kind: str
