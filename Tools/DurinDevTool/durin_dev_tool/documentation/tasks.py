"""Open-task discovery and mechanical validation."""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

from .model import Diagnostic, DiagnosticSeverity


TITLE_PATTERN = re.compile(r"^# (?P<title>\S.*)$")
REQUIRED_SECTIONS = (
    "Outcome",
    "Evidence",
    "Required Changes",
    "Protected Invariants",
    "Likely Working Set",
    "Acceptance",
)
RESERVED_LINES = re.compile(
    r"^(?:Status|Completed|Last reviewed|Plan|Stage):(?:\s|$)"
)
RESERVED_SECTIONS = {"Current Status", "Progress", "Stages"}
EXCLUDED_TASK_FILES = {"AGENTS.md", "README.md"}


@dataclass(frozen=True)
class Task:
    path: Path
    title: str
    summary: str


@dataclass(frozen=True)
class TaskCatalog:
    tasks_directory: Path
    tasks: tuple[Task, ...]
    diagnostics: tuple[Diagnostic, ...]


def _repository_path(tasks_directory: Path, path: Path) -> Path:
    repository = tasks_directory.parent.parent
    return path.relative_to(repository)


def _diagnostic(
    tasks_directory: Path,
    path: Path,
    code: str,
    message: str,
    *,
    line: int | None = None,
) -> Diagnostic:
    return Diagnostic(
        code=code,
        severity=DiagnosticSeverity.ERROR,
        message=message,
        path=_repository_path(tasks_directory, path),
        line=line,
    )


def _visible_lines(lines: Sequence[str]) -> Iterable[tuple[int, str]]:
    in_fence = False
    for line_number, line in enumerate(lines, start=1):
        stripped = line.lstrip()
        if stripped.startswith(("```", "~~~")):
            in_fence = not in_fence
            continue
        if not in_fence:
            yield line_number, line


def _meaningful_lines(lines: Sequence[str]) -> list[str]:
    meaningful: list[str] = []
    in_comment = False
    for line in lines:
        stripped = line.strip()
        if not stripped:
            continue
        if in_comment:
            if "-->" in stripped:
                in_comment = False
            continue
        if stripped.startswith("<!--"):
            if "-->" not in stripped:
                in_comment = True
            continue
        meaningful.append(stripped)
    return meaningful


def _first_paragraph(lines: Sequence[str]) -> str:
    paragraph: list[str] = []
    started = False
    in_comment = False
    for line in lines:
        stripped = line.strip()
        if in_comment:
            if "-->" in stripped:
                in_comment = False
            continue
        if stripped.startswith("<!--"):
            if "-->" not in stripped:
                in_comment = True
            continue
        if not stripped:
            if started:
                break
            continue
        started = True
        paragraph.append(stripped)
    return " ".join(paragraph)


