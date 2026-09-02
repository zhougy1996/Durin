
import io
import json
from dataclasses import replace
from pathlib import Path
from unittest import mock

import pytest
from jsonschema import validate

DEV_TOOL_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = DEV_TOOL_ROOT.parents[1]
from durin_dev_tool import cli
from durin_dev_tool import asset
from durin_dev_tool.build.errors import BuildToolError
from durin_dev_tool.context import RepositoryContext
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry


FIXTURE_ROOT = Path(__file__).resolve().parent / "fixtures"
REPOSITORY = RepositoryContext.load(REPOSITORY_ROOT)


def package(
    path: str,
    *,
    inspection: str = "Ready",
    compatibility: str = "Compatible",
    freshness: str = "Current",
    format_version: int = 9,
    code: str | None = None,
) -> dict[str, object]:
    findings = [] if code is None else [{
        "code": code,
        "objectPath": "",
        "classIdentity": "",
        "declaringType": "",
        "fieldName": "",
        "storedKind": "",
        "storedTypeSignature": "",
        "expectedKind": "",
        "expectedTypeSignature": "",
        "payloadSize": 0,
        "payloadOffset": 0,
        "diagnostic": f"{code} diagnostic",
    }]
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
        "canonicalizationEvidence": [],
        "deprecatedRouteEvidence": [],
    }


def report(*packages: dict[str, object]) -> str:
    return json.dumps({"schemaVersion": 3, "packages": list(packages)})


def with_canonicalization_evidence(value: dict[str, object]) -> dict[str, object]:
    value["canonicalizationEvidence"] = [{
        "storedIdentity": "Durin::OldAsset",
        "currentIdentity": "Durin::CurrentAsset",
        "kind": "Class",
        "location": "ObjectRecord",
        "logicalPath": "MainAsset",
    }]
    return value


def with_deprecated_route_evidence(value: dict[str, object]) -> dict[str, object]:
    value["deprecatedRouteEvidence"] = [{
        "objectPath": "/Game/Baseline.Baseline",
        "declaringType": "Durin::DExample",
        "storedFieldName": "OldValue",
        "deprecatedPropertyName": "OldValue_DEPRECATED",
        "customVersionGuid": "00000000-0000-0000-0000-000000000001",
        "sourceVersion": 1,
        "deprecatedBefore": 2,
        "migrationTargets": ["CurrentValue"],
    }]
    return value


def run_handler(tmp_path: Path, report_text: str, format_name: str = "json") -> tuple[int, str, str]:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    namespace = type("Namespace", (), {
        "asset_command": "check",
        "project_path": project,
        "format_name": format_name,
        "baseline": False,
    })()
    output = io.StringIO()
    errors = io.StringIO()
    result = asset.run(
        namespace,
        repository_root=tmp_path,
        repository_context=REPOSITORY,
        stdout=output,
        stderr=errors,
        executable_resolver=lambda _namespace, _root: executable,
        command_runner=lambda *_args, **_kwargs: report_text,
    )
    return result, output.getvalue(), errors.getvalue()


