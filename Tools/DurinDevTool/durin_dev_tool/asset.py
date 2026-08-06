"""Asset compatibility audit and explicit migration orchestration."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence, TextIO

from .build.config import (
    load_configure_presets,
    load_local_config,
    load_profiles,
    select_preset,
    select_profile,
)
from .build.locations import resolve_location
from .errors import DevToolError

POLICY_EXIT_CODE = 3
SCHEMA_VERSION = 1
MIGRATION_SCHEMA_VERSION = 1
CURRENT_ASSET_FORMAT_VERSION = 3
INSPECTION_NAMES = {"NotChecked", "Ready", "Failed"}
COMPATIBILITY_NAMES = {"Compatible", "Incompatible", "Unsupported"}
FRESHNESS_NAMES = {"Current", "Stale"}
FINDING_CODES = {
    "UnknownField",
    "IncompatibleFieldSignature",
    "UnavailableClass",
    "UnsupportedPackageFormat",
    "InvalidObjectGraph",
    "CorruptPackage",
    "IoFailure",
}
MIGRATION_KINDS = {"PackageFormat", "AssetSchema"}
MIGRATION_RISKS = {"Lossless", "DataLoss", "Unknown"}
SHA256_PATTERN = re.compile(r"^sha256:[0-9a-f]{64}$")


def _runtime_executable(
    namespace: argparse.Namespace, repository_root: Path
) -> Path:
    config = load_local_config()
    profile = select_profile(
        load_profiles(),
        requested=str(getattr(namespace, "profile", "") or ""),
        configured=config.default_build_profile,
    )
    preset = select_preset(
        profile,
        load_configure_presets(),
        requested=str(getattr(namespace, "preset", "") or ""),
    )
    directory = resolve_location(
        "runtime", profile=profile, preset=preset, root=repository_root
    ).path
    return directory / f"DurinAssetTool{profile.test_executable_suffix}"


def _validate_report(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict) or value.get("schemaVersion") != SCHEMA_VERSION:
        raise DevToolError("Asset audit returned an unsupported report schema.")
    packages = value.get("packages")
    if not isinstance(packages, list):
        raise DevToolError("Asset audit report field 'packages' must be an array.")
    previous_path = ""
    for package in packages:
        if not isinstance(package, dict):
            raise DevToolError("Asset audit report contains an invalid package record.")
        path = package.get("packagePath")
        findings = package.get("findings")
        if not isinstance(path, str) or not path or path < previous_path:
            raise DevToolError("Asset audit package order is not deterministic.")
        previous_path = path
        if package.get("inspection") not in INSPECTION_NAMES:
            raise DevToolError("Asset audit returned an unknown inspection name.")
        if package.get("compatibility") not in COMPATIBILITY_NAMES:
            raise DevToolError("Asset audit returned an unknown compatibility name.")
        if package.get("freshness") not in FRESHNESS_NAMES:
            raise DevToolError("Asset audit returned an unknown freshness name.")
        if not isinstance(findings, list) or any(
            not isinstance(finding, dict)
            or finding.get("code") not in FINDING_CODES
            for finding in findings
        ):
            raise DevToolError("Asset audit returned an unknown finding code.")
    return value


def _read_report(output: str) -> dict[str, Any]:
    try:
        return _validate_report(json.loads(output))
    except json.JSONDecodeError as exc:
        raise DevToolError(
            f"Asset audit returned invalid JSON at line {exc.lineno}, column {exc.colno}."
        ) from exc


def _validate_migration_report(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict) or value.get("schemaVersion") != MIGRATION_SCHEMA_VERSION:
        raise DevToolError("Asset migration returned an unsupported report schema.")
    if set(value) != {"schemaVersion", "operation", "result", "packages", "summary", "changedPaths"}:
        raise DevToolError("Asset migration returned unknown or missing report fields.")
    operation = value.get("operation")
    if operation not in {"Plan", "Apply"} or value.get("result") not in {
        "Ready", "Succeeded", "Blocked", "Failed", "Cancelled", "RolledBack"
    }:
        raise DevToolError("Asset migration returned invalid operation or result names.")
    packages = value.get("packages")
    summary = value.get("summary")
    if not isinstance(packages, list) or not isinstance(summary, dict):
        raise DevToolError("Asset migration report has invalid package or summary fields.")
    previous_path = ""
    counts = {name: 0 for name in ("planned", "migrated", "skipped", "blocked", "failed", "rolledBack")}
    status_to_count = {
        "Planned": "planned", "Migrated": "migrated", "Skipped": "skipped",
        "Blocked": "blocked", "Failed": "failed", "RolledBack": "rolledBack",
    }
    for package in packages:
        if not isinstance(package, dict):
            raise DevToolError("Asset migration report contains an invalid package record.")
        if set(package) != {
            "packagePath", "physicalPath", "status", "fingerprint",
            "sourceFormatVersion", "targetFormatVersion", "steps", "diagnostics",
        }:
            raise DevToolError("Asset migration returned unknown or missing package fields.")
        path = package.get("packagePath")
        status = package.get("status")
        fingerprint = package.get("fingerprint")
        steps = package.get("steps")
        diagnostics = package.get("diagnostics")
        if (
            not isinstance(path, str)
            or not path
            or (bool(previous_path) and path <= previous_path)
        ):
            raise DevToolError("Asset migration package order is not deterministic.")
        previous_path = path
        allowed_statuses = {"Planned", "Skipped", "Blocked", "Failed"} if operation == "Plan" else {
            "Migrated", "Skipped", "Blocked", "Failed", "RolledBack"
        }
        if status not in allowed_statuses:
            raise DevToolError("Asset migration returned an unknown package status.")
        counts[status_to_count[status]] += 1
        if (
            not isinstance(package.get("physicalPath"), str)
            or not package["physicalPath"]
            or not isinstance(package.get("sourceFormatVersion"), int)
            or isinstance(package["sourceFormatVersion"], bool)
            or package["sourceFormatVersion"] < 0
            or not isinstance(package.get("targetFormatVersion"), int)
            or isinstance(package["targetFormatVersion"], bool)
            or package["targetFormatVersion"] < 0
        ):
            raise DevToolError("Asset migration returned invalid package identity or versions.")
        if (
            not isinstance(fingerprint, dict)
            or set(fingerprint) != {"fileSize", "lastWriteTimeTicks", "contentHash"}
            or not isinstance(fingerprint.get("fileSize"), int)
            or isinstance(fingerprint["fileSize"], bool)
            or fingerprint["fileSize"] < 0
            or not isinstance(fingerprint.get("lastWriteTimeTicks"), int)
            or isinstance(fingerprint["lastWriteTimeTicks"], bool)
            or not SHA256_PATTERN.fullmatch(str(fingerprint.get("contentHash", "")))
        ):
            raise DevToolError("Asset migration returned an invalid content fingerprint.")
        if not isinstance(steps, list) or any(
            not isinstance(step, dict)
            or set(step) != {"handlerId", "kind", "sourceVersion", "targetVersion", "risk"}
            or not isinstance(step.get("handlerId"), str)
            or not step["handlerId"]
            or step.get("kind") not in MIGRATION_KINDS
            or step.get("risk") not in MIGRATION_RISKS
            or not isinstance(step.get("sourceVersion"), int)
            or isinstance(step["sourceVersion"], bool)
            or step["sourceVersion"] < 0
            or not isinstance(step.get("targetVersion"), int)
            or isinstance(step["targetVersion"], bool)
            or step["targetVersion"] < 0
            for step in steps
        ):
            raise DevToolError("Asset migration returned an invalid migration step.")
        if not isinstance(diagnostics, list) or any(not isinstance(item, str) for item in diagnostics):
            raise DevToolError("Asset migration returned invalid diagnostics.")
        if status == "Planned" and (
            not steps or any(step["risk"] != "Lossless" for step in steps)
        ):
            raise DevToolError("Asset migration planned a package without a lossless chain.")
    changed_paths = value.get("changedPaths")
    if not isinstance(changed_paths, list) or any(not isinstance(path, str) or not path for path in changed_paths):
        raise DevToolError("Asset migration returned invalid changed paths.")
    if operation == "Plan":
        expected_result = "Blocked" if counts["blocked"] or counts["failed"] else "Ready"
        if changed_paths or value["result"] != expected_result:
            raise DevToolError("Asset migration dry-run summary or changed paths are invalid.")
    elif value["result"] == "Succeeded":
        if counts["planned"] or counts["failed"] or counts["rolledBack"] or len(changed_paths) != counts["migrated"]:
            raise DevToolError("Asset migration apply success report is inconsistent.")
    elif changed_paths:
        raise DevToolError("A failed or rolled-back migration may not report changed paths.")
    if summary != counts:
        raise DevToolError("Asset migration report summary is inconsistent.")
    return value


def _read_migration_report(output: str) -> dict[str, Any]:
    try:
        return _validate_migration_report(json.loads(output))
    except json.JSONDecodeError as exc:
        raise DevToolError(
            f"Asset migration returned invalid JSON at line {exc.lineno}, column {exc.colno}."
        ) from exc


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


def _render_migration_human(report: Mapping[str, Any], stdout: TextIO) -> None:
    summary: Mapping[str, int] = report["summary"]
    print(
        f"Asset migration {str(report['operation']).lower()}: "
        f"{summary['planned']} planned, {summary['migrated']} migrated, "
        f"{summary['skipped']} skipped, {summary['blocked']} blocked, "
        f"{summary['failed']} failed, {summary['rolledBack']} rolled back.",
        file=stdout,
    )
    for label in ("Planned", "Migrated", "Skipped", "Blocked", "Failed", "RolledBack"):
        selected = [package for package in report["packages"] if package["status"] == label]
        if not selected:
            continue
        print(f"\n{label} ({len(selected)}):", file=stdout)
        for package in selected:
            print(f"  {package['packagePath']}", file=stdout)
            for step in package["steps"]:
                print(
                    f"    [{step['kind']}] {step['handlerId']}: "
                    f"{step['sourceVersion']} -> {step['targetVersion']} ({step['risk']})",
                    file=stdout,
                )
            for diagnostic in package["diagnostics"]:
                print(f"    {diagnostic}", file=stdout)


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
        report["result"] != "Ready"
        or not packages
        or any(
            package["status"] != "Skipped"
            or package["sourceFormatVersion"] != CURRENT_ASSET_FORMAT_VERSION
            or package["targetFormatVersion"] != CURRENT_ASSET_FORMAT_VERSION
            or package["steps"]
            or package["diagnostics"]
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
    is_migrate = asset_command in {"migrate", "baseline"}
    is_apply = is_migrate and getattr(namespace, "apply", False)
    executable = executable_resolver(namespace, repository_root)
    if not executable.is_file():
        raise DevToolError(
            f'Asset audit executable was not found: "{executable}". '
            "Build it with 'DevTool build --target DurinAssetTool'."
        )
    project = Path(namespace.project_path)
    if not project.is_absolute():
        project = repository_root / project
    if not project.is_file():
        raise DevToolError(f'Project descriptor was not found: "{project}".')
    arguments = [str(executable), f"--project={project}", "--format=json"]
    if is_migrate:
        arguments.append("--operation=migrate")
        if is_apply:
            arguments.append("--apply")
        arguments.extend(f"--mount={value}" for value in getattr(namespace, "mounts", ()))
        arguments.extend(f"--package={value}" for value in getattr(namespace, "packages", ()))
    completed = process_runner(
        arguments,
        cwd=repository_root,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode == 130:
        print("Asset migration cancelled." if is_migrate else "Asset compatibility audit cancelled.", file=stderr)
        return 130
    if completed.returncode != 0:
        diagnostic = completed.stderr.strip() or "native audit process failed"
        operation = "migration apply" if is_apply else ("migration planning" if is_migrate else "compatibility audit")
        raise DevToolError(f"Asset {operation} failed: {diagnostic}")
    report = _read_migration_report(completed.stdout) if is_migrate else _read_report(completed.stdout)
    report_path_value = getattr(namespace, "report_path", None)
    if is_migrate and report_path_value is not None:
        report_path = Path(report_path_value)
        if not report_path.is_absolute():
            report_path = repository_root / report_path
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(
            json.dumps(report, separators=(",", ":"), ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
    if namespace.format_name == "json":
        print(json.dumps(report, separators=(",", ":"), ensure_ascii=False), file=stdout)
    elif is_baseline:
        if _baseline_failed(report):
            _render_migration_human(report, stdout)
            print("\nAsset baseline rejected: every package must be current DAST v3 with no schema findings.", file=stdout)
        else:
            print(f"Asset baseline: {len(report['packages'])} current DAST v3 package(s).", file=stdout)
    elif is_migrate:
        _render_migration_human(report, stdout)
    else:
        _render_human(report, stdout)
    if is_migrate and report_path_value is not None and namespace.format_name != "json":
        print(f"\nReport: {report_path}", file=stdout)
    if is_baseline:
        return POLICY_EXIT_CODE if _baseline_failed(report) else 0
    if is_migrate:
        if report["result"] == "Cancelled":
            return 130
        if is_apply and report["result"] in {"Failed", "RolledBack"}:
            return 1
        return POLICY_EXIT_CODE if report["result"] in {"Blocked", "Failed"} else 0
    return POLICY_EXIT_CODE if _policy_failed(report, set(namespace.fail_on)) else 0
