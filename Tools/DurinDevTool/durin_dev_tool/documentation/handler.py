"""Unified registry handler for documentation-plan commands."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from ..errors import DevToolError
from .archive import ArchiveError, ArchivePreview, apply_archive, preview_archive
from .model import DiagnosticSeverity, DocumentKind, DocumentRef
from .plans import filter_plans, load_catalog, render_listing
from .rendering import (
    render_change_set,
    render_diagnostics,
    render_documents,
    render_references,
)
from .service import DocumentWorkspace, ListDocumentsRequest, ValidationScope


def _plans_directory(repository_root: Path) -> Path:
    return repository_root / "Documentation" / "Plans"


def _default_output_format(
    namespace: argparse.Namespace,
    *,
    interactive: bool,
) -> str:
    return namespace.output_format or (
        "terminal" if interactive else "markdown"
    )


def _document_under(value: str | None) -> Path | None:
    if value is None:
        return None
    path = Path(value)
    if path.is_absolute() or ".." in path.parts:
        raise DevToolError("--under must be a repository-relative path")
    if not path.parts or path.parts[0] != "Documentation":
        raise DevToolError('--under must select a path inside "Documentation"')
    return path


def _run_document_list(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    interactive: bool,
    stdout: TextIO,
) -> int:
    workspace = DocumentWorkspace(repository_root)
    documents = workspace.list_documents(
        ListDocumentsRequest(
            under=_document_under(namespace.under),
            kinds=tuple(DocumentKind(kind) for kind in namespace.kinds or ()),
            query=getattr(namespace, "document_query", None),
            include_archive=namespace.include_archive,
        )
    )
    if not documents:
        raise DevToolError("no documentation files matched the request")
    print(
        render_documents(
            documents,
            output_format=_default_output_format(
                namespace,
                interactive=interactive,
            ),
        ),
        file=stdout,
    )
    return 0


def _run_document_refs(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    interactive: bool,
    stdout: TextIO,
) -> int:
    workspace = DocumentWorkspace(repository_root)
    references = workspace.references(
        DocumentRef.parse(namespace.document_path),
        include_archive=namespace.include_archive,
    )
    print(
        render_references(
            references,
            output_format=_default_output_format(
                namespace,
                interactive=interactive,
            ),
        ),
        file=stdout,
    )
    return 0


def _run_document_validate(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    interactive: bool,
    stdout: TextIO,
) -> int:
    workspace = DocumentWorkspace(repository_root)
    validation = workspace.validate(
        scope=ValidationScope(namespace.scope),
        include_archive=namespace.include_archive,
    )
    print(
        render_diagnostics(
            validation.diagnostics,
            output_format=_default_output_format(
                namespace,
                interactive=interactive,
            ),
            document_count=validation.document_count,
        ),
        file=stdout,
    )
    return (
        1
        if any(
            diagnostic.severity is DiagnosticSeverity.ERROR
            for diagnostic in validation.diagnostics
        )
        else 0
    )


def _run_document_change(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    interactive: bool,
    stdout: TextIO,
) -> int:
    workspace = DocumentWorkspace(repository_root)
    if namespace.document_action == "create":
        change_set = workspace.prepare_create(
            destination=DocumentRef.parse(namespace.document_path),
            kind=DocumentKind(namespace.document_kind),
            title=namespace.title,
            summary=namespace.summary,
        )
    else:
        change_set = workspace.prepare_move(
            source=DocumentRef.parse(namespace.source_path),
            destination=DocumentRef.parse(namespace.destination_path),
        )
    if namespace.apply:
        workspace.apply(change_set)
    print(
        render_change_set(
            change_set,
            repository_root=repository_root.resolve(),
            applied=namespace.apply,
            output_format=_default_output_format(
                namespace,
                interactive=interactive,
            ),
        ),
        file=stdout,
    )
    return 0


def _write_errors(errors: list[str], stderr: TextIO) -> None:
    for error in errors:
        print(f"error: {error}", file=stderr)


def _run_list(
    namespace: argparse.Namespace,
    *,
    plans_directory: Path,
    interactive: bool,
    stdout: TextIO,
    stderr: TextIO,
) -> int:
    if (
        namespace.scope in {"archive", "all"}
        and not namespace.query
        and not namespace.all_results
    ):
        raise DevToolError(
            "archive listings require --query <title-or-filename>; "
            "use --all-results only for an explicitly requested full listing"
        )
    catalog = load_catalog(plans_directory)
    errors = catalog.errors_for(namespace.scope)
    if errors:
        _write_errors(errors, stderr)
        return 1
    plans = filter_plans(catalog.select(namespace.scope), namespace.query)
    if not plans:
        if namespace.query:
            raise DevToolError(
                f"no {namespace.scope} plans match query {namespace.query!r}"
            )
        if namespace.scope == "completed":
            print(
                "No completed implementation plans are awaiting archival.",
                file=stdout,
            )
            return 0
        raise DevToolError(f"no {namespace.scope} implementation plans found")
    output_format = _default_output_format(
        namespace,
        interactive=interactive,
    )
    print(
        render_listing(
            plans,
            catalog.plans_directory,
            scope=namespace.scope,
            output_format=output_format,
            color=namespace.color,
        ),
        file=stdout,
    )
    return 0


def _run_validate(
    namespace: argparse.Namespace,
    *,
    plans_directory: Path,
    stdout: TextIO,
    stderr: TextIO,
) -> int:
    catalog = load_catalog(plans_directory)
    errors = catalog.errors_for(namespace.scope)
    if errors:
        _write_errors(errors, stderr)
        return 1
    plans = catalog.select(namespace.scope)
    if namespace.scope == "all":
        print(
            f"Validated {len(catalog.active)} active, "
            f"{len(catalog.completed)} completed, and "
            f"{len(catalog.archived)} archived implementation plans.",
            file=stdout,
        )
    else:
        print(
            f"Validated {len(plans)} {namespace.scope} implementation plans.",
            file=stdout,
        )
    return 0


def _print_archive(
    preview: ArchivePreview,
    *,
    repository_root: Path,
    applied: bool,
    stdout: TextIO,
) -> None:
    if not preview.moves:
        print(
            f"No completed plans are awaiting archival for {preview.month}.",
            file=stdout,
        )
        return
    action = "Archived" if applied else "Would archive"
    for move in preview.moves:
        print(
            f"{action}: {move.source.relative_to(repository_root).as_posix()} -> "
            f"{move.destination.relative_to(repository_root).as_posix()}",
            file=stdout,
        )
    verb = "Updated" if applied else "Would update"
    print(
        f"{verb} {len(preview.reference_files)} referencing Markdown file(s).",
        file=stdout,
    )
    for path in preview.reference_files:
        print(f"  {path.relative_to(repository_root).as_posix()}", file=stdout)
    if applied:
        print("Archive applied and all plans validated.", file=stdout)
    else:
        print("Dry-run only; add --apply to perform the archive.", file=stdout)


def _run_archive(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    plans_directory: Path,
    stdout: TextIO,
) -> int:
    preview = (
        apply_archive(plans_directory, namespace.month)
        if namespace.apply
        else preview_archive(plans_directory, namespace.month)
    )
    _print_archive(
        preview,
        repository_root=repository_root,
        applied=namespace.apply,
        stdout=stdout,
    )
    return 0


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    session_state: dict[str, object] | None = None,
    **_: object,
) -> int:
    """Adapt one CLI documentation request to the documentation services."""
    plans_directory = _plans_directory(repository_root)
    try:
        document_action = getattr(namespace, "document_action", "")
        interactive = session_state is not None
        if document_action in {"list", "find"}:
            return _run_document_list(
                namespace,
                repository_root=repository_root,
                interactive=interactive,
                stdout=stdout,
            )
        if document_action == "refs":
            return _run_document_refs(
                namespace,
                repository_root=repository_root,
                interactive=interactive,
                stdout=stdout,
            )
        if document_action == "validate":
            return _run_document_validate(
                namespace,
                repository_root=repository_root,
                interactive=interactive,
                stdout=stdout,
            )
        if document_action in {"create", "move"}:
            return _run_document_change(
                namespace,
                repository_root=repository_root,
                interactive=interactive,
                stdout=stdout,
            )
        if namespace.plan_action == "list":
            return _run_list(
                namespace,
                plans_directory=plans_directory,
                interactive=interactive,
                stdout=stdout,
                stderr=stderr,
            )
        if namespace.plan_action == "validate":
            return _run_validate(
                namespace,
                plans_directory=plans_directory,
                stdout=stdout,
                stderr=stderr,
            )
        if namespace.plan_action == "archive":
            return _run_archive(
                namespace,
                repository_root=repository_root,
                plans_directory=plans_directory,
                stdout=stdout,
            )
    except (ArchiveError, OSError, ValueError) as exc:
        raise DevToolError(str(exc)) from exc
    raise DevToolError("a documentation command is required")
