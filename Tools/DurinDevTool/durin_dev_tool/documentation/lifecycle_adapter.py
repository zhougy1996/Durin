"""Configuration-driven plan/roadmap command adapter."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from ..errors import DevToolError
from .adapter_common import output_format
from .archive import ArchivePreview, apply_lifecycle_archive, preview_lifecycle_archive
from .lifecycle import LifecycleConfig
from .model import DocumentRef
from .plans import render_plan_context
from .rendering import render_change_set
from .service import DocumentWorkspace


def _errors(errors: list[str], stream: TextIO) -> None:
    for error in errors:
        print(f"error: {error}", file=stream)


def _print_archive(
    preview: ArchivePreview,
    *,
    repository_root: Path,
    applied: bool,
    stdout: TextIO,
    label: str,
) -> None:
    if not preview.moves:
        print(f"No completed {label}s are awaiting archival for {preview.month}.", file=stdout)
        return
    action = "Archived" if applied else "Would archive"
    for move in preview.moves:
        print(
            f"{action}: {move.source.relative_to(repository_root).as_posix()} -> "
            f"{move.destination.relative_to(repository_root).as_posix()}",
            file=stdout,
        )
    verb = "Updated" if applied else "Would update"
    print(f"{verb} {len(preview.reference_files)} referencing Markdown file(s).", file=stdout)
    for path in preview.reference_files:
        print(f"  {path.relative_to(repository_root).as_posix()}", file=stdout)
    print(
        f"Archive applied and all {label}s validated."
        if applied
        else "Dry-run only; remove --dry-run to perform the archive.",
        file=stdout,
    )


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    interactive: bool,
    stdout: TextIO,
    stderr: TextIO,
    config: LifecycleConfig,
) -> int:
    document_workspace = DocumentWorkspace(repository_root)
    workspace = document_workspace.lifecycle(config)
    action = getattr(namespace, f"{config.document_label}_action")
    if action == "create":
        change_set = workspace.prepare_create(
            destination=DocumentRef.parse(namespace.plan_path),
            title=namespace.title,
            summary=namespace.summary,
        )
        if not namespace.dry_run:
            document_workspace.apply(change_set)
        print(render_change_set(
            change_set,
            repository_root=repository_root.resolve(),
            applied=not namespace.dry_run,
            output_format=output_format(namespace, interactive=interactive),
            preview_instruction="Dry-run only; remove --dry-run to create the plan.",
        ), file=stdout)
        return 0
    if action == "list":
        if namespace.scope in {"archive", "all"} and not namespace.query and not namespace.all_results:
            raise DevToolError(
                "archive listings require --query <title-or-filename>; "
                "use --all-results only for an explicitly requested full listing"
            )
        errors = workspace.catalog().errors_for(namespace.scope)
        if errors:
            _errors(errors, stderr)
            return 1
        documents = workspace.select(namespace.scope, namespace.query)
        if not documents:
            if namespace.query:
                raise DevToolError(
                    f"no {namespace.scope} {config.document_label}s match query {namespace.query!r}"
                )
            if namespace.scope == "completed":
                print(f"No completed {config.document_label}s are awaiting archival.", file=stdout)
                return 0
            raise DevToolError(f"no {namespace.scope} {config.document_label}s found")
        print(workspace.render(
            documents,
            scope=namespace.scope,
            output_format=output_format(namespace, interactive=interactive),
            color=namespace.color,
        ), file=stdout)
        return 0
    if action == "context":
        errors = workspace.catalog().errors_for(namespace.scope)
        if errors:
            _errors(errors, stderr)
            return 1
        matches = workspace.select(namespace.scope, namespace.plan_query)
        if not matches:
            raise DevToolError(
                f"no {namespace.scope} plans match query {namespace.plan_query!r}"
            )
        if len(matches) != 1:
            choices = ", ".join(plan.title for plan in matches)
            raise DevToolError(
                f"plan query {namespace.plan_query!r} is ambiguous: {choices}"
            )
        print(
            render_plan_context(
                matches[0],
                repository_root=repository_root,
                output_format=namespace.output_format,
            ),
            file=stdout,
        )
        return 0
    if action == "validate":
        catalog = workspace.catalog()
        errors = catalog.errors_for(namespace.scope)
        if errors:
            _errors(errors, stderr)
            return 1
        documents = catalog.select(namespace.scope)
        if namespace.scope == "all":
            print(
                f"Validated {len(catalog.active)} active, {len(catalog.completed)} completed, "
                f"and {len(catalog.archived)} archived {config.document_label}s.",
                file=stdout,
            )
        else:
            print(f"Validated {len(documents)} {namespace.scope} {config.document_label}s.", file=stdout)
        return 0
    if namespace.apply and namespace.dry_run:
        raise DevToolError("--apply and --dry-run cannot be combined")
    applied = not namespace.dry_run
    preview = (
        apply_lifecycle_archive(workspace.directory, namespace.month, config)
        if applied
        else preview_lifecycle_archive(workspace.directory, namespace.month, config)
    )
    _print_archive(
        preview,
        repository_root=repository_root,
        applied=applied,
        stdout=stdout,
        label=config.document_label,
    )
    return 0
