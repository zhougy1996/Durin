from __future__ import annotations

import io
import json
import subprocess
import sys
from pathlib import Path

import pytest
from jsonschema import validate

DEV_TOOL_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = DEV_TOOL_ROOT.parents[1]
if str(DEV_TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(DEV_TOOL_ROOT))

from durin_dev_tool import cli
from durin_dev_tool import asset
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry


FIXTURE_ROOT = Path(__file__).resolve().parent / "fixtures"


def package(
    path: str,
    *,
    inspection: str = "Ready",
    compatibility: str = "Compatible",
    freshness: str = "Current",
    format_version: int = 3,
    code: str | None = None,
) -> dict[str, object]:
    findings = [] if code is None else [{"code": code, "diagnostic": f"{code} diagnostic"}]
    return {
        "packagePath": path,
        "physicalPath": f"C:/{path[1:]}.dasset",
        "formatVersion": format_version,
        "inspection": inspection,
        "compatibility": compatibility,
        "freshness": freshness,
        "fileSize": 10,
        "lastWriteTimeTicks": 20,
        "findings": findings,
    }


def report(*packages: dict[str, object]) -> str:
    return json.dumps({"schemaVersion": 2, "packages": list(packages)})


def run_handler(tmp_path: Path, report_text: str, *fail_on: str, format_name: str = "json") -> tuple[int, str, str]:
    executable = tmp_path / "DurinAssetTool.exe"
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


def test_selected_asset_command_grammar_is_frozen() -> None:
    registry = CommandRegistry()
    _, baseline_namespace = registry.parse(
        ["asset", "baseline", "--project", "Sandbox/Sandbox.dproject", "--format", "json"]
    )
    assert baseline_namespace.asset_command == "baseline"

    _, audit_namespace = registry.parse(
        [
            "asset", "audit", "--project", "Sandbox/Sandbox.dproject",
            "--fail-on", "incompatible", "--fail-on", "unsupported",
            "--fail-on", "error", "--format", "json",
        ]
    )
    assert audit_namespace.asset_command == "audit"
    assert audit_namespace.fail_on == ["incompatible", "unsupported", "error"]

    _, plan_namespace = registry.parse(
        [
            "asset", "migrate", "--project", "Sandbox/Sandbox.dproject",
            "--mount", "/Engine", "--mount", "/Game",
            "--package", "/Game/Levels/NewLevel",
            "--format", "json", "--report", "migration.json",
        ]
    )
    assert plan_namespace.asset_command == "migrate"
    assert plan_namespace.apply is False
    assert plan_namespace.mounts == ["/Engine", "/Game"]
    assert plan_namespace.packages == ["/Game/Levels/NewLevel"]
    assert plan_namespace.report_path == Path("migration.json")

    _, apply_namespace = registry.parse(
        ["asset", "migrate", "--project", "Sandbox/Sandbox.dproject", "--apply"]
    )
    assert apply_namespace.apply is True


