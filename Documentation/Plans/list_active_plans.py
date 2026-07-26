#!/usr/bin/env python3
"""List and validate active implementation plans."""

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
EXCLUDED_FILES = {"AGENTS.md", "README.md"}


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


def discover_plans(directory: Path) -> tuple[list[Plan], list[str]]:
    plans: list[Plan] = []
    errors: list[str] = []

    for path in sorted(directory.glob("*.md"), key=lambda item: item.name.casefold()):
        if path.name in EXCLUDED_FILES:
            continue
        plan, plan_errors = parse_plan(path)
        errors.extend(f"{path.name}: {error}" for error in plan_errors)
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


def render_markdown(plans: list[Plan]) -> str:
    lines = ["| Plan | Primary Scope |", "| --- | --- |"]
    for plan in plans:
        title = markdown_cell(plan.title)
        summary = markdown_cell(plan.summary)
        lines.append(f"| [{title}]({plan.path.name}) | {summary} |")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate or validate the active implementation-plan index."
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
    plans, errors = discover_plans(plans_directory)

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    if not plans:
        print("error: no active implementation plans found", file=sys.stderr)
        return 1

    if args.validate:
        print(f"Validated {len(plans)} active implementation plans.")
    else:
        print(render_markdown(plans))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
