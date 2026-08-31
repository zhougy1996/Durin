"""Authored asset checking and canonical resave commands."""

from __future__ import annotations

import argparse
import io
import json
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence, TextIO

from .build.output import BuildOutput
from .build.errors import BuildToolError
from .build.models import OutputMode
from .context import RepositoryContext
from .errors import DevToolError
from .json_contract import JsonContractError, parse_json_contract
from .runtime_program import (
    ExecutableDescription,
    RuntimeProcessPolicy,
    RuntimeSelection,
    invoke_runtime_program,
    locate_executable,
    resolve_project,
    select_runtime,
)

SCHEMA_VERSION = 3
SCHEMA_DIRECTORY = Path(__file__).resolve().parents[1] / "schemas"
ASSET_EXECUTABLE = ExecutableDescription(
    "Asset maintenance", "DurinAssetTool", "DurinAssetTool"
)


def _validate_report(value: Any) -> dict[str, Any]:
    assert isinstance(value, dict)
    packages = value["packages"]
    previous_path = ""
    for package in packages:
        path = package["packagePath"]
        if path < previous_path:
            raise DevToolError("Asset audit package order is not deterministic.")
        previous_path = path
    return value


def _read_report(output: str) -> dict[str, Any]:
    try:
        value = parse_json_contract(
            output,
            label="Asset audit report",
            source="from DurinAssetTool",
            schema_path=SCHEMA_DIRECTORY / "asset-audit-v3.schema.json",
        )
    except JsonContractError as exc:
        raise DevToolError(str(exc)) from exc
    return _validate_report(value)


def _render_human(report: Mapping[str, Any], stdout: TextIO) -> None:
    packages: Sequence[Mapping[str, Any]] = report["packages"]
    compatible = sum(p["compatibility"] == "Compatible" for p in packages)
    incompatible = sum(p["compatibility"] == "Incompatible" for p in packages)
    unsupported = sum(p["compatibility"] == "Unsupported" for p in packages)
    failed = sum(p["inspection"] == "Failed" for p in packages)
    stale = sum(p["freshness"] == "Stale" for p in packages)
    resave_recommended = sum(
        bool(p["canonicalizationEvidence"] or p["deprecatedRouteEvidence"])
        for p in packages
    )
    print(
        f"Asset compatibility audit: {len(packages)} package(s); "
        f"{compatible} compatible, {incompatible} incompatible, "
        f"{unsupported} unsupported, {failed} failed, {stale} stale, "
        f"{resave_recommended} resave recommended.",
        file=stdout,
    )
    groups = (
        ("Incompatible", lambda p: p["compatibility"] == "Incompatible"),
        ("Unsupported", lambda p: p["compatibility"] == "Unsupported"),
        ("Failed", lambda p: p["inspection"] == "Failed"),
        ("Stale", lambda p: p["freshness"] == "Stale"),
        (
            "Resave recommended",
            lambda p: bool(
                p["canonicalizationEvidence"] or p["deprecatedRouteEvidence"]
            ),
        ),
    )
    for label, predicate in groups:
        selected = [package for package in packages if predicate(package)]
        if not selected:
            continue
        print(f"\n{label} ({len(selected)}):", file=stdout)
        for package in selected:
            print(f"  {package['packagePath']}", file=stdout)
            for finding in package["findings"]:
                print(
                    f"    [{finding['code']}] {finding['diagnostic']}",
                    file=stdout,
                )
            if label != "Resave recommended":
                continue
            for evidence in package["canonicalizationEvidence"]:
                print(
                    f"    [Canonicalization] {evidence['storedIdentity']} -> "
                    f"{evidence['currentIdentity']} "
                    f"({evidence['kind']} {evidence['location']}: "
                    f"{evidence['logicalPath']})",
                    file=stdout,
                )
            for evidence in package["deprecatedRouteEvidence"]:
                targets = ", ".join(evidence["migrationTargets"])
                print(
                    f"    [DeprecatedRoute] {evidence['declaringType']}."
                    f"{evidence['storedFieldName']} -> {targets}",
                    file=stdout,
                )


def _project_from_namespace(
    namespace: argparse.Namespace,
    repository: RepositoryContext,
) -> Path:
    value = getattr(namespace, "project_path", None)
    if value is None:
        value = repository.config.paths.default_game_project
    return resolve_project(repository, Path(value))


