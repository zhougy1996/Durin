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

ENV_SOURCE_NAMES = ("DURIN_EXTERNAL_SOURCE", "DURIN_EXTERNAL_ROOT")
DEFAULT_SOURCE_WORKTREE_NAME = "Durin"


class LinkExternalError(RuntimeError):
    pass


def is_windows() -> bool:
    return os.name == "nt"


def normalize_path(path: Path) -> Path:
    return path.expanduser().resolve()


def find_source_argument(args: argparse.Namespace) -> str | None:
    if args.source:
        return args.source

    for env_name in ENV_SOURCE_NAMES:
        env_value = os.environ.get(env_name)
        if env_value:
            return env_value

    return str(REPO_ROOT.parent / DEFAULT_SOURCE_WORKTREE_NAME)


def resolve_external_dir(path_value: str) -> Path:
    path = normalize_path(Path(path_value))

    if path.name.lower() == "external":
        if not path.is_dir():
            raise LinkExternalError(f"External directory does not exist: \"{path}\"")
        return path

    repo_external = path / "Engine" / "External"
    if repo_external.exists():
        return normalize_path(repo_external)

    raise LinkExternalError(
        f"Could not find an Engine/External directory under \"{path}\".\n"
        "Pass either a Durin worktree root or an Engine/External directory."
    )


def resolve_source_external(args: argparse.Namespace) -> Path:
    source_value = find_source_argument(args)
    if not source_value:
        raise LinkExternalError(
            "Missing source External directory.\n"
            "Use --source <main-worktree-root-or-Engine/External>, or set DURIN_EXTERNAL_SOURCE."
        )

    source_external = resolve_external_dir(source_value)
    target_external = normalize_path(REPO_ROOT / "Engine" / "External")
    if same_path(source_external, target_external):
        raise LinkExternalError(
            f"Source External is the same as this worktree's External: \"{source_external}\"."
        )

    return source_external


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

    raise LinkExternalError(f"Refusing to remove non-empty real directory: \"{path}\"")


def create_junction(source: Path, target: Path, *, dry_run: bool) -> None:
    command = ["cmd", "/c", "mklink", "/J", str(target), str(source)]
    if dry_run:
        print(f"[dry-run] {' '.join(command)}")
        return

    result = subprocess.run(command)
    if result.returncode != 0:
        raise LinkExternalError(
            f"Failed to create junction \"{target}\" -> \"{source}\" "
            f"(exit code {result.returncode})."
        )


def create_symlink(source: Path, target: Path, *, dry_run: bool) -> None:
    if dry_run:
        print(f"[dry-run] symlink \"{target}\" -> \"{source}\"")
        return

    os.symlink(source, target, target_is_directory=True)


def choose_link_type(requested: str) -> str:
    if requested == "auto":
        return "junction" if is_windows() else "symlink"
    return requested


def create_link(source: Path, target: Path, *, link_type: str, dry_run: bool) -> None:
    if link_type == "junction":
        if not is_windows():
            raise LinkExternalError("Directory junctions are only supported on Windows.")
        create_junction(source, target, dry_run=dry_run)
        return

    if link_type == "symlink":
        create_symlink(source, target, dry_run=dry_run)
        return

    raise LinkExternalError(f"Unsupported link type: {link_type}")


def link_external(source_external: Path, target_external: Path, *, link_type: str, dry_run: bool) -> None:
    if not source_external.is_dir():
        raise LinkExternalError(f"Source External directory does not exist: \"{source_external}\"")

    if linked_to(target_external, source_external):
        print(f"External is already linked to \"{source_external}\".")
        return

    if target_external.exists() or is_link_like(target_external):
        if is_link_like(target_external) or is_empty_directory(target_external):
            remove_link_or_empty_dir(target_external, dry_run=dry_run)
        else:
            raise LinkExternalError(
                f"Target External already exists and is not an empty directory or link: \"{target_external}\"\n"
                "Move it aside manually if you really want to replace it."
            )

    if dry_run:
        print(f"[dry-run] link External: \"{target_external}\" -> \"{source_external}\"")
    else:
        target_external.parent.mkdir(parents=True, exist_ok=True)

    create_link(source_external, target_external, link_type=link_type, dry_run=dry_run)
    print(f"Linked External: \"{target_external}\" -> \"{source_external}\"")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Link this Durin worktree's Engine/External directory to another "
            "worktree's prepared third-party dependency directory."
        )
    )
    parser.add_argument(
        "--source",
        help=(
            "Source Durin worktree root or Engine/External directory. "
            "Defaults to DURIN_EXTERNAL_SOURCE, DURIN_EXTERNAL_ROOT, or a sibling Durin worktree."
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
        source_external = resolve_source_external(args)
        target_external = normalize_path(REPO_ROOT / "Engine" / "External")
        link_type = choose_link_type(args.link_type)

        print(f"Source External: \"{source_external}\"")
        print(f"Target External: \"{target_external}\"")
        print(f"Link type: {link_type}")

        link_external(source_external, target_external, link_type=link_type, dry_run=args.dry_run)

    except LinkExternalError as exc:
        print(exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print(f"Filesystem error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
