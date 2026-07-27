#!/usr/bin/env python3
"""Batch completed implementation plans into their completion-month archive."""

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from list_plans import discover_plans


MARKDOWN_LINK_PATTERN = re.compile(
    r"(?P<prefix>\]\()(?P<target>[^)\n]+)(?P<suffix>\))"
)
REFERENCE_LINK_PATTERN = re.compile(
    r"(?P<prefix>^\s*\[[^\]]+\]:\s*)(?P<target>\S+)(?P<suffix>.*)$",
    re.MULTILINE,
)


@dataclass(frozen=True)
class Move:
    source: Path
    destination: Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Preview or apply the monthly archive of plans marked Completed."
        )
    )
    parser.add_argument(
        "--month",
        required=True,
        help="completion month to archive in YYYY-MM form",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="move plans and repair Markdown references; default is dry-run",
    )
    return parser.parse_args()


def parse_month(value: str) -> str:
    try:
        parsed = dt.datetime.strptime(value, "%Y-%m")
    except ValueError as error:
        raise ValueError("--month must be a valid YYYY-MM month") from error
    return parsed.strftime("%Y-%m")


def split_target(target: str) -> tuple[str, str, bool]:
    enclosed = target.startswith("<") and target.endswith(">")
    raw = target[1:-1] if enclosed else target
    path, separator, fragment = raw.partition("#")
    suffix = f"{separator}{fragment}" if separator else ""
    return path, suffix, enclosed


def rewrite_target(
    target: str,
    *,
    document: Path,
    output_document: Path,
    moves: dict[Path, Path],
) -> str:
    path_text, suffix, enclosed = split_target(target)
    if (
        not path_text
        or "://" in path_text
        or path_text.startswith(("mailto:", "/"))
        or Path(path_text).is_absolute()
    ):
        return target

    candidate = (document.parent / path_text).resolve()
    destination = moves.get(candidate, candidate)
    if candidate not in moves and output_document == document:
        return target

    relative = os.path.relpath(
        destination,
        output_document.parent,
    ).replace(os.sep, "/")
    rewritten = f"{relative}{suffix}"
    return f"<{rewritten}>" if enclosed else rewritten


def rewrite_markdown_links(
    text: str,
    *,
    document: Path,
    output_document: Path,
    moves: dict[Path, Path],
) -> str:
    def replace(match: re.Match[str]) -> str:
        target = rewrite_target(
            match.group("target"),
            document=document,
            output_document=output_document,
            moves=moves,
        )
        return f"{match.group('prefix')}{target}{match.group('suffix')}"

    text = MARKDOWN_LINK_PATTERN.sub(replace, text)
    return REFERENCE_LINK_PATTERN.sub(replace, text)


def rewrite_repository_paths(
    text: str,
    *,
    repository: Path,
    moves: dict[Path, Path],
) -> str:
    for source, destination in moves.items():
        old = source.relative_to(repository).as_posix()
        new = destination.relative_to(repository).as_posix()
        text = text.replace(old, new)
        text = text.replace(old.replace("/", "\\"), new.replace("/", "\\"))
    return text


def archive_metadata(text: str) -> str:
    return re.sub(
        r"^Status: Completed$",
        "Status: Archived",
        text,
        count=1,
        flags=re.MULTILINE,
    )


def main() -> int:
    args = parse_args()
    try:
        month = parse_month(args.month)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    plans_directory = Path(__file__).resolve().parent
    repository = plans_directory.parent.parent
    plans, errors = discover_plans(plans_directory)
    _, archive_errors = discover_plans(
        plans_directory / "Archive",
        recursive=True,
        require_archive_month=True,
    )
    errors.extend(f"Archive/{error}" for error in archive_errors)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    selected = [
        plan
        for plan in plans
        if plan.status == "Completed"
        and plan.completed is not None
        and plan.completed.strftime("%Y-%m") == month
    ]
    if not selected:
        print(f"No completed plans are awaiting archival for {month}.")
        return 0

    archive_directory = plans_directory / "Archive" / month
    planned_moves = [
        Move(plan.path, archive_directory / plan.path.name) for plan in selected
    ]
    conflicts = [
        move.destination
        for move in planned_moves
        if move.destination.exists()
    ]
    if conflicts:
        for conflict in conflicts:
            print(
                f"error: archive destination already exists: "
                f"{conflict.relative_to(repository).as_posix()}",
                file=sys.stderr,
            )
        return 1

    moves = {
        move.source.resolve(): move.destination.resolve() for move in planned_moves
    }
    markdown_files = sorted(repository.rglob("*.md"))
    rewrites: dict[Path, str] = {}
    for document in markdown_files:
        text = document.read_text(encoding="utf-8")
        output_document = moves.get(document.resolve(), document)
        rewritten = rewrite_repository_paths(
            rewrite_markdown_links(
                text,
                document=document,
                output_document=output_document,
                moves=moves,
            ),
            repository=repository,
            moves=moves,
        )
        if document.resolve() in moves:
            rewritten = archive_metadata(rewritten)
        if rewritten != text:
            rewrites[document] = rewritten

    action = "Archive" if args.apply else "Would archive"
    for move in planned_moves:
        print(
            f"{action}: {move.source.relative_to(repository).as_posix()} -> "
            f"{move.destination.relative_to(repository).as_posix()}"
        )
    reference_files = [
        path for path in rewrites if path.resolve() not in moves
    ]
    print(
        f"{'Updated' if args.apply else 'Would update'} "
        f"{len(reference_files)} referencing Markdown file(s)."
    )
    for path in reference_files:
        print(f"  {path.relative_to(repository).as_posix()}")

    if not args.apply:
        print("Dry-run only; pass --apply to perform the archive.")
        return 0

    archive_directory.mkdir(parents=True, exist_ok=True)
    for move in planned_moves:
        move.source.rename(move.destination)
    for original, rewritten in rewrites.items():
        destination = moves.get(original.resolve(), original)
        destination.write_text(rewritten, encoding="utf-8")

    validation = subprocess.run(
        [
            sys.executable,
            str(plans_directory / "list_plans.py"),
            "--scope",
            "all",
            "--validate",
        ],
        cwd=repository,
        check=False,
    )
    if validation.returncode != 0:
        print("error: archive applied but plan validation failed", file=sys.stderr)
        return validation.returncode

    print("Archive applied and all plans validated.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
