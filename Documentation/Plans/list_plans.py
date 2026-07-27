#!/usr/bin/env python3
"""List and validate active or archived implementation plans."""

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path


TITLE_PATTERN = re.compile(r"^# (?P<title>.+ Plan)$")
SUMMARY_PATTERN = re.compile(r"^Summary: (?P<summary>.+)$")
REVIEWED_PATTERN = re.compile(r"^Last reviewed: (?P<date>\d{4}-\d{2}-\d{2})$")
STATUS_PATTERN = re.compile(r"^Status: (?P<status>Active|Completed|Archived)$")
COMPLETED_PATTERN = re.compile(
    r"^Completed:(?: (?P<date>\d{4}-\d{2}-\d{2}))?$"
)
REQUIRED_SECTION = "## Current Status"
EXCLUDED_FILES = {"AGENTS.md"}


@dataclass(frozen=True)
class Plan:
    path: Path
    title: str
    summary: str
    status: str
    completed: dt.date | None


def parse_plan(
    path: Path,
    *,
    default_status: str = "Active",
) -> tuple[Plan | None, list[str]]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    errors: list[str] = []

    title_match = TITLE_PATTERN.fullmatch(lines[0]) if lines else None
    if title_match is None:
        errors.append("first line must be '# <Feature> Plan'")

    summary_match = SUMMARY_PATTERN.fullmatch(lines[2]) if len(lines) > 2 else None
    if len(lines) < 2 or lines[1] != "" or summary_match is None:
        errors.append("line 3 must be a non-empty 'Summary:' after one blank line")

    reviewed_match = REVIEWED_PATTERN.fullmatch(lines[4]) if len(lines) > 4 else None
    if len(lines) < 4 or lines[3] != "" or reviewed_match is None:
        errors.append("line 5 must be 'Last reviewed: YYYY-MM-DD' after one blank line")
    elif reviewed_match is not None:
        try:
            dt.date.fromisoformat(reviewed_match.group("date"))
        except ValueError:
            errors.append("Last reviewed must contain a valid calendar date")

    if REQUIRED_SECTION not in lines:
        errors.append(f"missing required section '{REQUIRED_SECTION}'")
        header_lines: list[str] = []
    else:
        header_lines = lines[5 : lines.index(REQUIRED_SECTION)]

    status_matches = [
        STATUS_PATTERN.fullmatch(line)
        for line in header_lines
        if line.startswith("Status:")
    ]
    status_matches = [match for match in status_matches if match is not None]
    invalid_status_lines = [
        line
        for line in header_lines
        if line.startswith("Status:") and STATUS_PATTERN.fullmatch(line) is None
    ]
    if invalid_status_lines:
        errors.append("Status must be Active, Completed, or Archived")
    if len(status_matches) > 1:
        errors.append("Status must appear at most once")
    status = (
        status_matches[0].group("status") if status_matches else default_status
    )

    completed_lines = [
        line for line in header_lines if line.startswith("Completed:")
    ]
    completed_match = (
        COMPLETED_PATTERN.fullmatch(completed_lines[0])
        if len(completed_lines) == 1
        else None
    )
    if len(completed_lines) > 1:
        errors.append("Completed must appear at most once")
    elif completed_lines and completed_match is None:
        errors.append("Completed must be empty or contain YYYY-MM-DD")

    completed: dt.date | None = None
    if completed_match is not None and completed_match.group("date") is not None:
        try:
            completed = dt.date.fromisoformat(completed_match.group("date"))
        except ValueError:
            errors.append("Completed must contain a valid calendar date")

    if status in ("Completed", "Archived") and completed is None and status_matches:
        errors.append(f"{status} plans must include a Completed date")
    if status == "Active" and completed is not None:
        errors.append("Active plans must not include a Completed date")

    if errors or title_match is None or summary_match is None:
        return None, errors

    return (
        Plan(
            path=path,
            title=title_match.group("title").removesuffix(" Plan"),
            summary=summary_match.group("summary"),
            status=status,
            completed=completed,
        ),
        errors,
    )


