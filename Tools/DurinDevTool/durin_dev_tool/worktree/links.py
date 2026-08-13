"""Directory-link inspection, creation, containment, detach, and restoration."""

from __future__ import annotations

import os
import stat
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from ..context import CommandIO, RepositoryContext
from .git import is_windows, normalized_path, same_path
from .models import DetachedLink, Worktree, WorktreeToolError


@dataclass(frozen=True)
class SharedDirectorySpec:
    relative_path: Path
    label: str
    preserve_existing: bool = False


def shared_directory_specs(repository: RepositoryContext) -> tuple[SharedDirectorySpec, ...]:
    paths = repository.config.worktrees
    return (
        SharedDirectorySpec(paths.agent_directory, "Agent", True),
        SharedDirectorySpec(paths.vscode_directory, "VS Code", True),
        SharedDirectorySpec(paths.python_environment, "Python environment"),
        SharedDirectorySpec(paths.external_directory, "External dependencies"),
    )


def is_reparse_point(path: Path) -> bool:
    if not is_windows():
        return path.is_symlink()
    try:
        return bool(path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    except (AttributeError, OSError):
        return path.is_symlink()


def is_link_like(path: Path) -> bool:
    return path.is_symlink() or is_reparse_point(path)


def choose_link_type(requested: str) -> str:
    if requested == "auto":
        return "junction" if is_windows() else "symlink"
    return requested


def is_empty_directory(path: Path) -> bool:
    return path.is_dir() and not any(path.iterdir())


def remove_link_or_empty_directory(path: Path, *, dry_run: bool, command_io: CommandIO) -> None:
    if dry_run:
        command_io.out(f'[dry-run] remove "{path}"')
        return
    if is_link_like(path) or is_empty_directory(path):
        path.rmdir()
        return
    raise WorktreeToolError(f'Refusing to remove non-empty real directory: "{path}"')


def create_directory_link(
    source: Path,
    target: Path,
    *,
    link_type: str,
    dry_run: bool,
    command_io: CommandIO,
) -> None:
    if link_type == "junction":
        if not is_windows():
            raise WorktreeToolError("Directory junctions are only supported on Windows.")
        command = ["cmd.exe", "/d", "/c", "mklink", "/J", str(target), str(source)]
        if dry_run:
            command_io.out(f"[dry-run] {' '.join(command)}")
            return
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "").strip()
            raise WorktreeToolError(f'Could not create junction "{target}" -> "{source}": {detail}')
        return
    if link_type == "symlink":
        if dry_run:
            command_io.out(f'[dry-run] symlink "{target}" -> "{source}"')
            return
        os.symlink(source, target, target_is_directory=True)
        return
    raise WorktreeToolError(f"Unsupported link type: {link_type}")


def linked_to(path: Path, expected_target: Path) -> bool:
    return path.exists() and is_link_like(path) and same_path(
        path.resolve(strict=False), expected_target.resolve(strict=False)
    )


def prepare_directory_link(
    source: Path,
    target: Path,
    *,
    label: str,
    link_type: str,
    dry_run: bool,
    command_io: CommandIO,
) -> None:
    source = source.expanduser().resolve()
    target = target.expanduser().absolute()
    if not source.is_dir():
        raise WorktreeToolError(f'Source {label} directory does not exist: "{source}"')
    if linked_to(target, source):
        command_io.out(f'{label} is already linked to "{source}".')
        return
    if target.exists() or is_link_like(target):
        if is_link_like(target) or is_empty_directory(target):
            remove_link_or_empty_directory(target, dry_run=dry_run, command_io=command_io)
        else:
            raise WorktreeToolError(
                f'Target {label} already exists and is not an empty directory or link: "{target}"\n'
                "Move it aside manually if you really want to replace it."
            )
    if dry_run:
        command_io.out(f'[dry-run] link {label}: "{target}" -> "{source}"')
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
    create_directory_link(source, target, link_type=link_type, dry_run=dry_run, command_io=command_io)
    command_io.out(f'Linked {label}: "{target}" -> "{source}"')


