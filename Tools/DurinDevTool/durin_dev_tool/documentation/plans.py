"""Implementation-plan parsing, validation, discovery, and rendering."""

from __future__ import annotations

import datetime as dt
import json
import os
import re
import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Iterable, Sequence


SUMMARY_PATTERN = re.compile(r"^Summary: (?P<summary>.+)$")
REVIEWED_PATTERN = re.compile(r"^Last reviewed: (?P<date>\d{4}-\d{2}-\d{2})$")
STATUS_PATTERN = re.compile(r"^Status: (?P<status>Active|Completed|Archived)$")
COMPLETED_PATTERN = re.compile(r"^Completed:(?: (?P<date>\d{4}-\d{2}-\d{2}))?$")
CURRENT_STATUS_SECTION = "## Current Status"
EXCLUDED_PLAN_FILES = {"AGENTS.md"}


class PlanStatus(str, Enum):
    ACTIVE = "Active"
    COMPLETED = "Completed"
    ARCHIVED = "Archived"


@dataclass(frozen=True)
class Plan:
    path: Path
    title: str
    summary: str
    status: PlanStatus
    completed: dt.date | None


@dataclass(frozen=True)
class PlanCatalog:
    plans_directory: Path
    active: tuple[Plan, ...]
    completed: tuple[Plan, ...]
    archived: tuple[Plan, ...]
    errors: tuple[str, ...]

    def select(self, scope: str) -> list[Plan]:
        groups = {
            "active": self.active,
            "completed": self.completed,
            "archive": self.archived,
            "all": self.active + self.completed + self.archived,
        }
        return list(groups[scope])

    def errors_for(self, scope: str) -> list[str]:
        prefixes = ("Archive/",)
        if scope == "archive":
            return [error for error in self.errors if error.startswith(prefixes)]
        if scope in {"active", "completed"}:
            return [error for error in self.errors if not error.startswith(prefixes)]
        return list(self.errors)


def _valid_date(value: str, label: str, errors: list[str]) -> dt.date | None:
    try:
        return dt.date.fromisoformat(value)
    except ValueError:
        errors.append(f"{label} must contain a valid calendar date")
        return None


