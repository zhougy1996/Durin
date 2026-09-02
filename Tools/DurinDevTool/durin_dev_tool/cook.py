"""Project Cook command hosted by DurinAssetTool."""

from __future__ import annotations

import argparse
import io
import json
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence, TextIO

from .build.errors import BuildToolError
from .build.models import OutputMode
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
COOK_EXECUTABLE = ExecutableDescription(
    "Project Cook",
    "DurinAssetTool",
    "DurinAssetTool",
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
    repository_context: RepositoryContext,
    stdout: TextIO,
    stderr: TextIO,
    command_runner: Callable[..., str] | None = None,
    executable_resolver: Callable[[argparse.Namespace, Path], Path] | None = None,
    **_kwargs: object,
) -> int:
    repository = repository_context
    selection = select_runtime(
        repository,
        profile_name=str(getattr(namespace, "profile", "") or ""),
        preset_name=str(getattr(namespace, "preset", "") or ""),
    )
    executable = (
        executable_resolver(namespace, repository.root)
        if executable_resolver
        else locate_executable(selection, COOK_EXECUTABLE)
    )
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

    process_output = BuildOutput(
        plain=True,
        output_mode=OutputMode.COMPACT,
        stdout=io.StringIO(),
        stderr=stderr,
    )
    try:
        native_output = invoke_runtime_program(
            selection,
            COOK_EXECUTABLE,
            arguments,
            output=process_output,
            policy=RuntimeProcessPolicy(
                interruption_message="Project Cook cancelled.",
                show_heartbeat=True,
                capture_output=True,
            ),
            executable_override=executable,
            command_runner=command_runner,
        )
    except BuildToolError as error:
        if error.exit_code == 130:
            print("Project Cook cancelled.", file=stderr)
            return 130
        if not error.output_excerpt.strip():
            raise
        native_output = error.output_excerpt

    report = _read_report(native_output)
    if namespace.format_name == "json":
        print(json.dumps(report, separators=(",", ":"), ensure_ascii=False), file=stdout)
    else:
        _render_human(report, stdout)
    if report["status"] == "cancelled":
        return 130
    return 0 if report["status"] == "succeeded" else 1
