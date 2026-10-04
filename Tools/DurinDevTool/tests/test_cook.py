from __future__ import annotations

import io
import json
from dataclasses import replace
from pathlib import Path
from unittest import mock

import pytest

from durin_dev_tool import cook
from durin_dev_tool.build.errors import BuildToolError
from durin_dev_tool.context import RepositoryContext
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)


def report(*, status: str = "succeeded", target: str = "win64") -> str:
    return json.dumps(
        {
            "schemaVersion": 1,
            "status": status,
            "code": status,
            "diagnostic": "done",
            "target": target,
            "profile": "game",
            "changedBytes": 12,
            "reusedBytes": 4,
            "peakCapturedBytes": 12,
            "rangeReadCount": 0,
            "wallTimeNanoseconds": 20,
            "commitTimeNanoseconds": 5,
            "rollbackTimeNanoseconds": 0,
            "packages": [
                {
                    "packagePath": "/Game/Root",
                    "contributor": "generic-package",
                    "status": "captured",
                    "stage": "capture",
                    "code": "captured",
                    "diagnostic": "captured",
                    "packageBytes": 12,
                    "segmentBytes": 0,
                }
            ],
        }
    )


def test_cook_command_grammar_is_top_level_and_explicit() -> None:
    _, namespace = CommandRegistry().parse(
        [
            "cook",
            "--output",
            "Saved/Cooked",
            "--target",
            "win64",
            "--target-profile",
            "game",
            "--root",
            "/Game/Root",
            "--root",
            "/Engine/Default",
            "--no-incremental",
            "--dry-run",
            "--json",
        ]
    )
    assert namespace.roots == ["/Game/Root", "/Engine/Default"]
    assert namespace.no_incremental
    assert namespace.dry_run
    assert namespace.format_name == "json"


def test_macos_cook_target_is_selected_and_reported() -> None:
    _, namespace = CommandRegistry().parse(
        ["cook", "--output", "Saved/MacCook", "--target", "macos",
         "--target-profile", "game"]
    )
    assert namespace.target == "macos"
    assert cook._read_report(report(target="macos"))["target"] == "macos"


def test_cook_maps_stable_native_contract_and_renders_json(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    namespace = type(
        "Namespace",
        (),
        {
            "profile": "",
            "preset": "",
            "project_path": project,
            "output_path": Path("Saved/Cooked"),
            "target": "win64",
            "target_profile": "game",
            "roots": ["/Game/Root", "/Engine/Default"],
            "no_incremental": True,
            "dry_run": True,
            "format_name": "json",
        },
    )()
    calls: list[list[str]] = []

    def command_runner(arguments: list[str], **_kwargs: object) -> str:
        calls.append(arguments)
        return report()

    output = io.StringIO()
    repository = REPOSITORY.at_root(tmp_path)
    selection = replace(cook.select_runtime(REPOSITORY), repository=repository)
    with mock.patch.object(
        RepositoryContext,
        "load",
        side_effect=AssertionError("repository context was rediscovered"),
    ), mock.patch.object(cook, "select_runtime", return_value=selection) as select:
        assert cook.run(
            namespace,
            repository_root=tmp_path,
            repository_context=repository,
            stdout=output,
            stderr=io.StringIO(),
            executable_resolver=lambda *_args: executable,
            command_runner=command_runner,
        ) == 0
    select.assert_called_once_with(repository, profile_name="", preset_name="")
    assert calls == [
        [
            str(executable),
            "cook",
            f"--project={project}",
            f"--output={(tmp_path / 'Saved/Cooked').resolve()}",
            "--target=win64",
            "--profile=game",
            "--json",
            "--root=/Game/Root",
            "--root=/Engine/Default",
            "--no-incremental",
            "--dry-run",
        ]
    ]
    assert json.loads(output.getvalue())["schemaVersion"] == 1


def test_cook_rejects_malformed_or_nondeterministic_reports() -> None:
    with pytest.raises(DevToolError, match="malformed JSON"):
        cook._read_report("not-json")
    value = json.loads(report())
    value["packages"] = [
        {**value["packages"][0], "packagePath": "/Game/Z"},
        {**value["packages"][0], "packagePath": "/Game/A"},
    ]
    with pytest.raises(DevToolError, match="not deterministic"):
        cook._read_report(json.dumps(value))


def test_failed_cook_reads_complete_report_from_process_log(tmp_path: Path) -> None:
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    log = tmp_path / "cook.log"
    log.write_text(report(status="failed", target="macos"), encoding="utf-8")
    _, namespace = CommandRegistry().parse(
        ["cook", "--project", str(project), "--output", str(tmp_path / "Cook"),
         "--target", "macos", "--target-profile", "game", "--json"]
    )
    repository = REPOSITORY.at_root(tmp_path)
    selection = replace(cook.select_runtime(REPOSITORY), repository=repository)
    output = io.StringIO()
    with mock.patch.object(cook, "select_runtime", return_value=selection), \
            mock.patch.object(cook, "invoke_runtime_program",
                side_effect=BuildToolError("Cook failed", exit_code=1,
                    output_excerpt="{\n  partial", log_path=log)):
        assert cook.run(namespace, repository_root=tmp_path,
            repository_context=repository, stdout=output, stderr=io.StringIO(),
            executable_resolver=lambda *_args: tmp_path / "DurinAssetTool") == 1
    assert json.loads(output.getvalue())["target"] == "macos"
