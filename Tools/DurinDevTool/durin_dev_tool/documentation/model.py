"""Transport-independent models for repository documentation operations."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from pathlib import Path


class DocumentKind(str, Enum):
    ROUTER = "router"
    CONTRACT = "contract"
    GUIDE = "guide"
    TASK = "task"
    PLAN = "plan"
    ROADMAP = "roadmap"
    INVESTIGATION = "investigation"
    POLICY = "policy"
    GENERIC = "generic"


class DiagnosticSeverity(str, Enum):
    ERROR = "error"
    WARNING = "warning"


@dataclass(frozen=True)
class DocumentRef:
    path: Path

    @classmethod
    def parse(cls, value: str | Path) -> "DocumentRef":
        path = Path(value)
        if path.is_absolute():
            raise ValueError("document paths must be repository-relative")
        normalized = Path(*path.parts)
        if not normalized.parts or normalized.parts[0] != "Documentation":
            raise ValueError('document paths must be inside "Documentation"')
        if ".." in normalized.parts:
            raise ValueError("document paths must not leave Documentation")
        if normalized.suffix.casefold() != ".md":
            raise ValueError("document paths must use the .md extension")
        return cls(normalized)

    def as_posix(self) -> str:
        return self.path.as_posix()


@dataclass(frozen=True)
class DocumentLink:
    target: str
    line: int
    resolved_path: Path | None
    external: bool = False


@dataclass(frozen=True)
class Document:
    ref: DocumentRef
    kind: DocumentKind
    title: str
    links: tuple[DocumentLink, ...]
    summary: str = ""
    modules: tuple[str, ...] = ()
    keywords: tuple[str, ...] = ()
    search_text: str = ""


@dataclass(frozen=True)
class Diagnostic:
    code: str
    severity: DiagnosticSeverity
    message: str
    path: Path
    line: int | None = None
    target: str | None = None


@dataclass(frozen=True)
class DocumentCatalog:
    repository_root: Path
    documents: tuple[Document, ...]
    diagnostics: tuple[Diagnostic, ...]

    def find(self, ref: DocumentRef) -> Document | None:
        key = ref.as_posix().casefold()
        return next(
            (
                document
                for document in self.documents
                if document.ref.as_posix().casefold() == key
            ),
            None,
        )


def infer_document_kind(path: Path) -> DocumentKind:
    parts = path.parts
    if path.name.casefold() == "agents.md":
        return DocumentKind.POLICY
    if path.name.casefold() == "readme.md":
        return DocumentKind.ROUTER
    if len(parts) > 1 and parts[1] == "Tasks":
        return DocumentKind.TASK
    if len(parts) > 1 and parts[1] == "Plans":
        return DocumentKind.PLAN
    if len(parts) > 1 and parts[1] == "Roadmaps":
        return DocumentKind.ROADMAP
    if len(parts) > 1 and parts[1] == "Investigations":
        return DocumentKind.INVESTIGATION
    if len(parts) > 2 and parts[1:3] == ("Editor", "Guides"):
        return DocumentKind.GUIDE
    if len(parts) > 1 and parts[1] in {
        "Development",
        "Editor",
        "Runtime",
        "Workspace",
    }:
        return DocumentKind.CONTRACT
    return DocumentKind.GENERIC
