"""Project Cook command hosted by DurinAssetTool."""

from __future__ import annotations

import argparse
import io
import json
import subprocess
from dataclasses import replace
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence, TextIO

from .build.config import BuildToolError, OutputMode
from .build.output import BuildOutput
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

SCHEMA_DIRECTORY = Path(__file__).resolve().parents[1] / "schemas"


def _runtime_executable(namespace: argparse.Namespace, repository_root: Path) -> Path:
    repository = RepositoryContext.load().at_root(repository_root)
    selection = select_runtime(
        repository,
        profile_name=str(getattr(namespace, "profile", "") or ""),
        preset_name=str(getattr(namespace, "preset", "") or ""),
    )
    return locate_executable(
        selection,
        ExecutableDescription("Project Cook", "DurinAssetTool", "DurinAssetTool"),
    )


def _read_report(output: str) -> dict[str, Any]:
    try:
        value = parse_json_contract(
            output,
            label="Cook run report",
            source="from DurinAssetTool",
            schema_path=SCHEMA_DIRECTORY / "cook-run-v1.schema.json",
        )
    except JsonContractError as exc:
        raise DevToolError(str(exc)) from exc
    assert isinstance(value, dict)
    previous = ""
    for package in value["packages"]:
        path = package["packagePath"]
        if path < previous:
            raise DevToolError("Cook package report order is not deterministic.")
        previous = path
    return value


def _render_human(report: Mapping[str, Any], stdout: TextIO) -> None:
    packages: Sequence[Mapping[str, Any]] = report["packages"]
    hits = sum(package["status"] == "cook-hit" for package in packages)
    failed = sum(package["status"] in {"failed", "unsupported"} for package in packages)
    print(
        f"Cook {report['status']}: {len(packages)} package(s), {hits} Cook hit(s), "
        f"{failed} failed, {report['changedBytes']} changed byte(s), "
        f"{report['reusedBytes']} reused byte(s).",
        file=stdout,
    )
    if report["diagnostic"]:
        print(f"  {report['code']}: {report['diagnostic']}", file=stdout)
    for package in packages:
        if package["status"] not in {"failed", "unsupported", "cancelled"}:
            continue
        print(
            f"  {package['packagePath']} [{package['stage']}] "
            f"{package['code']}: {package['diagnostic']}",
            file=stdout,
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
    base_repository = RepositoryContext.load()
    repository = base_repository.at_root(repository_root)
    selection = select_runtime(
        base_repository,
        profile_name=str(getattr(namespace, "profile", "") or ""),
        preset_name=str(getattr(namespace, "preset", "") or ""),
    )
    selection = replace(selection, repository=repository)
    executable = executable_resolver(namespace, repository_root)
    project_value = getattr(namespace, "project_path", None)
    if project_value is None:
        project_value = repository.config.paths.default_game_project
    project = resolve_project(repository, Path(project_value))
    output_value = Path(namespace.output_path)
    output = (output_value if output_value.is_absolute() else repository_root / output_value).resolve()
    arguments = [
        "cook",
        f"--project={project}",
        f"--output={output}",
        f"--target={namespace.target}",
        f"--profile={namespace.target_profile}",
        "--json",
    ]
    arguments.extend(f"--root={root}" for root in namespace.roots)
    if namespace.no_incremental:
        arguments.append("--no-incremental")
    if namespace.dry_run:
        arguments.append("--dry-run")

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
                ExecutableDescription("Project Cook", "DurinAssetTool", "DurinAssetTool"),
                arguments,
                output=process_output,
                policy=RuntimeProcessPolicy(
                    interruption_message="Project Cook cancelled.",
                    show_heartbeat=True,
                    capture_output=True,
                ),
                executable_override=executable,
            )
        except BuildToolError as error:
            if error.exit_code == 130:
                print("Project Cook cancelled.", file=stderr)
                return 130
            if not error.output_excerpt.strip():
                raise
            native_output = error.output_excerpt
    else:
        completed = process_runner(
            [str(executable), *arguments],
            cwd=repository_root,
            text=True,
            capture_output=True,
            check=False,
        )
        if completed.returncode == 130:
            print("Project Cook cancelled.", file=stderr)
            return 130
        if completed.returncode != 0 and not completed.stdout.strip():
            diagnostic = completed.stderr.strip() or "native Cook process failed"
            raise DevToolError(f"Project Cook failed: {diagnostic}")
        native_output = completed.stdout

    report = _read_report(native_output)
    if namespace.format_name == "json":
        print(json.dumps(report, separators=(",", ":"), ensure_ascii=False), file=stdout)
    else:
        _render_human(report, stdout)
    if report["status"] == "cancelled":
        return 130
    return 0 if report["status"] == "succeeded" else 1
