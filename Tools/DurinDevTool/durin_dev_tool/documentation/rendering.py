"""Stable terminal, Markdown, and JSON rendering for documentation results."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Sequence

from .changes import DocumentChangeSet
from .model import Diagnostic, Document
from .service import DocumentReferences
from .tasks import Task


def render_documents(
    documents: Sequence[Document],
    *,
    output_format: str,
) -> str:
    if output_format == "json":
        return json.dumps(
            {
                "schemaVersion": 1,
                "documents": [
                    {
                        "path": document.ref.as_posix(),
                        "title": document.title,
                        "kind": document.kind.value,
                    }
                    for document in documents
                ],
            },
            indent=2,
        )
    if output_format == "markdown":
        lines = ["| Document | Kind |", "| --- | --- |"]
        lines.extend(
            f"| [{document.title}]({document.ref.as_posix()}) | "
            f"{document.kind.value} |"
            for document in documents
        )
        return "\n".join(lines)
    return "\n\n".join(
        f"[{document.kind.value}] {document.title}\n"
        f"  {document.ref.as_posix()}"
        for document in documents
    )


def render_tasks(
    tasks: Sequence[Task],
    *,
    repository_root: Path,
    output_format: str,
) -> str:
    def relative(task: Task) -> str:
        return task.path.relative_to(repository_root).as_posix()

    def markdown_cell(value: str) -> str:
        return value.replace("\\", "\\\\").replace("|", "\\|")

    if output_format == "json":
        return json.dumps(
            {
                "schemaVersion": 1,
                "tasks": [
                    {
                        "path": relative(task),
                        "title": task.title,
                        "summary": task.summary,
                    }
                    for task in tasks
                ],
            },
            indent=2,
        )
    if output_format == "markdown":
        lines = ["| Task | Outcome |", "| --- | --- |"]
        lines.extend(
            f"| [{markdown_cell(task.title)}]({relative(task)}) | "
            f"{markdown_cell(task.summary)} |"
            for task in tasks
        )
        return "\n".join(lines)
    return "\n\n".join(
        f"{task.title}\n  {relative(task)}\n  {task.summary}"
        for task in tasks
    )


def render_diagnostics(
    diagnostics: Sequence[Diagnostic],
    *,
    output_format: str,
    document_count: int,
) -> str:
    if output_format == "json":
        return json.dumps(
            {
                "schemaVersion": 1,
                "operation": "validate",
                "documents": document_count,
                "diagnostics": [
                    {
                        "code": diagnostic.code,
                        "severity": diagnostic.severity.value,
                        "path": diagnostic.path.as_posix(),
                        **(
                            {"line": diagnostic.line}
                            if diagnostic.line is not None
                            else {}
                        ),
                        **(
                            {"target": diagnostic.target}
                            if diagnostic.target is not None
                            else {}
                        ),
                        "message": diagnostic.message,
                    }
                    for diagnostic in diagnostics
                ],
            },
            indent=2,
        )
    if not diagnostics:
        return f"Validated {document_count} documentation file(s)."
    return "\n".join(
        f"{diagnostic.severity.value}: {diagnostic.path.as_posix()}"
        f"{f':{diagnostic.line}' if diagnostic.line is not None else ''}: "
        f"{diagnostic.message}"
        f"{f' ({diagnostic.target})' if diagnostic.target else ''}"
        for diagnostic in diagnostics
    )


def render_references(
    references: DocumentReferences,
    *,
    output_format: str,
) -> str:
    if output_format == "json":
        return json.dumps(
            {
                "schemaVersion": 1,
                "document": references.document.ref.as_posix(),
                "inbound": [
                    {
                        "path": document.ref.as_posix(),
                        "line": line,
                    }
                    for document, line in references.inbound
                ],
                "outbound": [
                    {
                        "path": ref.as_posix() if ref is not None else None,
                        "line": line,
                        "target": target,
                    }
                    for ref, line, target in references.outbound
                ],
            },
            indent=2,
        )
    lines = [f"References for {references.document.ref.as_posix()}:", "Inbound:"]
    lines.extend(
        f"  {document.ref.as_posix()}:{line}"
        for document, line in references.inbound
    )
    if not references.inbound:
        lines.append("  none")
    lines.append("Outbound:")
    lines.extend(
        f"  {(ref.as_posix() if ref is not None else target)}:{line}"
        for ref, line, target in references.outbound
    )
    if not references.outbound:
        lines.append("  none")
    return "\n".join(lines)


def render_change_set(
    change_set: DocumentChangeSet,
    *,
    repository_root: Path,
    applied: bool,
    output_format: str,
) -> str:
    def relative(path: Path | None) -> str | None:
        return (
            path.relative_to(repository_root).as_posix()
            if path is not None
            else None
        )

    if output_format == "json":
        return json.dumps(
            {
                "schemaVersion": 1,
                "operation": change_set.operation,
                "applied": applied,
                "changes": [
                    {
                        "source": relative(change.source),
                        "destination": relative(change.destination),
                    }
                    for change in change_set.changes
                ]
                + [
                    {
                        "source": relative(deletion.path),
                        "destination": None,
                    }
                    for deletion in change_set.deletions
                ],
            },
            indent=2,
        )
    action = "Applied" if applied else "Would apply"
    lines = [f"{action} document {change_set.operation}:"]
    for change in change_set.changes:
        source = relative(change.source)
        destination = relative(change.destination)
        lines.append(
            f"  {source} -> {destination}"
            if source is not None and source != destination
            else f"  {destination}"
        )
    for deletion in change_set.deletions:
        lines.append(f"  {relative(deletion.path)} -> deleted")
    if not applied:
        lines.append("Dry-run only; add --apply to perform the change.")
    return "\n".join(lines)
