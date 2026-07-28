"""Application API for ordinary repository documentation operations."""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Sequence

from ..errors import DevToolError
from .catalog import filter_documents, load_document_catalog, validate_documents
from .changes import (
    DocumentChangeSet,
    apply_change_set,
    prepare_create,
    prepare_move,
)
from .model import (
    Diagnostic,
    DiagnosticSeverity,
    Document,
    DocumentCatalog,
    DocumentKind,
    DocumentRef,
)


class ValidationScope(str, Enum):
    ALL = "all"
    CHANGED = "changed"


@dataclass(frozen=True)
class ListDocumentsRequest:
    under: Path | None = None
    kinds: tuple[DocumentKind, ...] = ()
    query: str | None = None
    include_archive: bool = False


@dataclass(frozen=True)
class ValidationResult:
    document_count: int
    diagnostics: tuple[Diagnostic, ...]


@dataclass(frozen=True)
class DocumentReferences:
    document: Document
    inbound: tuple[tuple[Document, int], ...]
    outbound: tuple[tuple[DocumentRef | None, int, str], ...]


class DocumentWorkspace:
    def __init__(self, repository_root: Path) -> None:
        self.repository_root = repository_root.resolve()

    def catalog(self, *, include_archive: bool = False) -> DocumentCatalog:
        return load_document_catalog(
            self.repository_root,
            include_archive=include_archive,
        )

    def list_documents(
        self,
        request: ListDocumentsRequest,
    ) -> tuple[Document, ...]:
        catalog = self.catalog(include_archive=request.include_archive)
        return tuple(
            filter_documents(
                catalog.documents,
                under=request.under,
                kinds=request.kinds,
                query=request.query,
            )
        )

    def references(
        self,
        ref: DocumentRef,
        *,
        include_archive: bool = False,
    ) -> DocumentReferences:
        catalog = self.catalog(include_archive=include_archive)
        document = catalog.find(ref)
        if document is None:
            raise DevToolError(f'document was not found: "{ref.as_posix()}"')
        inbound: list[tuple[Document, int]] = []
        for candidate in catalog.documents:
            for link in candidate.links:
                if link.resolved_path == ref.path:
                    inbound.append((candidate, link.line))
        outbound = tuple(
            (
                DocumentRef(link.resolved_path)
                if link.resolved_path is not None
                and link.resolved_path.parts
                and link.resolved_path.parts[0] == "Documentation"
                and link.resolved_path.suffix.casefold() == ".md"
                else None,
                link.line,
                link.target,
            )
            for link in document.links
            if not link.external
        )
        return DocumentReferences(document, tuple(inbound), outbound)

    def validate(
        self,
        *,
        scope: ValidationScope = ValidationScope.ALL,
        include_archive: bool = False,
    ) -> ValidationResult:
        catalog = self.catalog(include_archive=include_archive)
        documents: Sequence[Document] | None = None
        if scope is ValidationScope.CHANGED:
            changed = self._changed_document_paths()
            documents = tuple(
                document
                for document in catalog.documents
                if document.ref.path in changed
            )
        return ValidationResult(
            document_count=(
                len(catalog.documents)
                if documents is None
                else len(documents)
            ),
            diagnostics=tuple(validate_documents(catalog, documents)),
        )

    def prepare_create(
        self,
        *,
        destination: DocumentRef,
        kind: DocumentKind,
        title: str,
        summary: str,
    ) -> DocumentChangeSet:
        return prepare_create(
            self.repository_root,
            destination=destination,
            kind=kind,
            title=title,
            summary=summary,
        )

    def prepare_move(
        self,
        *,
        source: DocumentRef,
        destination: DocumentRef,
    ) -> DocumentChangeSet:
        return prepare_move(
            self.repository_root,
            source=source,
            destination=destination,
        )

    def apply(self, change_set: DocumentChangeSet) -> None:
        def validate_after_change() -> None:
            validation = self.validate(
                scope=ValidationScope.ALL,
                include_archive=True,
            )
            errors = [
                diagnostic
                for diagnostic in validation.diagnostics
                if diagnostic.severity is DiagnosticSeverity.ERROR
            ]
            if errors:
                details = "\n- ".join(
                    f"{diagnostic.path.as_posix()}: {diagnostic.message}"
                    for diagnostic in errors
                )
                raise DevToolError(
                    "document validation failed after applying changes:\n- "
                    + details
                )

        apply_change_set(change_set, validator=validate_after_change)

    def _changed_document_paths(self) -> set[Path]:
        result = subprocess.run(
            [
                "git",
                "-C",
                str(self.repository_root),
                "status",
                "--porcelain=v1",
                "-z",
                "--",
                "Documentation",
            ],
            check=False,
            capture_output=True,
        )
        if result.returncode != 0:
            detail = (
                result.stderr.decode(errors="replace").strip()
                or "git status failed"
            )
            raise DevToolError(
                f"could not discover changed documents: {detail}"
            )
        paths: set[Path] = set()
        entries = result.stdout.split(b"\0")
        index = 0
        while index < len(entries):
            entry = entries[index]
            index += 1
            if not entry:
                continue
            decoded = entry.decode(errors="replace")
            status = decoded[:2]
            path_values = [decoded[3:]]
            if "R" in status or "C" in status:
                if index < len(entries):
                    path_values.append(
                        entries[index].decode(errors="replace")
                    )
                    index += 1
            for path_text in path_values:
                path = Path(path_text)
                if path.suffix.casefold() == ".md":
                    paths.add(path)
        return paths
