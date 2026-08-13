"""Configuration-driven plan and roadmap lifecycle application service."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

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
