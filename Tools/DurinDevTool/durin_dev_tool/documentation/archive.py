"""Transactional monthly archival for completed implementation plans."""

from __future__ import annotations

import datetime as dt
import re
from dataclasses import dataclass
from pathlib import Path

from .changes import (
    atomic_write as _atomic_write,
    repository_markdown_files as _repository_markdown_files,
    rewrite_markdown_links as _rewrite_markdown_links,
    rewrite_repository_paths as _rewrite_repository_paths,
)
from .catalog import load_document_catalog, validate_documents
from .model import DiagnosticSeverity
from .plans import PlanStatus, load_catalog


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


def apply_archive(plans_directory: Path, month: str) -> ArchivePreview:
    plans_directory = plans_directory.resolve()
    repository = plans_directory.parent.parent
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
        document_catalog = load_document_catalog(
            repository,
            include_archive=True,
        )
        document_errors = [
            diagnostic
            for diagnostic in validate_documents(document_catalog)
            if diagnostic.severity is DiagnosticSeverity.ERROR
        ]
        if document_errors:
            details = "\n- ".join(
                f"{diagnostic.path.as_posix()}: {diagnostic.message}"
                for diagnostic in document_errors
            )
            raise ArchiveError(
                "documentation validation failed after archive:\n- "
                + details
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
