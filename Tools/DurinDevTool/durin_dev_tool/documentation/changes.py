"""Previewable, fingerprint-checked transactions for Markdown structure changes."""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from ..errors import DevToolError
from .model import DocumentKind, DocumentRef, infer_document_kind


MARKDOWN_LINK_PATTERN = re.compile(
    r"(?P<prefix>!?\[[^\]\n]*\]\()(?P<target>[^)\n]+)(?P<suffix>\))"
)
REFERENCE_LINK_PATTERN = re.compile(
    r"(?P<prefix>^\s*\[[^\]]+\]:\s*)(?P<target>\S+)(?P<suffix>.*)$",
    re.MULTILINE,
)


@dataclass(frozen=True)
class FileChange:
    source: Path | None
    destination: Path
    content: bytes
    expected_source_hash: str | None


@dataclass(frozen=True)
class FilePrecondition:
    path: Path
    expected_hash: str


@dataclass(frozen=True)
class DocumentChangeSet:
    operation: str
    changes: tuple[FileChange, ...]
    preconditions: tuple[FilePrecondition, ...]
    primary_source: Path | None
    primary_destination: Path

    @property
    def reference_files(self) -> tuple[Path, ...]:
        return tuple(
            change.destination
            for change in self.changes
            if change.destination != self.primary_destination
        )


