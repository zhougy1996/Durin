"""Engineering-roadmap lifecycle parsing, discovery, and rendering."""

from __future__ import annotations

from pathlib import Path
from typing import Sequence

from .plans import (
    Plan as Roadmap,
    PlanCatalog as RoadmapCatalog,
    PlanStatus as RoadmapStatus,
    filter_plans,
    load_catalog as load_lifecycle_catalog,
    parse_plan,
    render_listing as render_lifecycle_listing,
)


def parse_roadmap(
    path: Path,
    *,
    default_status: RoadmapStatus = RoadmapStatus.ACTIVE,
) -> tuple[Roadmap | None, list[str]]:
    return parse_plan(
        path,
        default_status=default_status,
        title_suffix=" Roadmap",
        document_label="roadmap",
    )


def load_catalog(roadmaps_directory: Path) -> RoadmapCatalog:
    return load_lifecycle_catalog(
        roadmaps_directory,
        title_suffix=" Roadmap",
        document_label="roadmap",
        excluded_files=frozenset({"AGENTS.md", "README.md"}),
    )


def filter_roadmaps(
    roadmaps: Sequence[Roadmap],
    query: str | None,
) -> list[Roadmap]:
    return filter_plans(roadmaps, query)


def render_listing(
    roadmaps: Sequence[Roadmap],
    roadmaps_directory: Path,
    *,
    scope: str,
    output_format: str,
    color: str,
) -> str:
    return render_lifecycle_listing(
        roadmaps,
        roadmaps_directory,
        scope=scope,
        output_format=output_format,
        color=color,
        document_label="roadmap",
    )
