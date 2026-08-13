"""Git subprocess and porcelain behavior for worktrees."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Sequence

from ..context import CommandIO, RepositoryContext
from .models import Worktree, WorktreeToolError


def _repository(repository: RepositoryContext | None) -> RepositoryContext:
    return repository or RepositoryContext.load()


def _command_io(command_io: CommandIO | None) -> CommandIO:
    return command_io or CommandIO.system()


def is_windows() -> bool:
    return os.name == "nt"


def normalized_path(path: Path) -> str:
    return os.path.normcase(os.path.abspath(path))


def same_path(left: Path, right: Path) -> bool:
    try:
        return os.path.samefile(left, right)
    except OSError:
        return normalized_path(left) == normalized_path(right)


def git_command(
    arguments: Sequence[str],
    *,
    repository: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
    cwd: Path | None = None,
    capture_output: bool = True,
) -> subprocess.CompletedProcess[str]:
    repository = _repository(repository)
    command_io = _command_io(command_io)
    cwd = cwd or repository.root
    safe_paths = [repository.root]
    if not same_path(cwd, repository.root):
        safe_paths.append(cwd)
    command = ["git.exe" if is_windows() else "git"]
    for safe_path in safe_paths:
        safe_path_text = str(safe_path).replace(os.sep, "/")
        command.extend(["-c", f"safe.directory={safe_path_text}"])
    command.extend(["-c", "core.quotePath=false", "-C", str(cwd), *arguments])
    try:
        return subprocess.run(
            command,
            check=False,
            capture_output=capture_output,
            stdout=None if capture_output else command_io.stdout,
            stderr=None if capture_output else command_io.stderr,
            text=True,
        )
    except OSError as exc:
        raise WorktreeToolError(f"Could not run Git: {exc}") from exc


def require_git_success(result: subprocess.CompletedProcess[str], operation: str) -> None:
    if result.returncode == 0:
        return
    detail = (result.stderr or result.stdout or "").strip()
    suffix = f"\n{detail}" if detail else ""
    raise WorktreeToolError(f"{operation} failed with exit code {result.returncode}.{suffix}")


def parse_worktrees(output: str) -> list[Worktree]:
    worktrees: list[Worktree] = []
    current_path: Path | None = None
    current_branch: str | None = None
    current_locked = False

    def append_current() -> None:
        if current_path is not None:
            worktrees.append(Worktree(current_path, current_branch, current_locked))

    for line in output.splitlines():
        if line.startswith("worktree "):
            append_current()
            current_path = Path(line.removeprefix("worktree "))
            current_branch = None
            current_locked = False
        elif line.startswith("branch refs/heads/"):
            current_branch = line.removeprefix("branch refs/heads/")
        elif line == "locked" or line.startswith("locked "):
            current_locked = True
    append_current()
    return worktrees


def get_worktrees(
    repository: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
) -> list[Worktree]:
    repository = _repository(repository)
    command_io = _command_io(command_io)
    result = git_command(
        ["worktree", "list", "--porcelain"],
        repository=repository,
        command_io=command_io,
    )
    require_git_success(result, "Enumerating Git worktrees")
    worktrees = parse_worktrees(result.stdout)
    if not worktrees:
        raise WorktreeToolError("Git did not report any worktrees.")
    return worktrees


def require_clean_worktree(
    worktree: Worktree,
    *,
    force: bool,
    repository: RepositoryContext,
    command_io: CommandIO | None = None,
) -> None:
    if force:
        return
    command_io = _command_io(command_io)
    result = git_command(
        ["status", "--porcelain", "--untracked-files=all"],
        repository=repository,
        command_io=command_io,
        cwd=worktree.path,
    )
    require_git_success(result, "Checking worktree status")
    if result.stdout.strip():
        raise WorktreeToolError(
            "Worktree contains modified or untracked files. Commit or remove them, "
            "or pass --force to discard them."
        )
