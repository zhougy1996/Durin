from __future__ import annotations

import argparse
from dataclasses import replace
from pathlib import Path
from typing import TextIO

from .config import (
    Action,
    CommandRequest,
    CreateKind,
    LinkType,
    ModuleKind,
    OutputMode,
)
from .operations import execute_request, execute_shell_request


NAMESPACE_FIELDS = {
    "target": "target",
    "jobs": "jobs",
    "filter": "test_filter",
    "timeout": "test_timeout_seconds",
    "run_arguments": "run_arguments",
    "profile": "profile",
    "preset": "preset",
    "cmake": "cmake",
    "environment_setup": "environment_setup",
    "all_presets": "all_presets",
    "yes": "yes",
    "fresh": "fresh",
    "plain": "plain",
    "output_mode": "output_mode",
    "create_name": "create_name",
    "project_path": "project_path",
    "destination_path": "destination_path",
    "module_kind": "module_kind",
    "link_type": "link_type",
    "pch": "pch",
    "public_dependencies": "public_dependencies",
    "private_dependencies": "private_dependencies",
    "optional_public_dependencies": "optional_public_dependencies",
    "optional_private_dependencies": "optional_private_dependencies",
    "enablements": "enablements",
    "dry_run": "dry_run",
}


def request_from_namespace(namespace: argparse.Namespace) -> CommandRequest:
    action = Action(namespace.build_action)
    changes: dict[str, object] = {"action": action}
    for namespace_name, request_name in NAMESPACE_FIELDS.items():
        if not hasattr(namespace, namespace_name):
            continue
        value = getattr(namespace, namespace_name)
        if request_name == "output_mode":
            value = OutputMode(value)
        elif request_name == "module_kind":
            value = ModuleKind(value)
        elif request_name == "link_type":
            value = LinkType(value)
        elif request_name in {
            "run_arguments",
            "public_dependencies",
            "private_dependencies",
            "optional_public_dependencies",
            "optional_private_dependencies",
        }:
            value = tuple(value or ())
        elif request_name == "enablements" and value is not None:
            value = tuple(value)
        changes[request_name] = value
    if action is Action.CREATE_MODULE:
        changes["create_kind"] = CreateKind.MODULE
    elif action is Action.CREATE_PROJECT:
        changes["create_kind"] = CreateKind.PROJECT
    return replace(CommandRequest(action), **changes)


def run(
    namespace: argparse.Namespace,
    *,
    registry: object,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    session_state: dict[str, object] | None = None,
) -> int:
    del registry, repository_root
    if getattr(namespace, "selected_preset", ""):
        namespace.preset = namespace.selected_preset
    request = request_from_namespace(namespace)
    executor = execute_request if session_state is None else execute_shell_request
    return executor(
        request,
        stdout=stdout,
        stderr=stderr,
        **({} if session_state is None else {"session_state": session_state}),
    )
