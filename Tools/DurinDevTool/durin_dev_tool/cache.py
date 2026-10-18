"""Explicit-root offline cache maintenance; format the native bounded report."""
from __future__ import annotations

import argparse
import io
import json
from pathlib import Path
from typing import TextIO

from .build.models import OutputMode
from .build.output import BuildOutput
from .context import RepositoryContext
from .errors import DevToolError
from .json_contract import parse_json_contract, JsonContractError
from .runtime_program import ExecutableDescription, RuntimeProcessPolicy, invoke_runtime_program, select_runtime

EXECUTABLE = ExecutableDescription("DDC maintenance", "DurinCacheTool", "DurinCacheTool")


def run(namespace: argparse.Namespace, *, repository_context: RepositoryContext,
        stdout: TextIO, stderr: TextIO, command_runner=None, **_kwargs) -> int:
    limits = (namespace.max_entries, namespace.max_file_bytes, namespace.max_read_bytes, namespace.max_working_bytes)
    if any(value <= 0 or value > 2**64 - 1 for value in limits) or namespace.max_entries > 1000000:
        raise DevToolError("Cache limits must be positive; --max-entries is at most 1000000.")
    # Preserve symbolic links for the native containment check instead of resolving them here.
    root = namespace.root if namespace.root.is_absolute() else repository_context.root / namespace.root
    selection = select_runtime(repository_context, profile_name=namespace.profile or "", preset_name=namespace.preset or "")
    arguments = [namespace.operation, f"--root={root.absolute()}",
                 f"--max-entries={namespace.max_entries}", f"--max-file-bytes={namespace.max_file_bytes}",
                 f"--max-read-bytes={namespace.max_read_bytes}", f"--max-working-bytes={namespace.max_working_bytes}"]
    output = invoke_runtime_program(selection, EXECUTABLE, arguments,
        output=BuildOutput(color="never", output_mode=OutputMode.COMPACT, stdout=io.StringIO(), stderr=stderr),
        policy=RuntimeProcessPolicy(interruption_message="Cache maintenance canceled; rerun to continue.", capture_output=True),
        command_runner=command_runner)
    try:
        report = parse_json_contract(output, label="DDC maintenance report", source="from DurinCacheTool",
            schema_path=Path(__file__).resolve().parents[1] / "schemas/cache-maintenance-v1.schema.json")
        if report["operation"] != namespace.operation or Path(report["root"]) != root.absolute():
            raise DevToolError("DDC maintenance report does not match the requested operation/root.")
    except JsonContractError as error:
        raise DevToolError(str(error)) from error
    if namespace.output_format == "json":
        print(json.dumps(report, separators=(",", ":"), ensure_ascii=False), file=stdout)
    elif not report["complete"]:
        print(report.get("diagnostic", "Cache maintenance failed."), file=stderr)
    else:
        print(f"Cache {namespace.operation}: {report['root']}", file=stdout)
        print(f"Records: {report['recordFiles']} files / {report['recordBytes']} bytes; "
              f"values: {report['valueFiles']} files / {report['valueBytes']} bytes.", file=stdout)
        print(f"Verified: {report['verifiedFiles']}; invalid: {report['invalidFiles']}; "
              f"missing: {report['missingValues']}; unsupported: {report['unsupportedFiles']}; "
              f"removed: {report['removedFiles']} files / {report['removedBytes']} bytes; "
              f"unrelated root entries retained: {report['otherRootEntries']}.", file=stdout)
        for diagnostic in report["diagnostics"]:
            print(f"  {diagnostic}", file=stdout)
    return 0 if report["complete"] and report["valid"] else 1