def parse_task(
    path: Path,
    *,
    tasks_directory: Path | None = None,
) -> tuple[Task | None, list[Diagnostic]]:
    """Parse one task while collecting all useful structural diagnostics."""
    owning_directory = (tasks_directory or path.parent).resolve()
    path = path.resolve()
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        return None, [
            _diagnostic(
                owning_directory,
                path,
                "doc.task.read_failed",
                f"could not read UTF-8 task: {error}",
            )
        ]

    diagnostics: list[Diagnostic] = []
    title_match = TITLE_PATTERN.fullmatch(lines[0]) if lines else None
    if title_match is None:
        diagnostics.append(
            _diagnostic(
                owning_directory,
                path,
                "doc.task.title_invalid",
                "first line must be a non-empty level-one Markdown title",
                line=1,
            )
        )

    visible = list(_visible_lines(lines))
    headings: list[tuple[int, str]] = []
    for line_number, line in visible:
        if line.startswith("## "):
            headings.append((line_number, line.removeprefix("## ").strip()))
        if RESERVED_LINES.match(line):
            diagnostics.append(
                _diagnostic(
                    owning_directory,
                    path,
                    "doc.task.lifecycle_metadata",
                    "task documents must not contain lifecycle or plan metadata",
                    line=line_number,
                )
            )

    for line_number, heading in headings:
        if heading in RESERVED_SECTIONS:
            diagnostics.append(
                _diagnostic(
                    owning_directory,
                    path,
                    "doc.task.lifecycle_section",
                    f'task documents must not contain the "{heading}" section',
                    line=line_number,
                )
            )

    section_positions: list[int] = []
    sections: dict[str, list[str]] = {}
    for required in REQUIRED_SECTIONS:
        matches = [
            (line_number, index)
            for index, (line_number, heading) in enumerate(headings)
            if heading == required
        ]
        if not matches:
            diagnostics.append(
                _diagnostic(
                    owning_directory,
                    path,
                    "doc.task.section_missing",
                    f'missing required section "## {required}"',
                )
            )
            continue
        if len(matches) > 1:
            diagnostics.append(
                _diagnostic(
                    owning_directory,
                    path,
                    "doc.task.section_duplicate",
                    f'section "## {required}" must appear exactly once',
                    line=matches[1][0],
                )
            )
        line_number, heading_index = matches[0]
        section_positions.append(heading_index)
        next_line = (
            headings[heading_index + 1][0] - 1
            if heading_index + 1 < len(headings)
            else len(lines)
        )
        content = lines[line_number:next_line]
        sections[required] = content
        if not _meaningful_lines(content):
            diagnostics.append(
                _diagnostic(
                    owning_directory,
                    path,
                    "doc.task.section_empty",
                    f'section "## {required}" must contain content',
                    line=line_number,
                )
            )

    if section_positions != sorted(section_positions):
        diagnostics.append(
            _diagnostic(
                owning_directory,
                path,
                "doc.task.section_order",
                "required task sections must use the repository-defined order",
            )
        )

    if title_match is None:
        return None, diagnostics
    return (
        Task(
            path=path,
            title=title_match.group("title"),
            summary=_first_paragraph(sections.get("Outcome", ())),
        ),
        diagnostics,
    )


def _task_candidates(tasks_directory: Path) -> list[Path]:
    if not tasks_directory.is_dir():
        return []
    return sorted(
        (
            path
            for path in tasks_directory.rglob("*.md")
            if not (
                path.parent == tasks_directory
                and path.name in EXCLUDED_TASK_FILES
            )
        ),
        key=lambda candidate: candidate.as_posix().casefold(),
    )


def load_task_catalog(tasks_directory: Path) -> TaskCatalog:
    tasks_directory = tasks_directory.resolve()
    diagnostics: list[Diagnostic] = []
    tasks: list[Task] = []
    seen_titles: dict[str, Path] = {}
    seen_filenames: dict[str, Path] = {}
    legacy_index = tasks_directory / "README.md"
    if legacy_index.exists():
        diagnostics.append(
            _diagnostic(
                tasks_directory,
                legacy_index,
                "doc.task.index_unsupported",
                "open tasks are discovered directly; do not maintain a task index",
            )
        )

    for path in _task_candidates(tasks_directory):
        if path.parent != tasks_directory:
            diagnostics.append(
                _diagnostic(
                    tasks_directory,
                    path,
                    "doc.task.layout_invalid",
                    "task documents must be direct children of Documentation/Tasks",
                )
            )
        task, task_diagnostics = parse_task(
            path,
            tasks_directory=tasks_directory,
        )
        diagnostics.extend(task_diagnostics)
        if task is None:
            continue
        filename_key = path.name.casefold()
        if previous := seen_filenames.get(filename_key):
            diagnostics.append(
                _diagnostic(
                    tasks_directory,
                    path,
                    "doc.task.filename_duplicate",
                    f"task filename duplicates {previous.name} ignoring case",
                )
            )
        else:
            seen_filenames[filename_key] = path
        title_key = task.title.casefold()
        if previous := seen_titles.get(title_key):
            diagnostics.append(
                _diagnostic(
                    tasks_directory,
                    path,
                    "doc.task.title_duplicate",
                    f"task title duplicates {previous.name} ignoring case",
                )
            )
        else:
            seen_titles[title_key] = path
        tasks.append(task)

    return TaskCatalog(
        tasks_directory=tasks_directory,
        tasks=tuple(
            sorted(
                tasks,
                key=lambda task: (
                    task.title.casefold(),
                    task.path.name.casefold(),
                ),
            )
        ),
        diagnostics=tuple(diagnostics),
    )


def filter_tasks(tasks: Sequence[Task], query: str | None) -> list[Task]:
    if not query:
        return list(tasks)
    needle = query.casefold()
    return [
        task
        for task in tasks
        if needle in task.title.casefold()
        or needle in task.path.name.casefold()
        or needle in task.summary.casefold()
    ]
