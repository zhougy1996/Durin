"""Configuration-driven plan and roadmap lifecycle application service."""

from __future__ import annotations

import datetime as dt
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from ..errors import DevToolError
from .changes import DocumentChangeSet, FileChange
from .model import DocumentRef
from .plans import Plan, PlanCatalog, filter_plans, load_catalog, parse_plan, render_listing


@dataclass(frozen=True)
class LifecycleConfig:
    directory_name: str
    title_suffix: str
    document_label: str
    excluded_files: frozenset[str]


PLAN_LIFECYCLE = LifecycleConfig(
    directory_name="Plans",
    title_suffix=" Plan",
    document_label="plan",
    excluded_files=frozenset({"AGENTS.md"}),
)
ROADMAP_LIFECYCLE = LifecycleConfig(
    directory_name="Roadmaps",
    title_suffix=" Roadmap",
    document_label="roadmap",
    excluded_files=frozenset({"AGENTS.md", "README.md"}),
)


class LifecycleWorkspace:
    def __init__(self, repository_root: Path, config: LifecycleConfig) -> None:
        self.repository_root = repository_root.resolve()
        self.config = config
        self.directory = self.repository_root / "Documentation" / config.directory_name
        self._catalog: PlanCatalog | None = None

    def catalog(self) -> PlanCatalog:
        if self._catalog is None:
            self._catalog = load_catalog(
                self.directory,
                title_suffix=self.config.title_suffix,
                document_label=self.config.document_label,
                excluded_files=self.config.excluded_files,
            )
        return self._catalog

    def select(self, scope: str, query: str | None = None) -> list[Plan]:
        return filter_plans(self.catalog().select(scope), query)

    def render(
        self,
        documents: Sequence[Plan],
        *,
        scope: str,
        output_format: str,
        color: str,
    ) -> str:
        return render_listing(
            documents,
            self.directory,
            scope=scope,
            output_format=output_format,
            color=color,
            document_label=self.config.document_label,
        )

    def parse(self, path: Path):
        return parse_plan(
            path,
            title_suffix=self.config.title_suffix,
            document_label=self.config.document_label,
        )

    def prepare_create(
        self,
        *,
        destination: DocumentRef,
        title: str,
        summary: str,
        reviewed_on: dt.date | None = None,
    ) -> DocumentChangeSet:
        if self.config != PLAN_LIFECYCLE:
            raise DevToolError("only implementation-plan creation is supported")
        expected_parent = Path("Documentation") / self.config.directory_name
        if destination.path.parent != expected_parent:
            raise DevToolError(
                "plan destination must be a direct child of Documentation/Plans"
            )
        absolute_destination = (self.repository_root / destination.path).resolve()
        if absolute_destination.exists():
            raise DevToolError(
                f'plan already exists: "{destination.as_posix()}"'
            )

        clean_title = title.strip()
        clean_summary = summary.strip()
        if not clean_title:
            raise DevToolError("plan title must not be empty")
        if clean_title.casefold().endswith(self.config.title_suffix.casefold()):
            raise DevToolError(
                f"plan title must omit the {self.config.title_suffix.strip()!r} suffix"
            )
        if not clean_summary:
            raise DevToolError("plan summary must not be empty")
        if "\n" in clean_summary or "\r" in clean_summary:
            raise DevToolError("plan summary must fit on one line")
        if any(
            plan.title.casefold() == clean_title.casefold()
            for plan in self.catalog().select("all")
        ):
            raise DevToolError(f'plan title already exists: "{clean_title}"')

        reviewed = reviewed_on or dt.date.today()
        content = (
            f"# {clean_title} Plan\n\n"
            f"Summary: {clean_summary}\n\n"
            f"Last reviewed: {reviewed.isoformat()}\n\n"
            "Status: Active\n"
            "Completed:\n\n"
            "## Current Status\n\n"
            "## Goal\n\n"
            "## Scope\n\n"
            "## Non-Goals\n\n"
            "## Design Decisions and Invariants\n\n"
            "## Current Foundations and Gaps\n\n"
            "## Implementation Stages\n\n"
            "### Stage 0: Define the implementation boundary\n\n"
            "- [ ] Confirm scope, dependencies, and selected design.\n\n"
            "#### Acceptance Gate\n\n"
            "- Scope, decisions, and validation requirements are explicit.\n\n"
            "## Validation Matrix\n\n"
            "## Definition of Done\n\n"
            "## Deferred Follow-ups\n\n"
            "## Related Documentation\n\n"
            "## Related Code\n"
        ).encode("utf-8")
        return DocumentChangeSet(
            operation="create-plan",
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
