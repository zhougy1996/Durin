"""Transactional monthly archival for completed implementation plans."""

from __future__ import annotations

import datetime as dt
import re
from dataclasses import dataclass
from pathlib import Path

from .changes import (
    DocumentChangeSet,
    FileChange,
    FilePrecondition,
    apply_change_set,
    content_hash,
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
) -> tuple[ArchivePreview, DocumentChangeSet | None]:
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
        return ArchivePreview(month, (), ()), None

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
    preconditions: list[FilePrecondition] = []
    changes: list[FileChange] = []
    reference_files: list[Path] = []
    for document in sorted(markdown_files):
        try:
            original = document.read_bytes()
            text = original.decode("utf-8")
        except (OSError, UnicodeError) as error:
            raise ArchiveError(f'could not read "{document}": {error}') from error
        source_hash = content_hash(original)
        preconditions.append(FilePrecondition(document, source_hash))
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
                r"^Status: Completed(?P<line_ending>\r?)$",
                lambda match: "Status: Archived" + match.group("line_ending"),
                rewritten,
                count=1,
                flags=re.MULTILINE,
            )
            if replacements != 1:
                raise ArchiveError(
                    f'completed status was not found exactly once in "{document}"'
                )
        content = rewritten.encode("utf-8")
        if content != original or document in move_map:
            changes.append(
                FileChange(
                    source=document,
                    destination=output_document,
                    content=content,
                    expected_source_hash=source_hash,
                )
            )
            if document not in move_map:
                reference_files.append(document)

    preview = ArchivePreview(month, moves, tuple(reference_files))
    return preview, DocumentChangeSet(
        operation="archive",
        changes=tuple(changes),
        preconditions=tuple(preconditions),
        primary_source=moves[0].source,
        primary_destination=moves[0].destination,
    )


def preview_archive(plans_directory: Path, month: str) -> ArchivePreview:
    preview, _ = _prepare(plans_directory.resolve(), month)
    return preview


def _validate_archive(plans_directory: Path, repository: Path) -> None:
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
            "documentation validation failed after archive:\n- " + details
        )


def apply_archive(plans_directory: Path, month: str) -> ArchivePreview:
    plans_directory = plans_directory.resolve()
    repository = plans_directory.parent.parent
    preview, change_set = _prepare(plans_directory, month)
    if change_set is None:
        return preview
    apply_change_set(
        change_set,
        validator=lambda: _validate_archive(plans_directory, repository),
    )
    return preview