def _content_hash(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


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


def rewrite_link_target(
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
    relative = os.path.relpath(destination, output_document.parent).replace(
        os.sep,
        "/",
    )
    if enclosed:
        fragment, marker, trailing = suffix.partition(">")
        return f"<{relative}{fragment}>{trailing}" if marker else target
    return f"{relative}{suffix}"


def rewrite_markdown_links(
    text: str,
    *,
    document: Path,
    output_document: Path,
    moves: dict[Path, Path],
) -> str:
    def replace(match: re.Match[str]) -> str:
        target = rewrite_link_target(
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


def rewrite_repository_paths(
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


def repository_markdown_files(repository: Path) -> list[Path]:
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
        detail = (
            result.stderr.decode(errors="replace").strip()
            or "git ls-files failed"
        )
        raise DevToolError(
            f"could not discover repository Markdown files: {detail}"
        )
    return sorted(
        (repository / os.fsdecode(raw_path)).resolve()
        for raw_path in result.stdout.split(b"\0")
        if raw_path
    )


def atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.",
        dir=path.parent,
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def prepare_create(
    repository_root: Path,
    *,
    destination: DocumentRef,
    kind: DocumentKind,
    title: str,
    summary: str,
) -> DocumentChangeSet:
    absolute_destination = (repository_root / destination.path).resolve()
    documentation_root = (repository_root / "Documentation").resolve()
    try:
        absolute_destination.relative_to(documentation_root)
    except ValueError as error:
        raise DevToolError(
            "document destination resolves outside Documentation"
        ) from error
    if absolute_destination.exists():
        raise DevToolError(
            f'document already exists: "{destination.as_posix()}"'
        )
    if kind in {
        DocumentKind.PLAN,
        DocumentKind.INVESTIGATION,
        DocumentKind.POLICY,
    }:
        raise DevToolError(
            f'doc create does not scaffold "{kind.value}" documents; '
            "use the owning workflow"
        )
    inferred_kind = infer_document_kind(destination.path)
    if inferred_kind in {
        DocumentKind.PLAN,
        DocumentKind.INVESTIGATION,
        DocumentKind.POLICY,
    }:
        raise DevToolError(
            f'"{destination.as_posix()}" is owned by the '
            f"{inferred_kind.value} workflow"
        )
    if inferred_kind is not kind:
        raise DevToolError(
            f'destination is classified as "{inferred_kind.value}", '
            f'not "{kind.value}"'
        )
    clean_title = title.strip()
    clean_summary = summary.strip()
    if not clean_title:
        raise DevToolError("document title must not be empty")
    body = f"# {clean_title}\n"
    if clean_summary:
        body += f"\n{clean_summary}\n"
    else:
        body += "\n"
    content = body.encode("utf-8")
    return DocumentChangeSet(
        operation="create",
        changes=(
            FileChange(
                source=None,
                destination=absolute_destination,
                content=content,
                expected_source_hash=None,
            ),
        ),
        preconditions=(),
        primary_source=None,
        primary_destination=absolute_destination,
    )


def prepare_move(
    repository_root: Path,
    *,
    source: DocumentRef,
    destination: DocumentRef,
) -> DocumentChangeSet:
    repository_root = repository_root.resolve()
    absolute_source = (repository_root / source.path).resolve()
    absolute_destination = (repository_root / destination.path).resolve()
    documentation_root = (repository_root / "Documentation").resolve()
    for label, path in (
        ("source", absolute_source),
        ("destination", absolute_destination),
    ):
        try:
            path.relative_to(documentation_root)
        except ValueError as error:
            raise DevToolError(
                f"document {label} resolves outside Documentation"
            ) from error
    if not absolute_source.is_file():
        raise DevToolError(f'document does not exist: "{source.as_posix()}"')
    if absolute_destination.exists():
        raise DevToolError(
            f'document destination already exists: "{destination.as_posix()}"'
        )
    for ref in (source, destination):
        kind = infer_document_kind(ref.path)
        if kind in {
            DocumentKind.PLAN,
            DocumentKind.INVESTIGATION,
            DocumentKind.POLICY,
        }:
            raise DevToolError(
                f"{kind.value} documents must be moved through their "
                "owning workflow"
            )

    move_map = {absolute_source: absolute_destination}
    markdown_files = set(repository_markdown_files(repository_root))
    markdown_files.add(absolute_source)
    preconditions: list[FilePrecondition] = []
    changes: list[FileChange] = []
    for document in sorted(markdown_files):
        try:
            original = document.read_bytes()
            text = original.decode("utf-8")
        except (OSError, UnicodeError) as error:
            raise DevToolError(
                f'could not read Markdown file "{document}": {error}'
            ) from error
        source_hash = _content_hash(original)
        preconditions.append(FilePrecondition(document, source_hash))
        output_document = move_map.get(document, document)
        rewritten = rewrite_repository_paths(
            rewrite_markdown_links(
                text,
                document=document,
                output_document=output_document,
                moves=move_map,
            ),
            repository=repository_root,
            moves=move_map,
        ).encode("utf-8")
        if rewritten != original or document == absolute_source:
            changes.append(
                FileChange(
                    source=document,
                    destination=output_document,
                    content=rewritten,
                    expected_source_hash=source_hash,
                )
            )
    return DocumentChangeSet(
        operation="move",
        changes=tuple(changes),
        preconditions=tuple(preconditions),
        primary_source=absolute_source,
        primary_destination=absolute_destination,
    )


def apply_change_set(
    change_set: DocumentChangeSet,
    *,
    validator: Callable[[], None] | None = None,
) -> None:
    for precondition in change_set.preconditions:
        if not precondition.path.is_file():
            raise DevToolError(
                "document input disappeared after preview: "
                f'"{precondition.path}"'
            )
        if (
            _content_hash(precondition.path.read_bytes())
            != precondition.expected_hash
        ):
            raise DevToolError(
                f'document input was modified after preview: "{precondition.path}"'
            )
    for change in change_set.changes:
        if change.source is None:
            if change.destination.exists():
                raise DevToolError(
                    f'change destination appeared after preview: "{change.destination}"'
                )
            continue
        if not change.source.is_file():
            raise DevToolError(
                f'change source disappeared after preview: "{change.source}"'
            )
        if _content_hash(change.source.read_bytes()) != change.expected_source_hash:
            raise DevToolError(
                f'change source was modified after preview: "{change.source}"'
            )
        if (
            change.source != change.destination
            and change.destination.exists()
        ):
            raise DevToolError(
                f'change destination appeared after preview: "{change.destination}"'
            )

    touched = {
        path
        for change in change_set.changes
        for path in (change.source, change.destination)
        if path is not None
    }
    snapshots = {
        path: path.read_bytes() if path.exists() else None for path in touched
    }
    created_directories: set[Path] = set()
    try:
        for change in change_set.changes:
            candidate = change.destination.parent
            while not candidate.exists():
                created_directories.add(candidate)
                candidate = candidate.parent
            atomic_write(change.destination, change.content)
        for change in change_set.changes:
            if (
                change.source is not None
                and change.source != change.destination
            ):
                change.source.unlink()
        if validator is not None:
            validator()
    except BaseException:
        for path, content in snapshots.items():
            if content is None:
                path.unlink(missing_ok=True)
            else:
                atomic_write(path, content)
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
