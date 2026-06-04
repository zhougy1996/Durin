#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import stat
import subprocess
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]

ENV_SOURCE_NAMES = ("DURIN_WORKTREE_SOURCE", "DURIN_EXTERNAL_SOURCE", "DURIN_EXTERNAL_ROOT")
DEFAULT_SOURCE_WORKTREE_NAME = "Durin"
AGENTS_LOCAL_NAME = "AGENTS_LOCAL.md"
VENV_DIR_NAME = ".venv"


class PrepareWorktreeError(RuntimeError):
    pass


def is_windows() -> bool:
    return os.name == "nt"


def normalize_path(path: Path) -> Path:
    return path.expanduser().resolve()


def read_gitdir_pointer(repo_root: Path) -> Path | None:
    git_entry = repo_root / ".git"
    if git_entry.is_dir():
        return git_entry

    if not git_entry.is_file():
        return None

    try:
        content = git_entry.read_text(encoding="utf-8").strip()
    except OSError:
        return None

    prefix = "gitdir:"
    if not content.lower().startswith(prefix):
        return None

    gitdir_value = content[len(prefix) :].strip()
    if not gitdir_value:
        return None

    return normalize_path(repo_root / gitdir_value)


def infer_main_worktree_root(repo_root: Path) -> Path | None:
    git_dir = read_gitdir_pointer(repo_root)
    if git_dir is None:
        return None

    worktrees_dir = git_dir.parent
    if worktrees_dir.name != "worktrees":
        return None

    commondir_file = git_dir / "commondir"
    if not commondir_file.is_file():
        return None

    try:
        commondir_value = commondir_file.read_text(encoding="utf-8").strip()
    except OSError:
        return None

    if not commondir_value:
        return None

    common_git_dir = normalize_path(git_dir / commondir_value)
    return common_git_dir.parent


def find_source_argument(args: argparse.Namespace) -> str:
    if args.source:
        return args.source

    for env_name in ENV_SOURCE_NAMES:
        env_value = os.environ.get(env_name)
        if env_value:
            return env_value

    main_worktree_root = infer_main_worktree_root(REPO_ROOT)
    if main_worktree_root is not None:
        return str(main_worktree_root)

    return str(REPO_ROOT.parent / DEFAULT_SOURCE_WORKTREE_NAME)


def resolve_source_worktree(path_value: str) -> Path:
    path = normalize_path(Path(path_value))

    if path.name.lower() == "external":
        candidate = path.parent.parent
    else:
        candidate = path

    external_dir = candidate / "Engine" / "External"
    if external_dir.exists():
        return normalize_path(candidate)

    raise PrepareWorktreeError(
        f"Could not find an Engine/External directory under \"{path}\".\n"
        "Pass either a Durin worktree root or an Engine/External directory."
    )


def resolve_source(args: argparse.Namespace) -> Path:
    source_worktree = resolve_source_worktree(find_source_argument(args))

    if same_path(source_worktree, REPO_ROOT):
        raise PrepareWorktreeError(f"Source worktree is the same as this worktree: \"{source_worktree}\".")

    return source_worktree


