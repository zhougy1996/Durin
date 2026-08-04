from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from .config import (
    Action,
    BuildActionOptions,
    CommandRequest,
    CreateActionOptions,
    CreateKind,
    LinkType,
    LocationActionOptions,
    ModuleKind,
    OutputOptions,
    OutputMode,
    PurgeActionOptions,
    RequestContext,
    RunActionOptions,
    TestActionOptions,
)
from .operations import execute_request


def namespace_value(
    namespace: argparse.Namespace,
    name: str,
    default: object,
) -> object:
    value = getattr(namespace, name, default)
    return default if value is None else value


def request_from_namespace(namespace: argparse.Namespace) -> CommandRequest:
    action = Action(namespace.build_action)
    selected_preset = str(namespace_value(namespace, "selected_preset", ""))
    context = RequestContext(
        profile=str(namespace_value(namespace, "profile", "")),
        preset=selected_preset or str(namespace_value(namespace, "preset", "")),
        cmake=str(namespace_value(namespace, "cmake", "")),
        environment_setup=str(
            namespace_value(namespace, "environment_setup", "")
        ),
        jobs=namespace_value(namespace, "jobs", None),
    )
    output = OutputOptions(
        plain=bool(namespace_value(namespace, "plain", False)),
        mode=OutputMode(namespace_value(namespace, "output_mode", OutputMode.AUTO)),
    )
    options = None
    if action in {Action.CONFIGURE, Action.BUILD, Action.REBUILD}:
        options = BuildActionOptions(
            target=str(namespace_value(namespace, "target", "")),
            fresh=bool(namespace_value(namespace, "fresh", False)),
        )
    elif action is Action.TEST:
        options = TestActionOptions(
            target=str(namespace_value(namespace, "target", "")),
            filter=str(namespace_value(namespace, "filter", "")),
            timeout_seconds=int(namespace_value(namespace, "timeout", 300)),
            schedule_random=bool(
                namespace_value(namespace, "schedule_random", False)
            ),
            output_junit=namespace_value(namespace, "output_junit", None),
            ctest_regex=str(namespace_value(namespace, "ctest_regex", "")),
            include_direct=bool(
                namespace_value(namespace, "include_direct", False)
            ),
        )
    elif action is Action.RUN:
        options = RunActionOptions(
            project_path=namespace_value(namespace, "project_path", None),
            arguments=tuple(namespace_value(namespace, "run_arguments", ()) or ()),
        )
    elif action is Action.PURGE:
        options = PurgeActionOptions(
            all_presets=bool(namespace_value(namespace, "all_presets", False)),
            yes=bool(namespace_value(namespace, "yes", False)),
        )
    elif action in {Action.PATH, Action.OPEN}:
        options = LocationActionOptions(
            location=str(namespace_value(namespace, "location", "")),
            all_locations=bool(
                namespace_value(namespace, "all_locations", False)
            ),
        )
    if action is Action.CREATE_MODULE:
        create_kind = CreateKind.MODULE
    elif action is Action.CREATE_PROJECT:
        create_kind = CreateKind.PROJECT
    else:
        create_kind = None
    if create_kind is not None:
        enablements = namespace_value(namespace, "enablements", None)
        options = CreateActionOptions(
            kind=create_kind,
            name=str(namespace_value(namespace, "create_name", "")),
            project_path=namespace_value(namespace, "project_path", None),
            destination_path=namespace_value(namespace, "destination_path", None),
            module_kind=ModuleKind(
                namespace_value(namespace, "module_kind", ModuleKind.RUNTIME)
            ),
            link_type=LinkType(
                namespace_value(namespace, "link_type", LinkType.SHARED)
            ),
            pch=str(namespace_value(namespace, "pch", "")),
            public_dependencies=tuple(
                namespace_value(namespace, "public_dependencies", ()) or ()
            ),
            private_dependencies=tuple(
                namespace_value(namespace, "private_dependencies", ()) or ()
            ),
            optional_public_dependencies=tuple(
                namespace_value(namespace, "optional_public_dependencies", ()) or ()
            ),
            optional_private_dependencies=tuple(
                namespace_value(namespace, "optional_private_dependencies", ()) or ()
            ),
            enablements=None if enablements is None else tuple(enablements),
            dry_run=bool(namespace_value(namespace, "dry_run", False)),
        )
    return CommandRequest(action, context=context, output=output, options=options)


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
    request = request_from_namespace(namespace)
    return execute_request(
        request,
        stdout=stdout,
        stderr=stderr,
        session_state=session_state,
    )