def discover_plans(
    directory: Path,
    *,
    recursive: bool = False,
    require_archive_month: bool = False,
) -> tuple[list[Plan], list[str]]:
    plans: list[Plan] = []
    errors: list[str] = []

    candidates = directory.rglob("*.md") if recursive else directory.glob("*.md")
    for path in sorted(candidates, key=lambda item: item.as_posix().casefold()):
        if path.name in EXCLUDED_FILES:
            continue
        if require_archive_month:
            month = path.parent.name
            valid_month = re.fullmatch(r"\d{4}-\d{2}", month) is not None
            if valid_month:
                try:
                    dt.datetime.strptime(month, "%Y-%m")
                except ValueError:
                    valid_month = False
            if not valid_month:
                errors.append(
                    f"{path.relative_to(directory).as_posix()}: archived plan must "
                    "be directly inside an Archive/YYYY-MM directory"
                )
        plan, plan_errors = parse_plan(
            path,
            default_status="Archived" if require_archive_month else "Active",
        )
        display_path = path.relative_to(directory).as_posix()
        errors.extend(f"{display_path}: {error}" for error in plan_errors)
        if plan is not None:
            if require_archive_month and plan.status != "Archived":
                errors.append(
                    f"{display_path}: plans inside Archive must be Archived"
                )
            if (
                require_archive_month
                and plan.completed is not None
                and plan.completed.strftime("%Y-%m") != path.parent.name
            ):
                errors.append(
                    f"{display_path}: Completed date must match archive month"
                )
            if not require_archive_month and plan.status == "Archived":
                errors.append(
                    f"{display_path}: Archived plans must be moved into Archive"
                )
            plans.append(plan)

    seen_titles: dict[str, Path] = {}
    for plan in plans:
        normalized_title = plan.title.casefold()
        if normalized_title in seen_titles:
            errors.append(
                f"{plan.path.name}: duplicate title also used by "
                f"{seen_titles[normalized_title].name}"
            )
        else:
            seen_titles[normalized_title] = plan.path

    plans.sort(key=lambda plan: (plan.title.casefold(), plan.path.name.casefold()))
    return plans, errors


def markdown_cell(value: str) -> str:
    return value.replace("\\", "\\\\").replace("|", "\\|")


def render_markdown(plans: list[Plan], plans_directory: Path) -> str:
    lines = ["| Plan | Primary Scope |", "| --- | --- |"]
    for plan in plans:
        title = markdown_cell(plan.title)
        summary = markdown_cell(plan.summary)
        link = plan.path.relative_to(plans_directory).as_posix()
        lines.append(f"| [{title}]({link}) | {summary} |")
    return "\n".join(lines)


def render_archive_markdown(plans: list[Plan], plans_directory: Path) -> str:
    months = sorted({plan.path.parent.name for plan in plans}, reverse=True)
    sections: list[str] = []
    for month in months:
        month_plans = [plan for plan in plans if plan.path.parent.name == month]
        sections.append(f"## {month}\n\n{render_markdown(month_plans, plans_directory)}")
    return "\n\n".join(sections)


def color_enabled(mode: str) -> bool:
    if mode == "always":
        return True
    if mode == "never":
        return False
    return (
        sys.stdout.isatty()
        and "NO_COLOR" not in os.environ
        and os.environ.get("TERM") != "dumb"
    )


def styled(text: str, code: str, enabled: bool) -> str:
    return f"\033[{code}m{text}\033[0m" if enabled else text


def render_terminal(
    plans: list[Plan],
    plans_directory: Path,
    *,
    use_color: bool,
) -> str:
    status_colors = {
        "Active": "32",
        "Completed": "33",
        "Archived": "2",
    }
    repository = plans_directory.parent.parent
    lines: list[str] = []
    for plan in plans:
        status = f"[{plan.status}]".ljust(11)
        lines.append(
            f"{styled(status, status_colors[plan.status], use_color)}  "
            f"{plan.title}"
        )
        path = plan.path.relative_to(repository).as_posix()
        lines.append(f"  {styled(path, '36', use_color)}")
        lines.append(f"  {plan.summary}")
        lines.append("")
    return "\n".join(lines).rstrip()


