"""Ordinary document command adapter."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from ..errors import DevToolError
from .adapter_common import document_under, output_format
from .model import DiagnosticSeverity, DocumentKind, DocumentRef
from .rendering import render_change_set, render_diagnostics, render_documents, render_references
from .service import DocumentWorkspace, ListDocumentsRequest, ValidationScope


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    interactive: bool,
    stdout: TextIO,
) -> int:
    workspace = DocumentWorkspace(repository_root)
    action = namespace.document_action
    if action in {"list", "find"}:
        documents = workspace.list_documents(ListDocumentsRequest(
            under=document_under(namespace.under),
            kinds=tuple(DocumentKind(kind) for kind in namespace.kinds or ()),
            query=getattr(namespace, "document_query", None),
            include_archive=namespace.include_archive,
        ))
        if (limit := getattr(namespace, "document_limit", None)) is not None:
            documents = documents[:limit]
        if not documents:
            raise DevToolError("no documentation files matched the request")
        print(render_documents(documents, output_format=output_format(namespace, interactive=interactive)), file=stdout)
        return 0
    if action == "refs":
        references = workspace.references(
            DocumentRef.parse(namespace.document_path),
            include_archive=namespace.include_archive,
        )
        print(render_references(references, output_format=output_format(namespace, interactive=interactive)), file=stdout)
        return 0
    if action == "validate":
        validation = workspace.validate(
            scope=ValidationScope(namespace.scope),
            include_archive=namespace.include_archive,
        )
        print(render_diagnostics(
            validation.diagnostics,
            output_format=output_format(namespace, interactive=interactive),
            document_count=validation.document_count,
        ), file=stdout)
        return 1 if any(item.severity is DiagnosticSeverity.ERROR for item in validation.diagnostics) else 0
    change_set = (
        workspace.prepare_create(
            destination=DocumentRef.parse(namespace.document_path),
            kind=DocumentKind(namespace.document_kind),
            title=namespace.title,
            summary=namespace.summary,
        )
        if action == "create"
        else workspace.prepare_move(
            source=DocumentRef.parse(namespace.source_path),
            destination=DocumentRef.parse(namespace.destination_path),
        )
    )
    applied = not namespace.dry_run
    if applied:
        workspace.apply(change_set)
    print(render_change_set(
        change_set,
        repository_root=repository_root.resolve(),
        applied=applied,
        output_format=output_format(namespace, interactive=interactive),
        preview_instruction="Dry-run only; remove --dry-run to perform the change.",
    ), file=stdout)
    return 0
