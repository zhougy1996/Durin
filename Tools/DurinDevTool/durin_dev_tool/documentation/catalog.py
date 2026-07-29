"""Discovery, Markdown link extraction, and mechanical document validation."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Iterable, Sequence
from urllib.parse import unquote, urlsplit

from .model import (
    Diagnostic,
    DiagnosticSeverity,
    Document,
    DocumentCatalog,
    DocumentKind,
    DocumentLink,
    DocumentRef,
    infer_document_kind,
)
from .plans import parse_plan
from .tasks import load_task_catalog


INLINE_LINK_PATTERN = re.compile(r"!?\[[^\]\n]*\]\((?P<target>[^)\n]+)\)")
REFERENCE_LINK_PATTERN = re.compile(
    r"^\s*\[[^\]]+\]:\s*(?P<target>\S+)", re.MULTILINE
)
TITLE_PATTERN = re.compile(r"^# (?P<title>.+?)\s*$")


def _markdown_targets(text: str) -> Iterable[tuple[int, str]]:
    in_fence = False
    for line_number, line in enumerate(text.splitlines(), start=1):
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for pattern in (INLINE_LINK_PATTERN, REFERENCE_LINK_PATTERN):
            for match in pattern.finditer(line):
                yield line_number, match.group("target")


def _target_path(target: str) -> tuple[str, bool]:
    value = target.strip()
    if value.startswith("<"):
        closing = value.find(">")
        if closing < 0:
            return value, False
        value = value[1:closing]
    else:
        value = value.split(maxsplit=1)[0]
    if not value or value.startswith("#"):
        return "", False
    parsed = urlsplit(value)
    if parsed.scheme or value.startswith("//"):
        return "", True
    return unquote(parsed.path.replace("\\", "/")), False


def _resolve_link(
    repository_root: Path,
    document_path: Path,
    target: str,
) -> tuple[Path | None, bool]:
    path_text, external = _target_path(target)
    if external or not path_text:
        return None, external
    if path_text.startswith("/"):
        return None, False
    resolved = (document_path.parent / path_text).resolve()
    try:
        return resolved.relative_to(repository_root), False
    except ValueError:
        return None, False


def _parse_document(
    repository_root: Path,
    relative_path: Path,
) -> tuple[Document | None, list[Diagnostic]]:
    absolute_path = repository_root / relative_path
    try:
        text = absolute_path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        return None, [
            Diagnostic(
                code="doc.read.failed",
                severity=DiagnosticSeverity.ERROR,
                message=f"could not read UTF-8 Markdown: {error}",
                path=relative_path,
            )
        ]
    title = ""
    for line in text.splitlines():
        if match := TITLE_PATTERN.fullmatch(line):
            title = match.group("title")
            break
    diagnostics: list[Diagnostic] = []
    if not title:
        diagnostics.append(
            Diagnostic(
                code="doc.title.missing",
                severity=DiagnosticSeverity.ERROR,
                message="document must contain a level-one Markdown title",
                path=relative_path,
            )
        )
    links = tuple(
        DocumentLink(
            target=target,
            line=line,
            resolved_path=resolved,
            external=external,
        )
        for line, target in _markdown_targets(text)
        for resolved, external in [
            _resolve_link(repository_root, absolute_path, target)
        ]
    )
    return (
        Document(
            ref=DocumentRef(relative_path),
            kind=infer_document_kind(relative_path),
            title=title,
            links=links,
        ),
        diagnostics,
    )


def load_document_catalog(
    repository_root: Path,
    *,
    include_archive: bool = False,
) -> DocumentCatalog:
    repository_root = repository_root.resolve()
    documentation_root = repository_root / "Documentation"
    documents: list[Document] = []
    diagnostics: list[Diagnostic] = []
    if not documentation_root.is_dir():
        diagnostics.append(
            Diagnostic(
                code="doc.root.missing",
                severity=DiagnosticSeverity.ERROR,
                message='repository does not contain a "Documentation" directory',
                path=Path("Documentation"),
            )
        )
        return DocumentCatalog(repository_root, (), tuple(diagnostics))

    for absolute_path in sorted(
        documentation_root.rglob("*.md"),
        key=lambda candidate: candidate.as_posix().casefold(),
    ):
        relative_path = absolute_path.relative_to(repository_root)
        relative_parts = relative_path.parts
        if len(relative_parts) > 1 and relative_parts[1] == "Local":
            continue
        if (
            not include_archive
            and len(relative_parts) > 2
            and relative_parts[1:3] == ("Plans", "Archive")
        ):
            continue
        document, document_diagnostics = _parse_document(
            repository_root,
            relative_path,
        )
        diagnostics.extend(document_diagnostics)
        if document is not None:
            documents.append(document)
    return DocumentCatalog(
        repository_root,
        tuple(documents),
        tuple(diagnostics),
    )


def filter_documents(
    documents: Sequence[Document],
    *,
    under: Path | None = None,
    kinds: Sequence[DocumentKind] = (),
    query: str | None = None,
) -> list[Document]:
    selected = list(documents)
    if under is not None:
        selected = [
            document
            for document in selected
            if document.ref.path == under or under in document.ref.path.parents
        ]
    if kinds:
        accepted = set(kinds)
        selected = [
            document for document in selected if document.kind in accepted
        ]
    if query:
        needle = query.casefold()
        selected = [
            document
            for document in selected
            if needle in document.title.casefold()
            or needle in document.ref.as_posix().casefold()
        ]
    return selected


def validate_documents(
    catalog: DocumentCatalog,
    documents: Sequence[Document] | None = None,
    *,
    validate_task_domain: bool | None = None,
) -> list[Diagnostic]:
    selected = tuple(documents) if documents is not None else catalog.documents
    selected_paths = {document.ref.path for document in selected}
    diagnostics = [
        diagnostic
        for diagnostic in catalog.diagnostics
        if diagnostic.path in selected_paths or documents is None
    ]
    if validate_task_domain is None:
        validate_task_domain = documents is None or any(
            len(document.ref.path.parts) > 1
            and document.ref.path.parts[1] == "Tasks"
            for document in selected
        )
    if validate_task_domain:
        diagnostics.extend(
            load_task_catalog(
                catalog.repository_root / "Documentation" / "Tasks"
            ).diagnostics
        )

    for document in selected:
        for link in document.links:
            if link.external or link.target.startswith("#"):
                continue
            if link.resolved_path is None:
                diagnostics.append(
                    Diagnostic(
                        code="doc.link.outside_repository",
                        severity=DiagnosticSeverity.ERROR,
                        message="local link resolves outside the repository",
                        path=document.ref.path,
                        line=link.line,
                        target=link.target,
                    )
                )
                continue
            target = catalog.repository_root / link.resolved_path
            if not target.exists():
                diagnostics.append(
                    Diagnostic(
                        code="doc.link.missing",
                        severity=DiagnosticSeverity.ERROR,
                        message="referenced local path does not exist",
                        path=document.ref.path,
                        line=link.line,
                        target=link.target,
                    )
                )
        if document.kind is DocumentKind.INVESTIGATION:
            text = (catalog.repository_root / document.ref.path).read_text(
                encoding="utf-8"
            )
            if not re.search(r"^\*\*Status:\*\*\s+\S+", text, re.MULTILINE):
                diagnostics.append(
                    Diagnostic(
                        code="doc.investigation.status_missing",
                        severity=DiagnosticSeverity.ERROR,
                        message="investigation must declare a bold Status field",
                        path=document.ref.path,
                    )
                )
            if not re.search(
                r"^\*\*Last reviewed:\*\*\s+\d{4}-\d{2}-\d{2}$",
                text,
                re.MULTILINE,
            ):
                diagnostics.append(
                    Diagnostic(
                        code="doc.investigation.reviewed_missing",
                        severity=DiagnosticSeverity.ERROR,
                        message="investigation must declare a Last reviewed date",
                        path=document.ref.path,
                    )
                )
        if document.kind is DocumentKind.PLAN:
            _, plan_errors = parse_plan(
                catalog.repository_root / document.ref.path
            )
            diagnostics.extend(
                Diagnostic(
                    code="doc.plan.invalid",
                    severity=DiagnosticSeverity.ERROR,
                    message=message,
                    path=document.ref.path,
                )
                for message in plan_errors
            )
    return diagnostics
