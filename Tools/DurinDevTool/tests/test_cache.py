from __future__ import annotations

import io
import json
from pathlib import Path
from unittest import mock

import pytest

from durin_dev_tool import cache
from durin_dev_tool.context import RepositoryContext
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry

REPOSITORY = RepositoryContext.load(Path(__file__).resolve().parents[3])


def report(root: Path, operation: str = "inspect", *, complete: bool = True, valid: bool = True) -> dict:
    value = dict(schemaVersion=1, root=str(root), operation=operation, complete=complete, valid=valid)
    if complete:
        value.update({key: 0 for key in ("recordFiles", "recordBytes", "valueFiles", "valueBytes", "readBytes", "verifiedFiles", "invalidFiles", "unsupportedFiles", "missingValues", "removedFiles", "removedBytes", "otherRootEntries")})
        value["diagnostics"] = []
    else:
        value.update(diagnostic="Cache root is in use", errorCode=3)
    return value


@pytest.mark.parametrize("operation", ["inspect", "verify", "clear"])
def test_forwards_explicit_root_and_limits_without_project_or_engine_options(tmp_path, operation):
    _, args = CommandRegistry().parse(["cache", operation, "--root", str(tmp_path), "--max-read-bytes", "4096", "--format", "json"])
    stdout, stderr = io.StringIO(), io.StringIO()
    with mock.patch.object(cache, "select_runtime") as select, mock.patch.object(cache, "invoke_runtime_program", return_value=json.dumps(report(tmp_path, operation))) as invoke:
        assert cache.run(args, repository_context=REPOSITORY, stdout=stdout, stderr=stderr) == 0
    command = invoke.call_args.args[2]
    assert command[0] == operation and f"--root={tmp_path}" in command
    assert "--max-read-bytes=4096" in command
    assert not any("project" in argument for argument in command)
    assert json.loads(stdout.getvalue())["root"] == str(tmp_path)
    assert select.call_count == 1


@pytest.mark.parametrize("complete,valid", [(False, False), (True, False)])
def test_failed_or_incomplete_reports_preserve_details_and_fail_the_command(tmp_path, complete, valid):
    _, args = CommandRegistry().parse(["cache", "verify", "--root", str(tmp_path), "--format", "json"])
    value = report(tmp_path, "verify", complete=complete, valid=valid)
    if complete:
        value["invalidFiles"] = 1
    output = io.StringIO()
    with mock.patch.object(cache, "select_runtime"), mock.patch.object(cache, "invoke_runtime_program", return_value=json.dumps(value)):
        assert cache.run(args, repository_context=REPOSITORY, stdout=output, stderr=io.StringIO()) == 1
    assert json.loads(output.getvalue()) == value


@pytest.mark.parametrize("change", [lambda r: r.pop("recordFiles"), lambda r: r.update(root="/other"), lambda r: r.update(operation="clear"), lambda r: r.update(schemaVersion=2), lambda r: r.update(recordBytes=-1)])
def test_rejects_malformed_or_mismatched_reports(tmp_path, change):
    _, args = CommandRegistry().parse(["cache", "inspect", "--root", str(tmp_path)])
    value = report(tmp_path); change(value)
    with mock.patch.object(cache, "select_runtime"), mock.patch.object(cache, "invoke_runtime_program", return_value=json.dumps(value)):
        with pytest.raises(DevToolError):
            cache.run(args, repository_context=REPOSITORY, stdout=io.StringIO(), stderr=io.StringIO())


def test_requires_root_and_rejects_invalid_limits_before_native_invocation(tmp_path):
    with pytest.raises(DevToolError):
        CommandRegistry().parse(["cache", "clear"])
    _, args = CommandRegistry().parse(["cache", "clear", "--root", str(tmp_path), "--max-entries", "0"])
    with mock.patch.object(cache, "invoke_runtime_program") as invoke:
        with pytest.raises(DevToolError):
            cache.run(args, repository_context=REPOSITORY, stdout=io.StringIO(), stderr=io.StringIO())
    invoke.assert_not_called()
