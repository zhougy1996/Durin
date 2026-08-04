from __future__ import annotations

import io
import json
import subprocess
import sys
from pathlib import Path

import pytest

DEV_TOOL_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = DEV_TOOL_ROOT.parents[1]
if str(DEV_TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(DEV_TOOL_ROOT))

from durin_dev_tool import cli
from durin_dev_tool import asset
from durin_dev_tool.errors import DevToolError


def package(
    path: str,
    *,
    inspection: str = "Ready",
    compatibility: str = "Compatible",
    freshness: str = "Current",
    code: str | None = None,
) -> dict[str, object]:
    findings = [] if code is None else [{"code": code, "diagnostic": f"{code} diagnostic"}]
    return {
        "packagePath": path,
        "physicalPath": f"C:/{path[1:]}.dasset",
        "inspection": inspection,
        "compatibility": compatibility,
        "freshness": freshness,
        "fileSize": 10,
        "lastWriteTimeTicks": 20,
        "findings": findings,
    }


def report(*packages: dict[str, object]) -> str:
    return json.dumps({"schemaVersion": 1, "packages": list(packages)})


def run_handler(tmp_path: Path, report_text: str, *fail_on: str, format_name: str = "json") -> tuple[int, str, str]:
    executable = tmp_path / "DurinAssetAudit.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    namespace = type("Namespace", (), {
        "project_path": project,
        "format_name": format_name,
        "fail_on": fail_on,
    })()
    output = io.StringIO()
    errors = io.StringIO()
    result = asset.run(
        namespace,
        repository_root=tmp_path,
        stdout=output,
        stderr=errors,
        executable_resolver=lambda _namespace, _root: executable,
        process_runner=lambda *_args, **_kwargs: subprocess.CompletedProcess([], 0, report_text, ""),
    )
    return result, output.getvalue(), errors.getvalue()


def test_registry_requires_project_and_validates_format() -> None:
    with pytest.raises(DevToolError, match="required"):
        cli.run(["asset", "audit"], stdout=io.StringIO(), stderr=io.StringIO())
    with pytest.raises(DevToolError, match="invalid choice"):
        cli.run(
            ["asset", "audit", "--project", "Test.dproject", "--format", "xml"],
            stdout=io.StringIO(), stderr=io.StringIO(),
        )


def test_json_schema_names_and_order_are_preserved(tmp_path: Path) -> None:
    result, output, _ = run_handler(
        tmp_path,
        report(
            package("/Engine/A"),
            package("/Game/B", compatibility="Incompatible", code="UnknownField"),
        ),
    )
    assert result == 0
    parsed = json.loads(output)
    assert parsed["schemaVersion"] == 1
    assert [item["packagePath"] for item in parsed["packages"]] == ["/Engine/A", "/Game/B"]
    assert parsed["packages"][1]["findings"][0]["code"] == "UnknownField"


def test_checked_in_schema_freezes_public_enum_names() -> None:
    schema = json.loads(
        (REPOSITORY_ROOT / "Tools/DurinDevTool/schemas/asset-audit-v1.schema.json").read_text(
            encoding="utf-8"
        )
    )
    package_properties = schema["$defs"]["package"]["properties"]
    finding_properties = schema["$defs"]["finding"]["properties"]
    assert schema["properties"]["schemaVersion"]["const"] == asset.SCHEMA_VERSION
    assert set(package_properties["inspection"]["enum"]) == asset.INSPECTION_NAMES
    assert set(package_properties["compatibility"]["enum"]) == asset.COMPATIBILITY_NAMES
    assert set(package_properties["freshness"]["enum"]) == asset.FRESHNESS_NAMES
    assert set(finding_properties["code"]["enum"]) == asset.FINDING_CODES


@pytest.mark.parametrize(
    ("policies", "expected"),
    [
        ((), 0),
        (("incompatible",), 3),
        (("unsupported",), 3),
        (("error",), 3),
        (("incompatible", "unsupported", "error"), 3),
    ],
)
def test_failure_policies_combine_by_logical_or(tmp_path: Path, policies: tuple[str, ...], expected: int) -> None:
    text = report(
        package("/Game/A", compatibility="Incompatible", code="UnknownField"),
        package("/Game/B", compatibility="Unsupported", code="UnsupportedPackageFormat"),
        package("/Game/C", inspection="Failed", compatibility="Unsupported", code="CorruptPackage"),
    )
    result, _, _ = run_handler(tmp_path, text, *policies)
    assert result == expected


def test_human_output_groups_orthogonal_states(tmp_path: Path) -> None:
    result, output, _ = run_handler(
        tmp_path,
        report(package("/Game/A", inspection="Failed", compatibility="Unsupported", freshness="Stale", code="IoFailure")),
        format_name="human",
    )
    assert result == 0
    assert "0 compatible, 0 incompatible, 1 unsupported, 1 failed, 1 stale" in output
    assert "Unsupported (1):" in output
    assert "Failed (1):" in output
    assert "Stale (1):" in output


def test_rejects_unstable_order_and_unknown_schema_names(tmp_path: Path) -> None:
    with pytest.raises(DevToolError, match="order"):
        run_handler(tmp_path, report(package("/Game/B"), package("/Game/A")))
    with pytest.raises(DevToolError, match="finding code"):
        run_handler(tmp_path, report(package("/Game/A", code="NewCode")))


def test_cancel_and_process_failure_are_distinct(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetAudit.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.touch()
    namespace = type("Namespace", (), {"project_path": project, "format_name": "json", "fail_on": ()})()
    errors = io.StringIO()
    result = asset.run(
        namespace, repository_root=tmp_path, stdout=io.StringIO(), stderr=errors,
        executable_resolver=lambda _namespace, _root: executable,
        process_runner=lambda *_args, **_kwargs: subprocess.CompletedProcess([], 130, "", ""),
    )
    assert result == 130
    assert "cancelled" in errors.getvalue()
    with pytest.raises(DevToolError, match="scan failed"):
        asset.run(
            namespace, repository_root=tmp_path, stdout=io.StringIO(), stderr=io.StringIO(),
            executable_resolver=lambda _namespace, _root: executable,
            process_runner=lambda *_args, **_kwargs: subprocess.CompletedProcess([], 1, "", "scan failed"),
        )
