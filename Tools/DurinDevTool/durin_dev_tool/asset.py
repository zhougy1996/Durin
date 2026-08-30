"""Authored asset checking and canonical resave commands."""

from __future__ import annotations

import argparse
import io
import json
import subprocess
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence, TextIO

from .build.output import BuildOutput
from .build.config import BuildToolError, OutputMode
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

POLICY_EXIT_CODE = 3
SCHEMA_VERSION = 3
CURRENT_ASSET_FORMAT_VERSION = 7
SCHEMA_DIRECTORY = Path(__file__).resolve().parents[1] / "schemas"


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


def _baseline_failed(report: Mapping[str, Any]) -> bool:
    packages: Sequence[Mapping[str, Any]] = report["packages"]
    return (
        not packages
        or any(
            package["formatVersion"] != CURRENT_ASSET_FORMAT_VERSION
            or package["inspection"] != "Ready"
            or package["compatibility"] != "Compatible"
            or package["freshness"] != "Current"
            or package["findings"]
            or package["canonicalizationEvidence"]
            or package["deprecatedRouteEvidence"]
            for package in packages
        )
    )


def _project_from_namespace(
    namespace: argparse.Namespace,
    repository: RepositoryContext,
) -> Path:
    value = getattr(namespace, "project_path", None)
    if value is None:
        value = repository.config.paths.default_game_project
    return resolve_project(repository, Path(value))


def _run_check(
    namespace: argparse.Namespace,
    *,
    selection: RuntimeSelection,
    repository: RepositoryContext,
    executable: Path,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    process_runner: Callable[..., subprocess.CompletedProcess[str]],
) -> int:
    is_baseline = bool(getattr(namespace, "baseline", False))
    project = _project_from_namespace(namespace, repository)
    arguments = ["check", f"--project={project}", "--json"]
    if process_runner is subprocess.run:
        process_output = BuildOutput(
            plain=True,
            output_mode=OutputMode.COMPACT,
            stdout=io.StringIO(),
            stderr=stderr,
        )
        try:
            native_output = invoke_runtime_program(
                selection,
                ExecutableDescription("Asset maintenance", "DurinAssetTool", "DurinAssetTool"),
                arguments,
                output=process_output,
                policy=RuntimeProcessPolicy(
                    interruption_message="Asset compatibility audit cancelled.",
                    show_heartbeat=True,
                    capture_output=True,
                ),
                executable_override=executable,
            )
        except BuildToolError as error:
            if error.exit_code == 130:
                print("Asset compatibility audit cancelled.", file=stderr)
                return 130
            raise
    else:
        completed = process_runner(
            [str(executable), *arguments],
            cwd=repository_root,
            text=True,
            capture_output=True,
            check=False,
        )
        if completed.returncode == 130:
            print("Asset compatibility audit cancelled.", file=stderr)
            return 130
        if completed.returncode != 0:
            diagnostic = completed.stderr.strip() or "native audit process failed"
            raise DevToolError(f"Asset compatibility audit failed: {diagnostic}")
        native_output = completed.stdout
    report = _read_report(native_output)
    if namespace.format_name == "json":
        print(json.dumps(report, separators=(",", ":"), ensure_ascii=False), file=stdout)
    elif is_baseline:
        if _baseline_failed(report):
            _render_human(report, stdout)
            print(
                "\nAsset baseline rejected: every package must be current DAST v7 "
                "with no compatibility or resave findings.",
                file=stdout,
            )
        else:
            print(f"Asset baseline: {len(report['packages'])} current DAST v7 package(s).", file=stdout)
    else:
        _render_human(report, stdout)
    if is_baseline:
        return POLICY_EXIT_CODE if _baseline_failed(report) else 0
    return 0


def _run_resave(
    namespace: argparse.Namespace,
    *,
    selection: RuntimeSelection,
    repository: RepositoryContext,
    executable: Path,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    process_runner: Callable[..., subprocess.CompletedProcess[str]],
) -> int:
    scopes = tuple(getattr(namespace, "scopes", ()) or ())
    whole_project = bool(getattr(namespace, "whole_project", False))
    if whole_project and scopes:
        raise DevToolError("Asset resave accepts either scopes or --all, not both.")
    if not whole_project and not scopes:
        raise DevToolError("Asset resave requires at least one scope or --all.")

    project = _project_from_namespace(namespace, repository)
    arguments = ["resave", f"--project={project}"]
    arguments.extend(scopes)
    if whole_project:
        arguments.append("--all")
    if bool(getattr(namespace, "apply", False)):
        arguments.append("--apply")
    if getattr(namespace, "format_name", "human") == "json":
        arguments.append("--json")

    if process_runner is subprocess.run:
        process_output = BuildOutput(
            plain=True,
            output_mode=OutputMode.COMPACT,
            stdout=io.StringIO(),
            stderr=stderr,
        )
        try:
            native_output = invoke_runtime_program(
                selection,
                ExecutableDescription("Asset maintenance", "DurinAssetTool", "DurinAssetTool"),
                arguments,
                output=process_output,
                policy=RuntimeProcessPolicy(
                    interruption_message="Asset canonical resave cancelled.",
                    show_heartbeat=True,
                    capture_output=True,
                ),
                executable_override=executable,
            )
        except BuildToolError as error:
            if error.exit_code == 130:
                print("Asset canonical resave cancelled.", file=stderr)
                return 130
            raise
    else:
        completed = process_runner(
            [str(executable), *arguments],
            cwd=repository_root,
            text=True,
            capture_output=True,
            check=False,
        )
        if completed.returncode == 130:
            print("Asset canonical resave cancelled.", file=stderr)
            return 130
        if completed.returncode != 0:
            diagnostic = completed.stderr.strip() or "native resave process failed"
            raise DevToolError(f"Asset canonical resave failed: {diagnostic}")
        native_output = completed.stdout
    print(native_output, end="" if native_output.endswith("\n") else "\n", file=stdout)
    return 0


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    repository_context: RepositoryContext | None = None,
    stdout: TextIO,
    stderr: TextIO,
    process_runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
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
            ExecutableDescription("Asset maintenance", "DurinAssetTool", "DurinAssetTool"),
        )
    )
    command = getattr(namespace, "asset_command", "check")
    if command == "check":
        return _run_check(
            namespace,
            selection=selection,
            repository=repository,
            executable=executable,
            repository_root=repository.root,
            stdout=stdout,
            stderr=stderr,
            process_runner=process_runner,
        )
    if command == "resave":
        return _run_resave(
            namespace,
            selection=selection,
            repository=repository,
            executable=executable,
            repository_root=repository.root,
            stdout=stdout,
            stderr=stderr,
            process_runner=process_runner,
        )
    raise DevToolError(f"Unsupported asset command: {command}")
