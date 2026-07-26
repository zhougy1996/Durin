#!/usr/bin/env python3
"""List and validate active or archived implementation plans."""

from __future__ import annotations

import argparse
import datetime as dt
import re
import sys
from dataclasses import dataclass
from pathlib import Path


TITLE_PATTERN = re.compile(r"^# (?P<title>.+ Plan)$")
SUMMARY_PATTERN = re.compile(r"^Summary: (?P<summary>.+)$")
REVIEWED_PATTERN = re.compile(r"^Last reviewed: (?P<date>\d{4}-\d{2}-\d{2})$")
REQUIRED_SECTION = "## Current Status"
EXCLUDED_FILES = {"AGENTS.md"}


@dataclass(frozen=True)
class Plan:
    path: Path
    title: str
    summary: str


def parse_plan(path: Path) -> tuple[Plan | None, list[str]]:
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

    if errors or title_match is None or summary_match is None:
        return None, errors

    return (
        Plan(
            path=path,
            title=title_match.group("title").removesuffix(" Plan"),
            summary=summary_match.group("summary"),
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
        plan, plan_errors = parse_plan(path)
        display_path = path.relative_to(directory).as_posix()
        errors.extend(f"{display_path}: {error}" for error in plan_errors)
        if plan is not None:
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate or validate implementation-plan indexes."
    )
    parser.add_argument(
        "--scope",
        choices=("active", "archive", "all"),
        default="active",
        help="select plans to list or validate (default: active)",
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="validate plans without printing the generated Markdown index",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    plans_directory = Path(__file__).resolve().parent
    active_plans, active_errors = discover_plans(plans_directory)
    archive_plans, archive_errors = discover_plans(
        plans_directory / "Archive",
        recursive=True,
        require_archive_month=True,
    )

    errors: list[str] = []
    if args.scope in ("active", "all"):
        errors.extend(active_errors)
    if args.scope in ("archive", "all"):
        errors.extend(f"Archive/{error}" for error in archive_errors)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1

    selected_plans = active_plans if args.scope == "active" else archive_plans
    if args.scope == "all":
        selected_plans = active_plans + archive_plans
    if not selected_plans:
        print(f"error: no {args.scope} implementation plans found", file=sys.stderr)
        return 1

    if args.validate:
        if args.scope == "all":
            print(
                f"Validated {len(active_plans)} active and "
                f"{len(archive_plans)} archived implementation plans."
            )
        else:
            print(f"Validated {len(selected_plans)} {args.scope} implementation plans.")
    elif args.scope == "active":
        print(render_markdown(active_plans, plans_directory))
    elif args.scope == "archive":
        print(render_archive_markdown(archive_plans, plans_directory))
    else:
        print(
            "# Active Implementation Plans\n\n"
            f"{render_markdown(active_plans, plans_directory)}\n\n"
            "# Archived Implementation Plans\n\n"
            f"{render_archive_markdown(archive_plans, plans_directory)}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