def is_reparse_point(path: Path) -> bool:
    if not is_windows():
        return path.is_symlink()

    try:
        return bool(path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    except (AttributeError, OSError):
        return path.is_symlink()


def is_link_like(path: Path) -> bool:
    return path.is_symlink() or is_reparse_point(path)


def same_path(left: Path, right: Path) -> bool:
    try:
        return os.path.samefile(left, right)
    except OSError:
        left_text = str(left.resolve(strict=False))
        right_text = str(right.resolve(strict=False))
        if is_windows():
            return left_text.lower() == right_text.lower()
        return left_text == right_text


def linked_to(path: Path, expected_target: Path) -> bool:
    if not path.exists() or not is_link_like(path):
        return False

    resolved_path = path.resolve(strict=False)
    resolved_expected = expected_target.resolve(strict=False)
    return same_path(resolved_path, resolved_expected)


def is_empty_directory(path: Path) -> bool:
    return path.is_dir() and not any(path.iterdir())


def remove_link_or_empty_dir(path: Path, *, dry_run: bool) -> None:
    if dry_run:
        print(f"[dry-run] remove \"{path}\"")
        return

    if is_link_like(path) or is_empty_directory(path):
        path.rmdir()
        return

    raise PrepareWorktreeError(f"Refusing to remove non-empty real directory: \"{path}\"")


def remove_file_or_link(path: Path, *, dry_run: bool) -> None:
    if dry_run:
        print(f"[dry-run] remove \"{path}\"")
        return

    if path.is_dir() and not is_link_like(path):
        raise PrepareWorktreeError(f"Refusing to remove directory for file link target: \"{path}\"")

    path.unlink()


def create_junction(source: Path, target: Path, *, dry_run: bool) -> None:
    command = ["cmd", "/c", "mklink", "/J", str(target), str(source)]
    if dry_run:
        print(f"[dry-run] {' '.join(command)}")
        return

    result = subprocess.run(command)
    if result.returncode != 0:
        raise PrepareWorktreeError(
            f"Failed to create junction \"{target}\" -> \"{source}\" "
            f"(exit code {result.returncode})."
        )


def create_symlink(source: Path, target: Path, *, target_is_directory: bool, dry_run: bool) -> None:
    if dry_run:
        print(f"[dry-run] symlink \"{target}\" -> \"{source}\"")
        return

    os.symlink(source, target, target_is_directory=target_is_directory)


def choose_link_type(requested: str) -> str:
    if requested == "auto":
        return "junction" if is_windows() else "symlink"
    return requested


def create_link(source: Path, target: Path, *, link_type: str, target_is_directory: bool, dry_run: bool) -> None:
    if link_type == "junction":
        if not is_windows():
            raise PrepareWorktreeError("Directory junctions are only supported on Windows.")
        if not target_is_directory:
            raise PrepareWorktreeError("Directory junctions are only supported for directories.")
        create_junction(source, target, dry_run=dry_run)
        return

    if link_type == "symlink":
        create_symlink(source, target, target_is_directory=target_is_directory, dry_run=dry_run)
        return

    raise PrepareWorktreeError(f"Unsupported link type: {link_type}")


def prepare_directory_link(
    source_dir: Path,
    target_dir: Path,
    *,
    label: str,
    link_type: str,
    dry_run: bool,
) -> None:
    source_dir = normalize_path(source_dir)
    target_dir = normalize_path(target_dir)

    if not source_dir.is_dir():
        raise PrepareWorktreeError(f"Source {label} directory does not exist: \"{source_dir}\"")

    if linked_to(target_dir, source_dir):
        print(f"{label} is already linked to \"{source_dir}\".")
        return

    if target_dir.exists() or is_link_like(target_dir):
        if is_link_like(target_dir) or is_empty_directory(target_dir):
            remove_link_or_empty_dir(target_dir, dry_run=dry_run)
        else:
            raise PrepareWorktreeError(
                f"Target {label} already exists and is not an empty directory or link: \"{target_dir}\"\n"
                "Move it aside manually if you really want to replace it."
            )

    if dry_run:
        print(f"[dry-run] link {label}: \"{target_dir}\" -> \"{source_dir}\"")
    else:
        target_dir.parent.mkdir(parents=True, exist_ok=True)

    create_link(source_dir, target_dir, link_type=link_type, target_is_directory=True, dry_run=dry_run)
    print(f"Linked {label}: \"{target_dir}\" -> \"{source_dir}\"")


def prepare_external_link(source_worktree: Path, *, link_type: str, dry_run: bool) -> None:
    prepare_directory_link(
        source_worktree / "Engine" / "External",
        REPO_ROOT / "Engine" / "External",
        label="External",
        link_type=link_type,
        dry_run=dry_run,
    )


def prepare_venv_link(source_worktree: Path, *, link_type: str, dry_run: bool) -> None:
    prepare_directory_link(
        source_worktree / VENV_DIR_NAME,
        REPO_ROOT / VENV_DIR_NAME,
        label=VENV_DIR_NAME,
        link_type=link_type,
        dry_run=dry_run,
    )


def prepare_agents_local_link(source_worktree: Path, *, dry_run: bool) -> None:
    source_file = source_worktree / AGENTS_LOCAL_NAME
    target_file = REPO_ROOT / AGENTS_LOCAL_NAME

    if not source_file.is_file():
        print(f"{AGENTS_LOCAL_NAME} was not found in \"{source_worktree}\"; skipping.")
        return

    if linked_to(target_file, source_file):
        print(f"{AGENTS_LOCAL_NAME} is already linked to \"{source_file}\".")
        return

    if target_file.exists() or is_link_like(target_file):
        remove_file_or_link(target_file, dry_run=dry_run)

    if dry_run:
        print(f"[dry-run] link \"{target_file}\" -> \"{source_file}\"")
    else:
        target_file.parent.mkdir(parents=True, exist_ok=True)

    create_link(source_file, target_file, link_type="symlink", target_is_directory=False, dry_run=dry_run)
    print(f"Linked {AGENTS_LOCAL_NAME}: \"{target_file}\" -> \"{source_file}\"")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Prepare a Durin Git worktree by linking shared dependency directories "
            "and linking machine-local AGENTS_LOCAL.md from a prepared worktree."
        )
    )
    parser.add_argument(
        "--source",
        help=(
            "Source Durin worktree root or Engine/External directory. "
            "Defaults to DURIN_WORKTREE_SOURCE, DURIN_EXTERNAL_SOURCE, "
            "DURIN_EXTERNAL_ROOT, the main Git worktree root, "
            "or a sibling Durin worktree."
        ),
    )
    parser.add_argument(
        "--link-type",
        choices=("auto", "junction", "symlink"),
        default="auto",
        help="Link type to create. Defaults to junction on Windows and symlink elsewhere.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print the operations without changing the filesystem.",
    )
    return parser.parse_args()


def main() -> int:
    try:
        args = parse_args()
        source_worktree = resolve_source(args)
        link_type = choose_link_type(args.link_type)

        print(f"Source worktree: \"{source_worktree}\"")
        print(f"Target worktree: \"{REPO_ROOT}\"")
        print(f"Link type: {link_type}")

        prepare_external_link(source_worktree, link_type=link_type, dry_run=args.dry_run)
        prepare_venv_link(source_worktree, link_type=link_type, dry_run=args.dry_run)
        prepare_agents_local_link(source_worktree, dry_run=args.dry_run)

    except PrepareWorktreeError as exc:
        print(exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"Filesystem error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
