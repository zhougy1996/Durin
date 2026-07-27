"""Transactional monthly archival for completed implementation plans."""

from __future__ import annotations

import datetime as dt
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path

from .plans import PlanStatus, load_catalog


MARKDOWN_LINK_PATTERN = re.compile(
    r"(?P<prefix>!?\[[^\]\n]*\]\()(?P<target>[^)\n]+)(?P<suffix>\))"
)
REFERENCE_LINK_PATTERN = re.compile(
    r"(?P<prefix>^\s*\[[^\]]+\]:\s*)(?P<target>\S+)(?P<suffix>.*)$",
    re.MULTILINE,
)


class ArchiveError(RuntimeError):
    pass


@dataclass(frozen=True)
class Move:
    source: Path
    destination: Path


@dataclass(frozen=True)
class ArchivePreview:
    month: str
    moves: tuple[Move, ...]
    reference_files: tuple[Path, ...]


def parse_month(value: str) -> str:
    try:
        parsed = dt.datetime.strptime(value, "%Y-%m")
    except ValueError as error:
        raise ArchiveError("month must be a valid YYYY-MM value") from error
    normalized = parsed.strftime("%Y-%m")
    if normalized != value:
        raise ArchiveError("month must be a valid YYYY-MM value")
    return normalized


def _split_target(target: str) -> tuple[str, str, bool]:
    enclosed = target.startswith("<")
    if enclosed:
        closing = target.find(">")
        if closing < 0:
            return target, "", False
        path_with_fragment = target[1:closing]
        trailing = target[closing + 1 :]
    else:
        match = re.match(r"(?P<path>\S+)(?P<trailing>\s+.*)?$", target)
        if match is None:
            return target, "", False
        path_with_fragment = match.group("path")
        trailing = match.group("trailing") or ""
    path, separator, fragment = path_with_fragment.partition("#")
    suffix = f"{separator}{fragment}" if separator else ""
    wrapper = f">{trailing}" if enclosed else trailing
    return path, f"{suffix}{wrapper}", enclosed


def _rewrite_target(
    target: str,
    *,
    document: Path,
    output_document: Path,
    moves: dict[Path, Path],
) -> str:
    path_text, suffix, enclosed = _split_target(target)
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
    relative = os.path.relpath(destination, output_document.parent).replace(os.sep, "/")
    if enclosed:
        fragment, marker, trailing = suffix.partition(">")
        return f"<{relative}{fragment}>{trailing}" if marker else target
    return f"{relative}{suffix}"


def _rewrite_markdown_links(
    text: str,
    *,
    document: Path,
    output_document: Path,
    moves: dict[Path, Path],
) -> str:
    def replace(match: re.Match[str]) -> str:
        target = _rewrite_target(
            match.group("target"),
            document=document,
            output_document=output_document,
            moves=moves,
        )
        return f"{match.group('prefix')}{target}{match.group('suffix')}"

    return REFERENCE_LINK_PATTERN.sub(
        replace,
        MARKDOWN_LINK_PATTERN.sub(replace, text),
    )


def _rewrite_repository_paths(
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


def _repository_markdown_files(repository: Path) -> list[Path]:
    result = subprocess.run(
        [
            "git",
            "-C",
            str(repository),
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
            "--",
            "*.md",
        ],
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = result.stderr.decode(errors="replace").strip() or "git ls-files failed"
        raise ArchiveError(f"could not discover repository Markdown files: {detail}")
    return sorted(
        (repository / os.fsdecode(raw_path)).resolve()
        for raw_path in result.stdout.split(b"\0")
        if raw_path
    )


def _prepare(
    plans_directory: Path,
    month: str,
) -> tuple[ArchivePreview, dict[Path, str]]:
    month = parse_month(month)
    catalog = load_catalog(plans_directory)
    if catalog.errors:
        raise ArchiveError("plan validation failed:\n- " + "\n- ".join(catalog.errors))
    selected = [
        plan
        for plan in catalog.completed
        if plan.completed is not None and plan.completed.strftime("%Y-%m") == month
    ]
    if not selected:
        return ArchivePreview(month, (), ()), {}

    repository = plans_directory.parent.parent.resolve()
    archive_directory = plans_directory / "Archive" / month
    moves = tuple(
        Move(plan.path, archive_directory / plan.path.name) for plan in selected
    )
    conflicts = [move.destination for move in moves if move.destination.exists()]
    if conflicts:
        display = "\n- ".join(
            path.relative_to(repository).as_posix() for path in conflicts
        )
        raise ArchiveError(f"archive destination already exists:\n- {display}")

    move_map = {
        move.source.resolve(): move.destination.resolve() for move in moves
    }
    markdown_files = set(_repository_markdown_files(repository))
    markdown_files.update(move.source.resolve() for move in moves)
    rewrites: dict[Path, str] = {}
    for document in sorted(markdown_files):
        try:
            text = document.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as error:
            raise ArchiveError(f'could not read "{document}": {error}') from error
        output_document = move_map.get(document, document)
        rewritten = _rewrite_repository_paths(
            _rewrite_markdown_links(
                text,
                document=document,
                output_document=output_document,
                moves=move_map,
            ),
            repository=repository,
            moves=move_map,
        )
        if document in move_map:
            rewritten, replacements = re.subn(
                r"^Status: Completed$",
                "Status: Archived",
                rewritten,
                count=1,
                flags=re.MULTILINE,
            )
            if replacements != 1:
                raise ArchiveError(
                    f'completed status was not found exactly once in "{document}"'
                )
        if rewritten != text:
            rewrites[document] = rewritten

    reference_files = tuple(
        path for path in rewrites if path not in move_map
    )
    return ArchivePreview(month, moves, reference_files), rewrites


def preview_archive(plans_directory: Path, month: str) -> ArchivePreview:
    preview, _ = _prepare(plans_directory.resolve(), month)
    return preview


def _atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def apply_archive(plans_directory: Path, month: str) -> ArchivePreview:
    plans_directory = plans_directory.resolve()
    preview, rewrites = _prepare(plans_directory, month)
    if not preview.moves:
        return preview

    move_map = {
        move.source.resolve(): move.destination.resolve() for move in preview.moves
    }
    touched = set(rewrites)
    touched.update(move_map.values())
    snapshots = {
        path: path.read_bytes() if path.exists() else None for path in touched
    }
    created_directories: set[Path] = set()
    try:
        for move in preview.moves:
            if not move.destination.parent.exists():
                candidate = move.destination.parent
                while not candidate.exists():
                    created_directories.add(candidate)
                    candidate = candidate.parent
                move.destination.parent.mkdir(parents=True)
        for original, rewritten in rewrites.items():
            destination = move_map.get(original.resolve(), original)
            _atomic_write(destination, rewritten.encode("utf-8"))
        for move in preview.moves:
            move.source.unlink()

        catalog = load_catalog(plans_directory)
        if catalog.errors:
            raise ArchiveError(
                "archive validation failed:\n- " + "\n- ".join(catalog.errors)
            )
    except BaseException:
        for path, content in snapshots.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                _atomic_write(path, content)
        for move in preview.moves:
            source = move.source.resolve()
            if source not in snapshots:
                _atomic_write(source, move.destination.read_bytes())
        for directory in sorted(
            created_directories,
            key=lambda path: len(path.parts),
            reverse=True,
        ):
            try:
                directory.rmdir()
            except OSError:
                pass
        raise
    return preview
