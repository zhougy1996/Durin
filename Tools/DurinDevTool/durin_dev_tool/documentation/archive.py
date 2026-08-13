"""Transactional monthly archival for completed lifecycle documents."""

from __future__ import annotations

import datetime as dt
import re
from dataclasses import dataclass
from pathlib import Path

from ..errors import DevToolError

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
from .model import Diagnostic, DiagnosticSeverity
from .lifecycle import LifecycleConfig, LifecycleWorkspace, PLAN_LIFECYCLE, ROADMAP_LIFECYCLE


class ArchiveError(DevToolError):
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
    documents_directory: Path,
    month: str,
    *,
    config: LifecycleConfig,
) -> tuple[ArchivePreview, DocumentChangeSet | None]:
    month = parse_month(month)
    workspace = LifecycleWorkspace(documents_directory.parent.parent, config)
    catalog = workspace.catalog()
    document_kind = config.document_label
    if catalog.errors:
        raise ArchiveError(
            f"{document_kind} validation failed:\n- "
            + "\n- ".join(catalog.errors)
        )
    selected = [
        plan
        for plan in catalog.completed
        if plan.completed is not None and plan.completed.strftime("%Y-%m") == month
    ]
    if not selected:
        return ArchivePreview(month, (), ()), None

    repository = documents_directory.parent.parent.resolve()
    archive_directory = documents_directory / "Archive" / month
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


def preview_lifecycle_archive(
    documents_directory: Path,
    month: str,
    config: LifecycleConfig,
) -> ArchivePreview:
    preview, _ = _prepare(
        documents_directory.resolve(),
        month,
        config=config,
    )
    return preview


def preview_archive(plans_directory: Path, month: str) -> ArchivePreview:
    return preview_lifecycle_archive(plans_directory, month, PLAN_LIFECYCLE)


def preview_roadmap_archive(
    roadmaps_directory: Path,
    month: str,
) -> ArchivePreview:
    return preview_lifecycle_archive(roadmaps_directory, month, ROADMAP_LIFECYCLE)


def _document_diagnostics(repository: Path) -> tuple[Diagnostic, ...]:
    document_catalog = load_document_catalog(
        repository,
        include_archive=True,
    )
    return tuple(validate_documents(document_catalog))


def _diagnostic_identity(diagnostic: Diagnostic) -> tuple[object, ...]:
    return (
        diagnostic.code,
        diagnostic.path,
        diagnostic.line,
        diagnostic.target,
        diagnostic.message,
    )


def _validate_archive(
    documents_directory: Path,
    repository: Path,
    *,
    config: LifecycleConfig,
    baseline_diagnostics: tuple[Diagnostic, ...],
) -> None:
    catalog = LifecycleWorkspace(repository, config).catalog()
    if catalog.errors:
        raise ArchiveError(
            "archive validation failed:\n- " + "\n- ".join(catalog.errors)
        )
    baseline_identities = {
        _diagnostic_identity(diagnostic)
        for diagnostic in baseline_diagnostics
    }
    document_regressions = [
        diagnostic
        for diagnostic in _document_diagnostics(repository)
        if (
            diagnostic.severity is DiagnosticSeverity.ERROR
            or _diagnostic_identity(diagnostic) not in baseline_identities
        )
    ]
    if document_regressions:
        details = "\n- ".join(
            f"{diagnostic.severity.value}: "
            f"{diagnostic.path.as_posix()}: {diagnostic.message}"
            for diagnostic in document_regressions
        )
        raise ArchiveError(
            "documentation validation regressed after archive:\n- " + details
        )


def apply_lifecycle_archive(
    documents_directory: Path,
    month: str,
    config: LifecycleConfig,
) -> ArchivePreview:
    documents_directory = documents_directory.resolve()
    repository = documents_directory.parent.parent
    preview, change_set = _prepare(
        documents_directory,
        month,
        config=config,
    )
    if change_set is None:
        return preview
    baseline_diagnostics = _document_diagnostics(repository)
    apply_change_set(
        change_set,
        validator=lambda: _validate_archive(
            documents_directory,
            repository,
            config=config,
            baseline_diagnostics=baseline_diagnostics,
        ),
    )
    return preview


def apply_archive(plans_directory: Path, month: str) -> ArchivePreview:
    return apply_lifecycle_archive(plans_directory, month, PLAN_LIFECYCLE)


def apply_roadmap_archive(
    roadmaps_directory: Path,
    month: str,
) -> ArchivePreview:
    return apply_lifecycle_archive(roadmaps_directory, month, ROADMAP_LIFECYCLE)
