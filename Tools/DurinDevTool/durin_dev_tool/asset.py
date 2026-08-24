"""Asset compatibility audit and current-format baseline enforcement."""

from __future__ import annotations

import argparse
import io
import json
import subprocess
from dataclasses import replace
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
    invoke_runtime_program,
    locate_executable,
    resolve_project,
    select_runtime,
)

POLICY_EXIT_CODE = 3
SCHEMA_VERSION = 3
CURRENT_ASSET_FORMAT_VERSION = 5
SCHEMA_DIRECTORY = Path(__file__).resolve().parents[1] / "schemas"


def _runtime_executable(
    namespace: argparse.Namespace, repository_root: Path
) -> Path:
    repository = RepositoryContext.load().at_root(repository_root)
    selection = select_runtime(
        repository,
        profile_name=str(getattr(namespace, "profile", "") or ""),
        preset_name=str(getattr(namespace, "preset", "") or ""),
    )
    return locate_executable(
        selection,
        ExecutableDescription("Asset audit", "DurinAssetTool", "DurinAssetTool"),
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
    print(
        f"Asset compatibility audit: {len(packages)} package(s); "
        f"{compatible} compatible, {incompatible} incompatible, "
        f"{unsupported} unsupported, {failed} failed, {stale} stale.",
        file=stdout,
    )
    groups = (
        ("Incompatible", lambda p: p["compatibility"] == "Incompatible"),
        ("Unsupported", lambda p: p["compatibility"] == "Unsupported"),
        ("Failed", lambda p: p["inspection"] == "Failed"),
        ("Stale", lambda p: p["freshness"] == "Stale"),
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


def _policy_failed(report: Mapping[str, Any], policies: set[str]) -> bool:
    packages: Sequence[Mapping[str, Any]] = report["packages"]
    return any(
        ("incompatible" in policies and p["compatibility"] == "Incompatible")
        or ("unsupported" in policies and p["compatibility"] == "Unsupported")
        or ("error" in policies and p["inspection"] == "Failed")
        for p in packages
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
            for package in packages
        )
    )


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    process_runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    executable_resolver: Callable[[argparse.Namespace, Path], Path] = _runtime_executable,
    **_kwargs: object,
) -> int:
    asset_command = getattr(namespace, "asset_command", "audit")
    is_baseline = asset_command == "baseline"
    base_repository = RepositoryContext.load()
    repository = base_repository.at_root(repository_root)
    selection = select_runtime(
        base_repository,
        profile_name=str(getattr(namespace, "profile", "") or ""),
        preset_name=str(getattr(namespace, "preset", "") or ""),
    )
    selection = replace(selection, repository=repository)
    executable = executable_resolver(namespace, repository_root)
    project = resolve_project(repository, Path(namespace.project_path))
    arguments = [f"--project={project}", "--format=json"]
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
                ExecutableDescription("Asset audit", "DurinAssetTool", "DurinAssetTool"),
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
            print("\nAsset baseline rejected: every package must be current DAST v5 with no schema findings.", file=stdout)
        else:
            print(f"Asset baseline: {len(report['packages'])} current DAST v5 package(s).", file=stdout)
    else:
        _render_human(report, stdout)
    if is_baseline:
        return POLICY_EXIT_CODE if _baseline_failed(report) else 0
    return POLICY_EXIT_CODE if _policy_failed(report, set(namespace.fail_on)) else 0
