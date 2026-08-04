"""Read-only asset compatibility command orchestration."""

from __future__ import annotations

import argparse
import json
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
    return directory / f"DurinAssetAudit{profile.test_executable_suffix}"


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
    executable = executable_resolver(namespace, repository_root)
    if not executable.is_file():
        raise DevToolError(
            f'Asset audit executable was not found: "{executable}". '
            "Build it with 'DevTool build --target DurinAssetAudit'."
        )
    project = Path(namespace.project_path)
    if not project.is_absolute():
        project = repository_root / project
    if not project.is_file():
        raise DevToolError(f'Project descriptor was not found: "{project}".')
    completed = process_runner(
        [str(executable), f"--project={project}", "--format=json"],
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
    report = _read_report(completed.stdout)
    if namespace.format_name == "json":
        print(json.dumps(report, separators=(",", ":"), ensure_ascii=False), file=stdout)
    else:
        _render_human(report, stdout)
    return POLICY_EXIT_CODE if _policy_failed(report, set(namespace.fail_on)) else 0