def test_asset_production_path_uses_runtime_program_service(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    namespace = type("Namespace", (), {
        "asset_command": "check",
        "project_path": project,
        "format_name": "json",
        "baseline": False,
    })()
    selection = asset.select_runtime(REPOSITORY)
    with mock.patch.object(asset, "select_runtime", return_value=selection) as select, mock.patch.object(
        asset, "locate_executable", return_value=executable
    ) as locate, mock.patch.object(
        asset, "invoke_runtime_program", return_value=report()
    ) as invoke, mock.patch.object(
        RepositoryContext,
        "load",
        side_effect=AssertionError("repository context was rediscovered"),
    ):
        assert asset.run(
            namespace,
            repository_root=tmp_path,
            repository_context=REPOSITORY,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
        ) == 0
    select.assert_called_once_with(REPOSITORY, profile_name="", preset_name="")
    locate.assert_called_once_with(selection, asset.ASSET_EXECUTABLE)
    assert invoke.call_args.args[2] == ["check", f"--project={project.resolve()}", "--json"]


def test_asset_direct_call_loads_only_the_explicit_repository_root(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    namespace = type("Namespace", (), {
        "asset_command": "check",
        "project_path": project,
        "format_name": "json",
        "baseline": False,
    })()
    with mock.patch.object(
        RepositoryContext, "load", return_value=REPOSITORY
    ) as load, mock.patch.object(asset, "invoke_runtime_program", return_value=report()):
        assert asset.run(
            namespace,
            repository_root=tmp_path,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
            executable_resolver=lambda *_args: executable,
        ) == 0
    assert load.call_args_list == [mock.call(tmp_path)]


def test_registry_defaults_to_check_and_rejects_removed_commands() -> None:
    registry = CommandRegistry()
    _, namespace = registry.parse(["asset"])
    assert namespace.asset_command == "check"
    assert namespace.project_path is None
    _, option_namespace = registry.parse(["asset", "--json"])
    assert option_namespace.asset_command == "check"
    assert option_namespace.format_name == "json"
    for removed_command in ("audit", "storage"):
        with pytest.raises(DevToolError, match="invalid choice"):
            registry.parse(["asset", removed_command])


def test_selected_asset_command_grammar_is_frozen() -> None:
    registry = CommandRegistry()
    _, check_namespace = registry.parse(
        ["asset", "check", "--project", "Sandbox/Sandbox.dproject", "--json"]
    )
    assert check_namespace.asset_command == "check"
    assert check_namespace.format_name == "json"

    _, resave_namespace = registry.parse(
        [
            "asset", "resave", "/Game/Characters", "/Engine/Materials",
            "--apply", "--json",
        ]
    )
    assert resave_namespace.asset_command == "resave"
    assert resave_namespace.scopes == ["/Game/Characters", "/Engine/Materials"]
    assert resave_namespace.apply


def test_asset_check_uses_configured_default_project(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Sandbox" / "Sandbox.dproject"
    project.parent.mkdir()
    project.write_text("{}", encoding="utf-8")
    namespace = type("Namespace", (), {
        "asset_command": "check",
        "project_path": None,
        "format_name": "json",
        "baseline": False,
    })()
    calls: list[tuple[list[str], dict[str, object]]] = []
    repository = replace(
        REPOSITORY,
        config=replace(
            REPOSITORY.config,
            paths=replace(REPOSITORY.config.paths, default_game_project=project),
        ),
    )

    def command_runner(arguments: list[str], **kwargs: object) -> str:
        calls.append((arguments, kwargs))
        return report()

    assert asset.run(
        namespace,
        repository_root=tmp_path,
        repository_context=repository,
        stdout=io.StringIO(),
        stderr=io.StringIO(),
        executable_resolver=lambda *_args: executable,
        command_runner=command_runner,
    ) == 0
    assert len(calls) == 1
    arguments, options = calls[0]
    assert arguments == [str(executable), "check", f"--project={project}", "--json"]
    assert options["show_heartbeat"]
    assert options["capture_output"]
    assert options["cwd"] == REPOSITORY_ROOT


def test_asset_resave_maps_scopes_and_write_intent_to_native_command(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    namespace = type("Namespace", (), {
        "asset_command": "resave",
        "project_path": project,
        "scopes": ["/Game/Characters", "/Engine/Materials/Default"],
        "whole_project": False,
        "apply": True,
        "format_name": "json",
    })()
    calls: list[list[str]] = []

    def command_runner(arguments: list[str], **_kwargs: object) -> str:
        calls.append(arguments)
        return '{"status":"Succeeded"}\n'

    output = io.StringIO()
    assert asset.run(
        namespace,
        repository_root=tmp_path,
        repository_context=REPOSITORY,
        stdout=output,
        stderr=io.StringIO(),
        executable_resolver=lambda *_args: executable,
        command_runner=command_runner,
    ) == 0
    assert calls == [[
        str(executable), "resave", f"--project={project}",
        "/Game/Characters", "/Engine/Materials/Default", "--apply", "--json",
    ]]
    assert json.loads(output.getvalue()) == {"status": "Succeeded"}


def test_asset_resave_requires_exactly_one_selection_style(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")

    for scopes, whole_project in (([], False), (["/Game"], True)):
        namespace = type("Namespace", (), {
            "asset_command": "resave",
            "project_path": project,
            "scopes": scopes,
            "whole_project": whole_project,
            "apply": False,
            "format_name": "human",
        })()
        with pytest.raises(DevToolError, match="scope|--all"):
            asset.run(
                namespace,
                repository_root=tmp_path,
                repository_context=REPOSITORY,
                stdout=io.StringIO(),
                stderr=io.StringIO(),
                executable_resolver=lambda *_args: executable,
                command_runner=lambda *_args, **_kwargs: pytest.fail("invalid selection must not launch"),
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
    assert parsed["schemaVersion"] == 3
    assert [item["packagePath"] for item in parsed["packages"]] == ["/Engine/A", "/Game/B"]
    assert parsed["packages"][1]["findings"][0]["code"] == "UnknownField"


def test_checked_in_schema_freezes_public_enum_names() -> None:
    schema = json.loads(
        (REPOSITORY_ROOT / "Tools/DurinDevTool/schemas/asset-audit-v3.schema.json").read_text(
            encoding="utf-8"
        )
    )
    package_properties = schema["$defs"]["package"]["properties"]
    finding_properties = schema["$defs"]["finding"]["properties"]
    assert schema["properties"]["schemaVersion"]["const"] == asset.SCHEMA_VERSION
    native_contract = (
        REPOSITORY_ROOT
        / "Engine/Source/Developer/AssetMaintenance/Public/AssetMaintenance/CompatibilityAudit.h"
    ).read_text(encoding="utf-8")
    assert f"AssetCompatibilityReportSchemaVersion = {asset.SCHEMA_VERSION};" in native_contract
    assert set(package_properties["inspection"]["enum"]) == {"NotChecked", "Ready", "Failed"}
    assert set(package_properties["compatibility"]["enum"]) == {"Compatible", "Incompatible", "Unsupported"}
    assert set(package_properties["freshness"]["enum"]) == {"Current", "Stale"}
    assert set(finding_properties["code"]["enum"]) == {
        "UnknownField", "IncompatibleFieldSignature", "DeprecatedRouteUsed", "UnavailableClass",
        "UnsupportedPackageFormat", "InvalidObjectGraph", "CorruptPackage", "IoFailure",
    }
    assert set(schema["$defs"]["canonicalizationEvidence"]["properties"]["kind"]["enum"]) == {
        "Class", "Struct", "Enum", "Property",
    }
    assert schema["$defs"]["deprecatedRouteEvidence"]["properties"]["sourceVersion"]["minimum"] == -1


def test_checked_in_report_fixtures_match_their_schemas() -> None:
    audit_schema = json.loads(
        (REPOSITORY_ROOT / "Tools/DurinDevTool/schemas/asset-audit-v3.schema.json").read_text(
            encoding="utf-8"
        )
    )
    validate(json.loads((FIXTURE_ROOT / "asset-audit-v3.json").read_text(encoding="utf-8")), audit_schema)


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


def test_human_output_exposes_resave_evidence(tmp_path: Path) -> None:
    value = with_canonicalization_evidence(package("/Game/Legacy"))
    with_deprecated_route_evidence(value)
    result, output, _ = run_handler(
        tmp_path,
        report(value),
        format_name="human",
    )
    assert result == 0
    assert "1 resave recommended" in output
    assert "Resave recommended (1):" in output
    assert "Durin::OldAsset -> Durin::CurrentAsset" in output
    assert "Durin::DExample.OldValue -> CurrentValue" in output


def test_rejects_unstable_order_and_unknown_schema_names(tmp_path: Path) -> None:
    with pytest.raises(DevToolError, match="order"):
        run_handler(tmp_path, report(package("/Game/B"), package("/Game/A")))
    with pytest.raises(DevToolError, match="NewCode"):
        run_handler(tmp_path, report(package("/Game/A", code="NewCode")))


def test_cancel_and_process_failure_are_distinct(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.touch()
    namespace = type("Namespace", (), {
        "asset_command": "check", "project_path": project,
        "format_name": "json", "baseline": False,
    })()
    errors = io.StringIO()

    def cancelled(*_args: object, **_kwargs: object) -> str:
        raise BuildToolError("cancelled", exit_code=130)

    result = asset.run(
        namespace, repository_root=tmp_path, repository_context=REPOSITORY,
        stdout=io.StringIO(), stderr=errors,
        executable_resolver=lambda _namespace, _root: executable,
        command_runner=cancelled,
    )
    assert result == 130
    assert "cancelled" in errors.getvalue()

    def failed(*_args: object, **_kwargs: object) -> str:
        raise BuildToolError("scan failed", exit_code=1)

    with pytest.raises(DevToolError, match="scan failed"):
        asset.run(
            namespace, repository_root=tmp_path, repository_context=REPOSITORY,
            stdout=io.StringIO(), stderr=io.StringIO(),
            executable_resolver=lambda _namespace, _root: executable,
            command_runner=failed,
        )


def test_check_invocation_is_read_only_and_missing_project_fails_before_launch(tmp_path: Path) -> None:
    executable = tmp_path / "DurinAssetTool.exe"
    executable.touch()
    project = tmp_path / "Test.dproject"
    project.write_text("{}", encoding="utf-8")
    authored = tmp_path / "Content" / "Test.dasset"
    authored.parent.mkdir()
    authored.write_bytes(b"DAST authored bytes")
    namespace = type(
        "Namespace", (),
        {"asset_command": "check", "project_path": project, "format_name": "json", "baseline": False},
    )()
    before = {path: path.read_bytes() for path in tmp_path.rglob("*") if path.is_file()}
    calls: list[tuple[list[str], Path]] = []

    def command_runner(arguments: list[str], **kwargs: object) -> str:
        calls.append((arguments, kwargs["cwd"]))
        return report()

    assert asset.run(
        namespace,
        repository_root=tmp_path,
        repository_context=REPOSITORY,
        stdout=io.StringIO(),
        stderr=io.StringIO(),
        executable_resolver=lambda *_args: executable,
        command_runner=command_runner,
    ) == 0
    after = {path: path.read_bytes() for path in tmp_path.rglob("*") if path.is_file()}
    assert before == after
    assert calls == [
        ([str(executable), "check", f"--project={project}", "--json"], REPOSITORY_ROOT)
    ]

    project.unlink()
    with pytest.raises(DevToolError, match="Project descriptor was not found"):
        asset.run(
            namespace,
            repository_root=tmp_path,
            repository_context=REPOSITORY,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
            executable_resolver=lambda *_args: executable,
            command_runner=lambda *_args, **_kwargs: pytest.fail("missing project must not launch"),
        )
