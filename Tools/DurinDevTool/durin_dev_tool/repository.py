from __future__ import annotations

from pathlib import Path

from .errors import DevToolError


REPOSITORY_MARKERS = ("CMakeLists.txt", "Engine", "Tools")


def is_repository_root(path: Path) -> bool:
    resolved = path.resolve()
    return (
        (resolved / ".git").exists()
        and (resolved / REPOSITORY_MARKERS[0]).is_file()
        and all((resolved / marker).is_dir() for marker in REPOSITORY_MARKERS[1:])
    )


def find_repository_root(start: Path) -> Path:
    candidate = start.resolve()
    if candidate.is_file():
        candidate = candidate.parent
    for path in (candidate, *candidate.parents):
        if is_repository_root(path):
            return path
    raise DevToolError(
        f'Could not find the Durin repository root from "{candidate}". '
        "Run the repository-owned DevTool launcher."
    )


def discover_repository_root() -> Path:
    return find_repository_root(Path(__file__))