def parse_plan(
    path: Path,
    *,
    default_status: PlanStatus = PlanStatus.ACTIVE,
    title_suffix: str = " Plan",
    document_label: str = "plan",
) -> tuple[Plan | None, list[str]]:
    """Parse one lifecycle document while collecting useful diagnostics."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except (OSError, UnicodeError) as error:
        return None, [f"could not read UTF-8 {document_label}: {error}"]

    errors: list[str] = []
    title_pattern = re.compile(
        rf"^# (?P<title>.+{re.escape(title_suffix)})$"
    )
    title_match = title_pattern.fullmatch(lines[0]) if lines else None
    if title_match is None:
        errors.append(
            f"first line must be '# <Feature>{title_suffix}'"
        )

    summary_match = SUMMARY_PATTERN.fullmatch(lines[2]) if len(lines) > 2 else None
    if len(lines) < 3 or lines[1] != "" or summary_match is None:
        errors.append("line 3 must be a non-empty 'Summary:' after one blank line")

    reviewed_match = REVIEWED_PATTERN.fullmatch(lines[4]) if len(lines) > 4 else None
    if len(lines) < 5 or lines[3] != "" or reviewed_match is None:
        errors.append("line 5 must be 'Last reviewed: YYYY-MM-DD' after one blank line")
    elif reviewed_match is not None:
        _valid_date(reviewed_match.group("date"), "Last reviewed", errors)

    try:
        current_status_index = lines.index(CURRENT_STATUS_SECTION)
    except ValueError:
        errors.append(f"missing required section '{CURRENT_STATUS_SECTION}'")
        header_lines: list[str] = []
    else:
        header_lines = lines[5:current_status_index]

    status_lines = [line for line in header_lines if line.startswith("Status:")]
    valid_status_matches = [
        match
        for line in status_lines
        if (match := STATUS_PATTERN.fullmatch(line)) is not None
    ]
    if any(STATUS_PATTERN.fullmatch(line) is None for line in status_lines):
        errors.append("Status must be Active, Completed, or Archived")
    if len(status_lines) > 1:
        errors.append("Status must appear at most once")
    status = (
        PlanStatus(valid_status_matches[0].group("status"))
        if len(status_lines) == 1 and valid_status_matches
        else default_status
    )

    completed_lines = [line for line in header_lines if line.startswith("Completed:")]
    if len(completed_lines) > 1:
        errors.append("Completed must appear at most once")
        completed_match = None
    elif completed_lines:
        completed_match = COMPLETED_PATTERN.fullmatch(completed_lines[0])
        if completed_match is None:
            errors.append("Completed must be empty or contain YYYY-MM-DD")
    else:
        completed_match = None

    completed: dt.date | None = None
    if completed_match is not None and completed_match.group("date"):
        completed = _valid_date(completed_match.group("date"), "Completed", errors)

    if (
        status_lines
        and status in {PlanStatus.COMPLETED, PlanStatus.ARCHIVED}
        and completed is None
    ):
        errors.append(
            f"{status.value} {document_label}s must include a Completed date"
        )
    if status is PlanStatus.ACTIVE and completed is not None:
        errors.append(
            f"Active {document_label}s must not include a Completed date"
        )

    if errors or title_match is None or summary_match is None:
        return None, errors
    return (
        Plan(
            path=path,
            title=title_match.group("title").removesuffix(title_suffix),
            summary=summary_match.group("summary"),
            status=status,
            completed=completed,
        ),
        [],
    )


def _sorted_markdown(
    directory: Path,
    *,
    excluded_files: frozenset[str],
) -> list[Path]:
    if not directory.is_dir():
        return []
    return sorted(
        (
            path
            for path in directory.glob("*.md")
            if path.name not in excluded_files
        ),
        key=lambda path: path.as_posix().casefold(),
    )


def _archive_candidates(
    archive_directory: Path,
    *,
    document_label: str,
) -> tuple[list[Path], list[str]]:
    if not archive_directory.is_dir():
        return [], []
    candidates: list[Path] = []
    errors: list[str] = []
    for path in sorted(
        archive_directory.rglob("*.md"),
        key=lambda item: item.as_posix().casefold(),
    ):
        if path.name in EXCLUDED_PLAN_FILES:
            continue
        relative = path.relative_to(archive_directory)
        if len(relative.parts) != 2 or not _is_valid_month(relative.parts[0]):
            errors.append(
                f"Archive/{relative.as_posix()}: archived {document_label} "
                "must be directly "
                "inside an Archive/YYYY-MM directory"
            )
            continue
        candidates.append(path)
    return candidates, errors


def _is_valid_month(value: str) -> bool:
    try:
        return dt.datetime.strptime(value, "%Y-%m").strftime("%Y-%m") == value
    except ValueError:
        return False


def _parse_candidates(
    candidates: Iterable[Path],
    *,
    plans_directory: Path,
    archived: bool,
    title_suffix: str,
    document_label: str,
) -> tuple[list[Plan], list[str]]:
    plans: list[Plan] = []
    errors: list[str] = []
    for path in candidates:
        plan, plan_errors = parse_plan(
            path,
            default_status=PlanStatus.ARCHIVED if archived else PlanStatus.ACTIVE,
            title_suffix=title_suffix,
            document_label=document_label,
        )
        relative = path.relative_to(plans_directory).as_posix()
        errors.extend(f"{relative}: {error}" for error in plan_errors)
        if plan is None:
            continue
        if archived:
            if plan.status is not PlanStatus.ARCHIVED:
                errors.append(
                    f"{relative}: {document_label}s inside Archive must be Archived"
                )
            if (
                plan.completed is not None
                and plan.completed.strftime("%Y-%m") != path.parent.name
            ):
                errors.append(f"{relative}: Completed date must match archive month")
        elif plan.status is PlanStatus.ARCHIVED:
            errors.append(
                f"{relative}: Archived {document_label}s must be moved into Archive"
            )
        plans.append(plan)
    return plans, errors


def load_catalog(
    plans_directory: Path,
    *,
    title_suffix: str = " Plan",
    document_label: str = "plan",
    excluded_files: frozenset[str] = frozenset(EXCLUDED_PLAN_FILES),
) -> PlanCatalog:
    plans_directory = plans_directory.resolve()
    current, current_errors = _parse_candidates(
        _sorted_markdown(
            plans_directory,
            excluded_files=excluded_files,
        ),
        plans_directory=plans_directory,
        archived=False,
        title_suffix=title_suffix,
        document_label=document_label,
    )
    archive_candidates, archive_layout_errors = _archive_candidates(
        plans_directory / "Archive",
        document_label=document_label,
    )
    archived, archive_errors = _parse_candidates(
        archive_candidates,
        plans_directory=plans_directory,
        archived=True,
        title_suffix=title_suffix,
        document_label=document_label,
    )

    errors = current_errors + archive_layout_errors + archive_errors
    all_plans = current + archived
    seen_titles: dict[str, Path] = {}
    for plan in all_plans:
        key = plan.title.casefold()
        if previous := seen_titles.get(key):
            errors.append(
                f"{plan.path.relative_to(plans_directory).as_posix()}: duplicate "
                f"title also used by "
                f"{previous.relative_to(plans_directory).as_posix()}"
            )
        else:
            seen_titles[key] = plan.path

    sort_key = lambda plan: (plan.title.casefold(), plan.path.name.casefold())
    active = tuple(sorted(
        (plan for plan in current if plan.status is PlanStatus.ACTIVE),
        key=sort_key,
    ))
    completed = tuple(sorted(
        (plan for plan in current if plan.status is PlanStatus.COMPLETED),
        key=sort_key,
    ))
    archived_plans = tuple(sorted(archived, key=sort_key))
    return PlanCatalog(
        plans_directory=plans_directory,
        active=active,
        completed=completed,
        archived=archived_plans,
        errors=tuple(errors),
    )


def filter_plans(plans: Sequence[Plan], query: str | None) -> list[Plan]:
    if not query:
        return list(plans)
    needle = query.casefold()
    return [
        plan
        for plan in plans
        if needle in plan.title.casefold() or needle in plan.path.name.casefold()
    ]


def _section_lines(lines: Sequence[str], heading: str) -> list[str]:
    try:
        start = lines.index(heading)
    except ValueError:
        return []
    end = next(
        (
            index
            for index in range(start + 1, len(lines))
            if lines[index].startswith("## ")
        ),
        len(lines),
    )
    return list(lines[start:end])


def _heading_block(lines: Sequence[str], start: int) -> list[str]:
    level = len(lines[start]) - len(lines[start].lstrip("#"))
    end = len(lines)
    for index in range(start + 1, len(lines)):
        match = re.match(r"^(?P<marks>#{1,6}) ", lines[index])
        if match is not None and len(match.group("marks")) <= level:
            end = index
            break
    return list(lines[start:end])


def render_plan_context(
    plan: Plan,
    *,
    repository_root: Path,
    output_format: str,
) -> str:
    """Render the minimum standard context needed to continue one plan."""
    lines = plan.path.read_text(encoding="utf-8").splitlines()
    current_status = _section_lines(lines, CURRENT_STATUS_SECTION)
    related_code = _section_lines(lines, "## Related Code")
    header_end = lines.index(CURRENT_STATUS_SECTION) if current_status else len(lines)
    header = list(lines[:header_end])

    stage_pattern = re.compile(r"^### Stage (?P<number>\d+): (?P<title>.+)$")
    task_pattern = re.compile(r"^\s*- \[(?P<state>[ xX])\] ")
    stage_starts = [
        index
        for index, line in enumerate(lines)
        if stage_pattern.fullmatch(line) is not None
    ]
    stages: list[dict[str, object]] = []
    for offset, start in enumerate(stage_starts):
        next_stage = stage_starts[offset + 1] if offset + 1 < len(stage_starts) else len(lines)
        next_section = next(
            (
                index
                for index in range(start + 1, next_stage)
                if lines[index].startswith("## ")
            ),
            next_stage,
        )
        block = list(lines[start:next_section])
        match = stage_pattern.fullmatch(lines[start])
        assert match is not None
        task_states = [
            task.group("state")
            for line in block
            if (task := task_pattern.match(line)) is not None
        ]
        completed = sum(state.casefold() == "x" for state in task_states)
        stages.append(
            {
                "number": int(match.group("number")),
                "title": match.group("title"),
                "start": start,
                "content": "\n".join(block).strip(),
                "tasks": len(task_states),
                "completedTasks": completed,
                "openTasks": len(task_states) - completed,
            }
        )

    selected = next(
        (stage for stage in stages if int(stage["openTasks"]) > 0),
        stages[-1] if stages else None,
    )
    previous_handoff: list[str] = []
    if selected is not None:
        selected_start = int(selected["start"])
        handoff_starts = [
            index
            for index, line in enumerate(lines[:selected_start])
            if re.match(r"^#{3,6} .*handoff", line, flags=re.IGNORECASE)
        ]
        if handoff_starts:
            previous_handoff = _heading_block(lines, handoff_starts[-1])

    relative_path = plan.path.relative_to(repository_root).as_posix()
    if output_format == "json":
        return json.dumps(
            {
                "schemaVersion": 1,
                "operation": "plan-context",
                "path": relative_path,
                "title": plan.title,
                "summary": plan.summary,
                "status": plan.status.value,
                "completed": plan.completed.isoformat() if plan.completed else None,
                "currentStatus": "\n".join(current_status[1:]).strip(),
                "stages": [
                    {
                        key: value
                        for key, value in stage.items()
                        if key not in {"start", "content"}
                    }
                    for stage in stages
                ],
                "selectedStage": (
                    {
                        key: value
                        for key, value in selected.items()
                        if key != "start"
                    }
                    if selected is not None
                    else None
                ),
                "previousHandoff": "\n".join(previous_handoff).strip() or None,
                "relatedCode": "\n".join(related_code[1:]).strip(),
            },
            indent=2,
        )

    output = ["\n".join(header).strip(), "\n".join(current_status).strip()]
    if stages:
        progress = ["## Stage Progress"]
        for stage in stages:
            progress.append(
                f"- Stage {stage['number']}: {stage['title']} - "
                f"{stage['completedTasks']}/{stage['tasks']} tasks complete; "
                f"{stage['openTasks']} open"
            )
        output.append("\n".join(progress))
    if selected is not None:
        output.append("## Selected Current Stage\n\n" + str(selected["content"]))
    if previous_handoff:
        output.append(
            "## Previous Handoff Context\n\n"
            + "\n".join(previous_handoff).strip()
        )
    if related_code:
        output.append("\n".join(related_code).strip())
    return "\n\n".join(section for section in output if section)


def _markdown_cell(value: str) -> str:
    return value.replace("\\", "\\\\").replace("|", "\\|")


def render_markdown(
    plans: Sequence[Plan],
    plans_directory: Path,
    *,
    document_label: str = "plan",
) -> str:
    lines = [
        f"| {document_label.title()} | Primary Scope |",
        "| --- | --- |",
    ]
    for plan in plans:
        link = plan.path.relative_to(plans_directory).as_posix()
        lines.append(
            f"| [{_markdown_cell(plan.title)}]({link}) | "
            f"{_markdown_cell(plan.summary)} |"
        )
    return "\n".join(lines)


def _color_enabled(mode: str) -> bool:
    if mode == "always":
        return True
    if mode == "never":
        return False
    return (
        sys.stdout.isatty()
        and "NO_COLOR" not in os.environ
        and os.environ.get("TERM") != "dumb"
    )


def _styled(text: str, code: str, enabled: bool) -> str:
    return f"\033[{code}m{text}\033[0m" if enabled else text


def render_terminal(
    plans: Sequence[Plan],
    plans_directory: Path,
    *,
    color: str,
) -> str:
    use_color = _color_enabled(color)
    status_colors = {
        PlanStatus.ACTIVE: "32",
        PlanStatus.COMPLETED: "33",
        PlanStatus.ARCHIVED: "2",
    }
    repository = plans_directory.parent.parent
    lines: list[str] = []
    for plan in plans:
        status = f"[{plan.status.value}]".ljust(11)
        lines.extend(
            (
                f"{_styled(status, status_colors[plan.status], use_color)}  {plan.title}",
                f"  {_styled(plan.path.relative_to(repository).as_posix(), '36', use_color)}",
                f"  {plan.summary}",
                "",
            )
        )
    return "\n".join(lines).rstrip()


def render_listing(
    plans: Sequence[Plan],
    plans_directory: Path,
    *,
    scope: str,
    output_format: str,
    color: str,
    document_label: str = "plan",
) -> str:
    title_label = document_label.title()

    def archive_markdown(archive_plans: Sequence[Plan]) -> str:
        months = sorted(
            {plan.path.parent.name for plan in archive_plans},
            reverse=True,
        )
        return "\n\n".join(
            f"## {month}\n\n"
            + render_markdown(
                [
                    plan
                    for plan in archive_plans
                    if plan.path.parent.name == month
                ],
                plans_directory,
                document_label=document_label,
            )
            for month in months
        )

    def archive_terminal(archive_plans: Sequence[Plan]) -> str:
        use_color = _color_enabled(color)
        months = sorted(
            {plan.path.parent.name for plan in archive_plans},
            reverse=True,
        )
        return "\n\n".join(
            f"{_styled(month, '1', use_color)}\n\n"
            f"{render_terminal([plan for plan in archive_plans if plan.path.parent.name == month], plans_directory, color=color)}"
            for month in months
        )

    if output_format == "markdown":
        if scope == "archive":
            return archive_markdown(plans)
        if scope != "all":
            return render_markdown(
                plans,
                plans_directory,
                document_label=document_label,
            )
        groups = (
            (
                f"Active {title_label}s",
                render_markdown(
                    [plan for plan in plans if plan.status is PlanStatus.ACTIVE],
                    plans_directory,
                    document_label=document_label,
                ),
            ),
            (
                f"Completed {title_label}s Awaiting Archive",
                render_markdown(
                    [plan for plan in plans if plan.status is PlanStatus.COMPLETED],
                    plans_directory,
                    document_label=document_label,
                ),
            ),
            (
                f"Archived {title_label}s",
                archive_markdown(
                    [plan for plan in plans if plan.status is PlanStatus.ARCHIVED]
                ),
            ),
        )
        return "\n\n".join(
            f"# {heading}\n\n{body}" for heading, body in groups
        )

    use_color = _color_enabled(color)
    if scope == "archive":
        return archive_terminal(plans)
    if scope != "all":
        return render_terminal(plans, plans_directory, color=color)
    groups = (
        (
            f"Active {title_label}s",
            render_terminal(
                [plan for plan in plans if plan.status is PlanStatus.ACTIVE],
                plans_directory,
                color=color,
            ),
        ),
        (
            f"Completed {title_label}s Awaiting Archive",
            render_terminal(
                [plan for plan in plans if plan.status is PlanStatus.COMPLETED],
                plans_directory,
                color=color,
            ),
        ),
        (
            f"Archived {title_label}s",
            archive_terminal(
                [plan for plan in plans if plan.status is PlanStatus.ARCHIVED]
            ),
        ),
    )
    sections = []
    for heading, body in groups:
        sections.append(f"{_styled(heading, '1', use_color)}\n\n{body}".rstrip())
    return "\n\n".join(sections)