@pytest.mark.parametrize(
    ("native_report", "expected"),
    [
        (report(package("/Game/Baseline")), 0),
        (report(package("/Game/Baseline", format_version=2, compatibility="Unsupported", code="UnsupportedPackageFormat")), 3),
        (report(package("/Game/Baseline", format_version=4)), 3),
        (report(package("/Game/Baseline", compatibility="Incompatible", code="UnknownField")), 3),
        (report(), 3),
    ],
)
def test_asset_baseline_requires_current_format_and_schema(
    tmp_path: Path, native_report: str, expected: int
) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    namespace = type("Namespace", (), {
        "asset_command": "baseline",
        "project_path": project,
        "format_name": "human",
    })()
    calls: list[list[str]] = []

    def process_runner(arguments: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(arguments)
        return subprocess.CompletedProcess(arguments, 0, native_report, "")

    output = io.StringIO()
    assert asset.run(
        namespace,
        repository_root=tmp_path,
        stdout=output,
        stderr=io.StringIO(),
        executable_resolver=lambda *_args: executable,
        process_runner=process_runner,
    ) == expected
    assert calls == [[str(executable), f"--project={project}", "--format=json"]]
    assert ("Asset baseline:" in output.getvalue()) == (expected == 0)


def test_migrate_apply_is_forwarded_only_by_explicit_apply(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    apply_report = (FIXTURE_ROOT / "asset-migration-apply-v1.json").read_text(encoding="utf-8")
    namespace = type("Namespace", (), {
        "asset_command": "migrate", "apply": True, "project_path": project,
        "format_name": "json", "mounts": [], "packages": [], "report_path": None,
    })()
    calls: list[list[str]] = []

    def process_runner(arguments: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(arguments)
        return subprocess.CompletedProcess(arguments, 0, apply_report, "")

    assert asset.run(
        namespace, repository_root=tmp_path, stdout=io.StringIO(), stderr=io.StringIO(),
        executable_resolver=lambda *_args: executable, process_runner=process_runner,
    ) == 0
    assert calls == [[
        str(executable), f"--project={project}", "--format=json",
        "--operation=migrate", "--apply",
    ]]


def test_migrate_apply_maps_rollback_to_operational_failure() -> None:
    fixture = json.loads(
        (FIXTURE_ROOT / "asset-migration-apply-v1.json").read_text(encoding="utf-8")
    )
    fixture["result"] = "RolledBack"
    fixture["packages"][0]["status"] = "RolledBack"
    fixture["summary"]["migrated"] = 0
    fixture["summary"]["rolledBack"] = 1
    fixture["changedPaths"] = []
    assert asset._validate_migration_report(fixture)["result"] == "RolledBack"


def test_migration_plan_forwards_filters_validates_and_writes_explicit_report(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    migration_report = json.loads(
        (FIXTURE_ROOT / "asset-migration-plan-v1.json").read_text(encoding="utf-8")
    )
    namespace = type("Namespace", (), {
        "asset_command": "migrate",
        "apply": False,
        "project_path": project,
        "format_name": "human",
        "mounts": ["/Engine", "/Game"],
        "packages": ["/Game/Only"],
        "report_path": Path("Saved/migration.json"),
    })()
    calls: list[list[str]] = []

    def process_runner(arguments: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(arguments)
        return subprocess.CompletedProcess(arguments, 0, json.dumps(migration_report), "")

    output = io.StringIO()
    assert asset.run(
        namespace,
        repository_root=tmp_path,
        stdout=output,
        stderr=io.StringIO(),
        executable_resolver=lambda *_args: executable,
        process_runner=process_runner,
    ) == 0
    assert calls == [[
        str(executable), f"--project={project}", "--format=json", "--operation=migrate",
        "--mount=/Engine", "--mount=/Game", "--package=/Game/Only",
    ]]
    assert "1 planned" in output.getvalue()
    written = json.loads((tmp_path / "Saved/migration.json").read_text(encoding="utf-8"))
    assert written == migration_report


def test_migration_plan_rejects_unstable_or_non_lossless_native_output(tmp_path: Path) -> None:
    fixture = json.loads(
        (FIXTURE_ROOT / "asset-migration-plan-v1.json").read_text(encoding="utf-8")
    )
    fixture["packages"][0]["steps"][0]["risk"] = "Unknown"
    with pytest.raises(DevToolError, match="lossless chain"):
        asset._validate_migration_report(fixture)

    fixture = json.loads(
        (FIXTURE_ROOT / "asset-migration-plan-v1.json").read_text(encoding="utf-8")
    )
    fixture["changedPaths"] = ["C:/authored.dasset"]
    with pytest.raises(DevToolError, match="changed paths"):
        asset._validate_migration_report(fixture)


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
    assert parsed["schemaVersion"] == 2
    assert [item["packagePath"] for item in parsed["packages"]] == ["/Engine/A", "/Game/B"]
    assert parsed["packages"][1]["findings"][0]["code"] == "UnknownField"


def test_checked_in_schema_freezes_public_enum_names() -> None:
    schema = json.loads(
        (REPOSITORY_ROOT / "Tools/DurinDevTool/schemas/asset-audit-v2.schema.json").read_text(
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


def test_checked_in_report_fixtures_match_their_schemas() -> None:
    audit_schema = json.loads(
        (REPOSITORY_ROOT / "Tools/DurinDevTool/schemas/asset-audit-v2.schema.json").read_text(
            encoding="utf-8"
        )
    )
    migration_schema = json.loads(
        (REPOSITORY_ROOT / "Tools/DurinDevTool/schemas/asset-migration-v1.schema.json").read_text(
            encoding="utf-8"
        )
    )
    validate(json.loads((FIXTURE_ROOT / "asset-audit-v2.json").read_text(encoding="utf-8")), audit_schema)
    for name in ("asset-migration-plan-v1.json", "asset-migration-apply-v1.json"):
        fixture = json.loads((FIXTURE_ROOT / name).read_text(encoding="utf-8"))
        validate(fixture, migration_schema)
        assert fixture["schemaVersion"] == asset.MIGRATION_SCHEMA_VERSION


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
    executable = tmp_path / "DurinAssetTool.exe"
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


def test_audit_invocation_is_read_only_and_missing_project_fails_before_launch(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    authored = tmp_path / "Content" / "Test.dasset"
    authored.parent.mkdir()
    authored.write_bytes(b"DAST authored bytes")
    namespace = type(
        "Namespace", (),
        {"asset_command": "audit", "project_path": project, "format_name": "json", "fail_on": []},
    )()
    before = {path: path.read_bytes() for path in tmp_path.rglob("*") if path.is_file()}
    calls: list[tuple[list[str], Path]] = []

    def process_runner(arguments: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append((arguments, kwargs["cwd"]))
        return subprocess.CompletedProcess(arguments, 0, report(), "")

    assert asset.run(
        namespace,
        repository_root=tmp_path,
        stdout=io.StringIO(),
        stderr=io.StringIO(),
        executable_resolver=lambda *_args: executable,
        process_runner=process_runner,
    ) == 0
    after = {path: path.read_bytes() for path in tmp_path.rglob("*") if path.is_file()}
    assert before == after
    assert calls == [
        ([str(executable), f"--project={project}", "--format=json"], tmp_path)
    ]

    project.unlink()
    with pytest.raises(DevToolError, match="Project descriptor was not found"):
        asset.run(
            namespace,
            repository_root=tmp_path,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
            executable_resolver=lambda *_args: executable,
            process_runner=lambda *_args, **_kwargs: pytest.fail("missing project must not launch"),
        )
