"""Engineering-task command adapter."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from ..errors import DevToolError
from .adapter_common import output_format
from .model import DiagnosticSeverity, DocumentRef
from .rendering import render_change_set, render_diagnostics, render_tasks
from .service import DocumentWorkspace
from .tasks import filter_tasks


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    interactive: bool,
    stdout: TextIO,
    stderr: TextIO,
) -> int:
    action = namespace.task_action
    workspace = DocumentWorkspace(repository_root)
    if action == "list":
        catalog = workspace.task_catalog()
        if catalog.diagnostics:
            print(render_diagnostics(
                catalog.diagnostics,
                output_format=output_format(namespace, interactive=interactive),
                document_count=len(catalog.tasks),
            ), file=stderr)
            return 1
        tasks = filter_tasks(catalog.tasks, namespace.task_query)
        if not tasks:
            if namespace.task_query:
                raise DevToolError(f"no open tasks match query {namespace.task_query!r}")
            print("No open engineering tasks.", file=stdout)
            return 0
        print(render_tasks(
            tasks,
            repository_root=repository_root,
            output_format=output_format(namespace, interactive=interactive),
        ), file=stdout)
        return 0
    if action == "validate":
        catalog = workspace.task_catalog()
        print(render_diagnostics(
            catalog.diagnostics,
            output_format=output_format(namespace, interactive=interactive),
            document_count=len(catalog.tasks),
        ), file=stdout)
        return 1 if any(item.severity is DiagnosticSeverity.ERROR for item in catalog.diagnostics) else 0
    change_set = workspace.prepare_task_remove(task=DocumentRef.parse(namespace.task_path))
    if namespace.apply:
        workspace.apply(change_set)
    print(render_change_set(
        change_set,
        repository_root=repository_root.resolve(),
        applied=namespace.apply,
        output_format=output_format(namespace, interactive=interactive),
    ), file=stdout)
    return 0
