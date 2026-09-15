from __future__ import annotations

import argparse
from pathlib import Path
from typing import TextIO

from ..context import CommandIO, RepositoryContext

from .errors import BuildToolError
from .models import Action, LinkType, ModuleKind, OutputMode, TestMode
from .operations import execute_request
from .requests import (
    BaseRequest,
    BuildRequest,
    ConfigureRequest,
    LocationRequest,
    ModuleCreationRequest,
    NativeTestRequest,
    OutputOptions,
    ProjectCreationRequest,
    PurgeRequest,
    RebuildRequest,
    RequestContext,
    RunRequest,
    SimpleRequest,
)


def namespace_value(
    namespace: argparse.Namespace,
    name: str,
    default: object,
) -> object:
    value = getattr(namespace, name, default)
    return default if value is None else value


def request_from_namespace(namespace: argparse.Namespace) -> BaseRequest:
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
    if action is Action.CONFIGURE:
        return ConfigureRequest(
            context=context,
            output=output,
            fresh=bool(namespace_value(namespace, "fresh", False)),
            defines=tuple(namespace_value(namespace, "defines", ())),
        )
    if action is Action.BUILD:
        return BuildRequest(
            context=context,
            output=output,
            target=str(namespace_value(namespace, "target", "all")),
        )
    if action is Action.REBUILD:
        return RebuildRequest(
            context=context,
            output=output,
            target=str(namespace_value(namespace, "target", "all")),
        )
    if action is Action.TEST:
        positional_selection = str(namespace_value(namespace, "selection", ""))
        positional_filter = str(namespace_value(namespace, "case_filter", ""))
        operation = "run"
        query = ""
        target = positional_selection
        test_filter = positional_filter
        if positional_selection == "list":
            operation, query, target, test_filter = "list", positional_filter, "", ""
        elif positional_selection == "explain":
            operation, query, target, test_filter = "explain", "", positional_filter, ""
        elif positional_selection == "affected":
            if positional_filter:
                raise BuildToolError("test affected accepts a Git base only through --base <ref>")
            operation, query, target, test_filter = "affected", "", "affected", ""
        test_mode = TestMode(str(namespace_value(namespace, "mode", "routine")))
        if (operation in {"list", "explain"} or namespace_value(namespace, "explain_affected", False)) and namespace_value(namespace, "timeout", None) is not None:
            raise BuildToolError("--timeout requires test execution; discovery does not run tests.")
        if namespace_value(namespace, "isolate", False):
            if operation != "run" or target.casefold() in {"all", "fast-all"}:
                raise BuildToolError("--isolate requires a named target or @set selection.")
            if test_mode is not TestMode.ROUTINE:
                raise BuildToolError("--isolate cannot be combined with --mode.")
            test_mode = TestMode.ISOLATION
        report_path = namespace_value(namespace, "report", None)
        return NativeTestRequest(
            context=context,
            output=output,
            target=target,
            test_filter=test_filter,
            test_operation=operation,
            test_query=query,
            test_mode=test_mode,
            test_parallel_jobs=namespace_value(namespace, "test_jobs", None),
            test_report_enabled=report_path is not None,
            test_report_path=report_path if isinstance(report_path, Path) else None,
            test_timeout_seconds=int(namespace_value(namespace, "timeout", 300)),
            test_base=str(namespace_value(namespace, "base", "")),
            test_explain_affected=bool(namespace_value(namespace, "explain_affected", False)),
        )
    if action is Action.RUN:
        return RunRequest(
            context=context,
            output=output,
            project_path=namespace_value(namespace, "project_path", None),
            run_arguments=tuple(namespace_value(namespace, "run_arguments", ()) or ()),
        )
    if action is Action.PURGE:
        return PurgeRequest(
            context=context,
            output=output,
            all_presets=bool(namespace_value(namespace, "all_presets", False)),
            yes=bool(namespace_value(namespace, "yes", False)),
        )
    if action in {Action.PATH, Action.OPEN}:
        return LocationRequest(
            context=context,
            output=output,
            action=action,
            location=str(namespace_value(namespace, "location", "")),
            all_locations=bool(
                namespace_value(namespace, "all_locations", False)
            ),
        )
    if action is Action.CREATE_MODULE:
        enablements = namespace_value(namespace, "enablements", None)
        return ModuleCreationRequest(
            context=context,
            output=output,
            create_name=str(namespace_value(namespace, "create_name", "")),
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
    if action is Action.CREATE_PROJECT:
        return ProjectCreationRequest(
            context=context,
            output=output,
            create_name=str(namespace_value(namespace, "create_name", "")),
            destination_path=namespace_value(namespace, "destination_path", None),
            dry_run=bool(namespace_value(namespace, "dry_run", False)),
        )
    return SimpleRequest(context=context, output=output, action=action)


def run(
    namespace: argparse.Namespace,
    *,
    registry: object,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    repository_context: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
    session_state: dict[str, object] | None = None,
) -> int:
    del registry, command_io
    repository = repository_context or RepositoryContext.load(repository_root)
    request = request_from_namespace(namespace)
    return execute_request(
        request,
        stdout=stdout,
        stderr=stderr,
        session_state=session_state,
        repository_context=repository,
    )