def prepare_preserved_directory_link(
    source_root: Path,
    target_root: Path,
    *,
    relative_path: Path,
    preservation_label: str,
    link_type: str,
    dry_run: bool,
    command_io: CommandIO,
) -> None:
    source = (source_root / relative_path).resolve()
    target = (target_root / relative_path).absolute()
    backup = target.with_name(f"{target.name}.pre-link-backup")
    if not source.is_dir():
        raise WorktreeToolError(f'Source {relative_path.as_posix()} directory does not exist: "{source}"')
    if target.is_dir() and not is_link_like(target) and not is_empty_directory(target):
        if backup.exists() or is_link_like(backup):
            raise WorktreeToolError(
                f'Cannot preserve the existing {preservation_label} directory because the backup path exists: "{backup}"'
            )
        command_io.out(f'Preserving existing {relative_path.as_posix()}: "{target}" -> "{backup}"')
        if dry_run:
            command_io.out(f'[dry-run] move "{target}" -> "{backup}"')
            command_io.out(f'[dry-run] link {relative_path.as_posix()}: "{target}" -> "{source}"')
            create_directory_link(source, target, link_type=link_type, dry_run=True, command_io=command_io)
            return
        target.rename(backup)
    prepare_directory_link(
        source,
        target,
        label=relative_path.as_posix(),
        link_type=link_type,
        dry_run=dry_run,
        command_io=command_io,
    )


def prepare_shared_directory_link(
    source_root: Path,
    target_root: Path,
    spec: SharedDirectorySpec,
    *,
    link_type: str,
    dry_run: bool,
    command_io: CommandIO,
) -> None:
    if spec.preserve_existing:
        prepare_preserved_directory_link(
            source_root,
            target_root,
            relative_path=spec.relative_path,
            preservation_label=spec.label,
            link_type=link_type,
            dry_run=dry_run,
            command_io=command_io,
        )
    else:
        prepare_directory_link(
            source_root / spec.relative_path,
            target_root / spec.relative_path,
            label=spec.label,
            link_type=link_type,
            dry_run=dry_run,
            command_io=command_io,
        )


def require_link_like_status(path: Path) -> bool:
    try:
        status = path.lstat()
    except OSError as exc:
        raise WorktreeToolError(f'Could not inspect possible directory link "{path}": {exc}') from exc
    if stat.S_ISLNK(status.st_mode):
        return True
    return bool(is_windows() and getattr(status, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT)


def directory_links_under(root: Path) -> list[Path]:
    links: list[Path] = []

    def raise_walk_error(exc: OSError) -> None:
        raise WorktreeToolError(f"Could not inspect worktree directory links: {exc}") from exc

    for current_root, directory_names, _ in os.walk(root, topdown=True, onerror=raise_walk_error, followlinks=False):
        current = Path(current_root)
        retained: list[str] = []
        for directory_name in directory_names:
            path = current / directory_name
            if require_link_like_status(path):
                links.append(path)
            else:
                retained.append(directory_name)
        directory_names[:] = retained
    return links


def validate_directory_links(worktree: Worktree, repository: RepositoryContext) -> list[Path]:
    expected = [worktree.path / relative for relative in repository.config.worktrees.shared_directories]
    expected_keys = {normalized_path(path) for path in expected}
    discovered = directory_links_under(worktree.path)
    unexpected = [path for path in discovered if normalized_path(path) not in expected_keys]
    if unexpected:
        formatted = "\n".join(f'  "{path}"' for path in unexpected)
        raise WorktreeToolError(
            "Refusing to remove a worktree containing unexpected directory links:\n"
            f"{formatted}\nRemove or review these links explicitly first."
        )
    return [path for path in expected if is_link_like(path)]


def detach_link(path: Path) -> DetachedLink:
    target = path.resolve(strict=False)
    kind = "symlink" if path.is_symlink() else "junction"
    path.rmdir()
    return DetachedLink(path, target, kind)


def restore_link(link: DetachedLink) -> None:
    if link.kind == "junction":
        result = subprocess.run(
            ["cmd.exe", "/d", "/c", "mklink", "/J", str(link.path), str(link.target)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "").strip()
            raise WorktreeToolError(f'Could not restore junction "{link.path}" -> "{link.target}": {detail}')
        return
    os.symlink(link.target, link.path, target_is_directory=True)