def render_archive_terminal(
    plans: list[Plan],
    plans_directory: Path,
    *,
    use_color: bool,
) -> str:
    months = sorted({plan.path.parent.name for plan in plans}, reverse=True)
    sections: list[str] = []
    for month in months:
        month_plans = [plan for plan in plans if plan.path.parent.name == month]
        heading = styled(month, "1", use_color)
        body = render_terminal(
            month_plans,
            plans_directory,
            use_color=use_color,
        )
        sections.append(f"{heading}\n\n{body}")
    return "\n\n".join(sections)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate or validate implementation-plan indexes."
    )
    parser.add_argument(
        "--scope",
        choices=("active", "completed", "archive", "all"),
        default="active",
        help="select plans to list or validate (default: active)",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="validate plans without printing the generated Markdown index",
    )
    parser.add_argument(
        "--query",
        help="filter listed plans by a title or filename substring",
    )
    parser.add_argument(
        "--all-results",
        action="store_true",
        help="explicitly allow an unfiltered archive or all-scope listing",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "terminal"),
        default="markdown",
        dest="output_format",
        help="select Markdown or human-oriented terminal output",
    )
    parser.add_argument(
        "--color",
        choices=("auto", "always", "never"),
        default="auto",
        help="control ANSI colors in terminal output (default: auto)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.validate and (args.query or args.all_results):
        print(
            "error: --validate cannot be combined with --query or --all-results",
            file=sys.stderr,
        )
        return 2
    if (
        args.scope in ("archive", "all")
        and not args.validate
        and not args.query
        and not args.all_results
    ):
        print(
            "error: archive listings require --query <title-or-filename>; "
            "use --all-results only for an explicitly requested full listing",
            file=sys.stderr,
        )
        return 2

    plans_directory = Path(__file__).resolve().parent
    active_plans, active_errors = discover_plans(plans_directory)
    archive_plans, archive_errors = discover_plans(
        plans_directory / "Archive",
        recursive=True,
        require_archive_month=True,
    )
    active_plans, completed_plans = (
        [plan for plan in active_plans if plan.status == "Active"],
        [plan for plan in active_plans if plan.status == "Completed"],
    )

    errors: list[str] = []
    if args.scope in ("active", "completed", "all"):
        errors.extend(active_errors)
    if args.scope in ("archive", "all"):
        errors.extend(f"Archive/{error}" for error in archive_errors)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    if args.query:
        query = args.query.casefold()

        def matches_query(plan: Plan) -> bool:
            return (
                query in plan.title.casefold()
                or query in plan.path.name.casefold()
            )

        active_plans = [plan for plan in active_plans if matches_query(plan)]
        completed_plans = [plan for plan in completed_plans if matches_query(plan)]
        archive_plans = [plan for plan in archive_plans if matches_query(plan)]

    selected_plans = active_plans if args.scope == "active" else archive_plans
    if args.scope == "completed":
        selected_plans = completed_plans
    if args.scope == "all":
        selected_plans = active_plans + completed_plans + archive_plans
    if not selected_plans:
        if args.query:
            print(
                f"error: no {args.scope} plans match query {args.query!r}",
                file=sys.stderr,
            )
            return 1
        if args.scope == "completed":
            if args.validate:
                print("Validated 0 completed implementation plans.")
            else:
                print("No completed implementation plans are awaiting archival.")
            return 0
        print(f"error: no {args.scope} implementation plans found", file=sys.stderr)
        return 1

    if args.validate:
        if args.scope == "all":
            print(
                f"Validated {len(active_plans)} active, "
                f"{len(completed_plans)} completed, and "
                f"{len(archive_plans)} archived implementation plans."
            )
        else:
            print(f"Validated {len(selected_plans)} {args.scope} implementation plans.")
    elif args.output_format == "terminal":
        use_color = color_enabled(args.color)
        if args.scope == "active":
            print(
                render_terminal(
                    active_plans,
                    plans_directory,
                    use_color=use_color,
                )
            )
        elif args.scope == "archive":
            print(
                render_archive_terminal(
                    archive_plans,
                    plans_directory,
                    use_color=use_color,
                )
            )
        elif args.scope == "completed":
            print(
                render_terminal(
                    completed_plans,
                    plans_directory,
                    use_color=use_color,
                )
            )
        else:
            active = render_terminal(
                active_plans,
                plans_directory,
                use_color=use_color,
            )
            completed = render_terminal(
                completed_plans,
                plans_directory,
                use_color=use_color,
            )
            archived = render_archive_terminal(
                archive_plans,
                plans_directory,
                use_color=use_color,
            )
            print(
                f"{styled('Active Plans', '1', use_color)}\n\n"
                f"{active}"
                f"\n\n{styled('Completed Plans Awaiting Archive', '1', use_color)}\n\n"
                f"{completed}"
                f"\n\n{styled('Archived Plans', '1', use_color)}\n\n"
                f"{archived}"
            )
    elif args.scope == "active":
        print(render_markdown(active_plans, plans_directory))
    elif args.scope == "archive":
        print(render_archive_markdown(archive_plans, plans_directory))
    elif args.scope == "completed":
        print(render_markdown(completed_plans, plans_directory))
    else:
        print(
            "# Active Implementation Plans\n\n"
            f"{render_markdown(active_plans, plans_directory)}\n\n"
            "# Completed Plans Awaiting Archive\n\n"
            f"{render_markdown(completed_plans, plans_directory)}\n\n"
            "# Archived Implementation Plans\n\n"
            f"{render_archive_markdown(archive_plans, plans_directory)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