def _invoke_asset_program(
    selection: RuntimeSelection,
    executable: Path,
    arguments: Sequence[str],
    *,
    stderr: TextIO,
    interruption_message: str,
    command_runner: Callable[..., str] | None,
) -> str | None:
    process_output = BuildOutput(
        plain=True,
        output_mode=OutputMode.COMPACT,
        stdout=io.StringIO(),
        stderr=stderr,
    )
    try:
        return invoke_runtime_program(
            selection,
            ASSET_EXECUTABLE,
            arguments,
            output=process_output,
            policy=RuntimeProcessPolicy(
                interruption_message=interruption_message,
                show_heartbeat=True,
                capture_output=True,
            ),
            executable_override=executable,
            command_runner=command_runner,
        )
    except BuildToolError as error:
        if error.exit_code == 130:
            print(interruption_message, file=stderr)
            return None
        raise


def _run_check(
    namespace: argparse.Namespace,
    *,
    selection: RuntimeSelection,
    repository: RepositoryContext,
    executable: Path,
    stdout: TextIO,
    stderr: TextIO,
    command_runner: Callable[..., str] | None,
) -> int:
    project = _project_from_namespace(namespace, repository)
    arguments = ["check", f"--project={project}", "--json"]
    native_output = _invoke_asset_program(
        selection,
        executable,
        arguments,
        stderr=stderr,
        interruption_message="Asset compatibility audit cancelled.",
        command_runner=command_runner,
    )
    if native_output is None:
        return 130
    report = _read_report(native_output)
    if namespace.format_name == "json":
        print(json.dumps(report, separators=(",", ":"), ensure_ascii=False), file=stdout)
    else:
        _render_human(report, stdout)
    return 0


def _run_scoped_operation(
    namespace: argparse.Namespace,
    *,
    native_command: str,
    operation_label: str,
    interruption_message: str,
    selection: RuntimeSelection,
    repository: RepositoryContext,
    executable: Path,
    stdout: TextIO,
    stderr: TextIO,
    command_runner: Callable[..., str] | None,
) -> int:
    scopes = tuple(getattr(namespace, "scopes", ()) or ())
    whole_project = bool(getattr(namespace, "whole_project", False))
    if whole_project and scopes:
        raise DevToolError(
            f"Asset {operation_label} accepts either scopes or --all, not both."
        )
    if not whole_project and not scopes:
        raise DevToolError(
            f"Asset {operation_label} requires at least one scope or --all."
        )

    project = _project_from_namespace(namespace, repository)
    arguments = [native_command, f"--project={project}"]
    arguments.extend(scopes)
    if whole_project:
        arguments.append("--all")
    if bool(getattr(namespace, "apply", False)):
        arguments.append("--apply")
    if getattr(namespace, "format_name", "human") == "json":
        arguments.append("--json")

    native_output = _invoke_asset_program(
        selection,
        executable,
        arguments,
        stderr=stderr,
        interruption_message=interruption_message,
        command_runner=command_runner,
    )
    if native_output is None:
        return 130
    print(native_output, end="" if native_output.endswith("\n") else "\n", file=stdout)
    return 0


def _run_resave(namespace: argparse.Namespace, **kwargs: Any) -> int:
    return _run_scoped_operation(
        namespace,
        native_command="resave",
        operation_label="resave",
        interruption_message="Asset canonical resave cancelled.",
        **kwargs,
    )


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    repository_context: RepositoryContext | None = None,
    stdout: TextIO,
    stderr: TextIO,
    command_runner: Callable[..., str] | None = None,
    executable_resolver: Callable[[argparse.Namespace, Path], Path] | None = None,
    **_kwargs: object,
) -> int:
    repository = repository_context or RepositoryContext.load(repository_root)
    selection = select_runtime(
        repository,
        profile_name=str(getattr(namespace, "profile", "") or ""),
        preset_name=str(getattr(namespace, "preset", "") or ""),
    )
    executable = (
        executable_resolver(namespace, repository.root)
        if executable_resolver
        else locate_executable(
            selection,
            ASSET_EXECUTABLE,
        )
    )
    command = getattr(namespace, "asset_command", "check")
    if command == "check":
        return _run_check(
            namespace,
            selection=selection,
            repository=repository,
            executable=executable,
            stdout=stdout,
            stderr=stderr,
            command_runner=command_runner,
        )
    if command == "resave":
        return _run_resave(
            namespace,
            selection=selection,
            repository=repository,
            executable=executable,
            stdout=stdout,
            stderr=stderr,
            command_runner=command_runner,
        )
    raise DevToolError(f"Unsupported asset command: {command}")
