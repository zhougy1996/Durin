
import io
import json
import subprocess
from pathlib import Path
from unittest import mock

import pytest
from jsonschema import validate

DEV_TOOL_ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = DEV_TOOL_ROOT.parents[1]
from durin_dev_tool import cli
from durin_dev_tool import asset
from durin_dev_tool import storage_qualification
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry


FIXTURE_ROOT = Path(__file__).resolve().parent / "fixtures"


def package(
    path: str,
    *,
    inspection: str = "Ready",
    compatibility: str = "Compatible",
    freshness: str = "Current",
    format_version: int = 6,
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
        stdout=output,
        stderr=errors,
        executable_resolver=lambda _namespace, _root: executable,
        process_runner=lambda *_args, **_kwargs: subprocess.CompletedProcess([], 0, report_text, ""),
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
    with mock.patch.object(asset, "invoke_runtime_program", return_value=report()) as invoke:
        assert asset.run(
            namespace,
            repository_root=tmp_path,
            stdout=io.StringIO(),
            stderr=io.StringIO(),
            executable_resolver=lambda *_args: executable,
        ) == 0
    assert invoke.call_args.args[2] == ["check", f"--project={project.resolve()}", "--json"]


def test_registry_defaults_to_check_and_rejects_removed_commands() -> None:
    registry = CommandRegistry()
    _, namespace = registry.parse(["asset"])
    assert namespace.asset_command == "check"
    assert namespace.project_path is None
    _, option_namespace = registry.parse(["asset", "--json"])
    assert option_namespace.asset_command == "check"
    assert option_namespace.format_name == "json"
    with pytest.raises(DevToolError, match="invalid choice"):
        registry.parse(["asset", "audit"])


def test_selected_asset_command_grammar_is_frozen() -> None:
    registry = CommandRegistry()
    _, check_namespace = registry.parse(
        ["asset", "check", "--project", "Sandbox/Sandbox.dproject", "--baseline", "--json"]
    )
    assert check_namespace.asset_command == "check"
    assert check_namespace.baseline
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

    _, storage_namespace = registry.parse(
        ["asset", "storage", "--project", "Sandbox/Sandbox.dproject"]
    )
    assert storage_namespace.output_path == Path(
        "Saved/AuthoredPackageStorageQualification/latest"
    )


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
    calls: list[list[str]] = []

    def process_runner(arguments: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(arguments)
        return subprocess.CompletedProcess(arguments, 0, report(), "")

    assert asset.run(
        namespace,
        repository_root=tmp_path,
        stdout=io.StringIO(),
        stderr=io.StringIO(),
        executable_resolver=lambda *_args: executable,
        process_runner=process_runner,
    ) == 0
    assert calls == [[str(executable), "check", f"--project={project}", "--json"]]


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

    def process_runner(arguments: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(arguments)
        return subprocess.CompletedProcess(arguments, 0, '{"status":"Succeeded"}\n', "")

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
                stdout=io.StringIO(),
                stderr=io.StringIO(),
                executable_resolver=lambda *_args: executable,
                process_runner=lambda *_args, **_kwargs: pytest.fail("invalid selection must not launch"),
            )


def test_storage_qualification_protocol_and_decision_match_current_v6_baseline() -> None:
    protocol = json.loads(storage_qualification.PROTOCOL_PATH.read_text(encoding="utf-8"))
    assert protocol["schemaVersion"] == 3
    assert {workload["kind"] for workload in protocol["workloads"]} == {
        "tracked", "synthetic", "future-consumer"
    }
    assert {candidate["id"] for candidate in protocol["candidates"]} == {
        "retain-dast6-dabk1", "next-dast-companion-index",
        "content-addressed-companion", "package-local-payload",
        "in-place-tail-or-append",
    }
    corpus = {
        "reachableExternalBytes": 2_359_616,
        "payloadCount": 2,
        "warmInspectionP95Milliseconds": 10.0,
    }
    git_baseline = {
        "files": [{
            "path": "Example.dabulk", "workingTreeBytes": 2_359_616,
            "history": {"monthlyTouchRate": 1.0},
        }]
    }
    publication = {"samples": [{"warmP95Milliseconds": 10.0}]}
    decision = storage_qualification._decision(
        protocol, corpus, git_baseline, publication
    )
    assert decision["result"] == "Retain"
    assert not any(decision["pressureGates"].values())

    corpus["warmInspectionP95Milliseconds"] = 75.0
    diagnostic = storage_qualification._decision(
        protocol, corpus, git_baseline, publication
    )
    assert diagnostic["result"] == "Retain"
    assert diagnostic["performanceDiagnosticOnly"]
    assert not diagnostic["pressureGates"]["inspectionP95"]

    broken_corpus = {
        **corpus,
        "corruptPackageCount": 1,
        "missingExternalPayloadCount": 1,
    }
    rejected = storage_qualification._decision(
        protocol, broken_corpus, git_baseline, publication
    )
    assert rejected["result"] == "Defer"
    assert rejected["integrityGates"]["corruptPackages"]
    assert rejected["integrityGates"]["missingExternalPayloads"]
    assert "Mandatory corpus or durability gates failed" in rejected["rationale"]


def test_storage_qualification_inventory_summary_distinguishes_hash_candidates_from_exact_duplicates() -> None:
    descriptor = {
        "payloadId": "A", "logicalBytes": 4, "storedBytes": 4,
        "contentHash": "HASH", "storage": "External", "reachable": True,
        "exactDuplicateGroup": 1,
    }
    inventory = {
        "packages": [
            {
                "inspection": "Ready", "fileBytesRead": 10,
                "inspectionNanoseconds": [2_000_000, 1_000_000],
                "descriptors": [descriptor],
            },
            {
                "inspection": "Ready", "fileBytesRead": 20,
                "inspectionNanoseconds": [2_000_000, 1_500_000],
                "descriptors": [{**descriptor, "payloadId": "B"}],
            },
        ]
    }
    summary = storage_qualification._summarize_inventory(inventory)
    assert summary["duplicatePayloadIds"] == []
    assert summary["duplicateContentCandidateHashes"] == ["HASH"]
    assert summary["exactDuplicateExternalPayloadCount"] == 2
    assert summary["inspectionBytesRead"] == 30
    assert summary["duplicatePayloadIdsWithinPackage"] == []
    assert summary["orphanCompanionCount"] == 0


def test_storage_qualification_failure_model_rejects_unproven_in_place_protocols() -> None:
    results = {
        item["candidate"]: item["result"]
        for item in storage_qualification._failure_model()
    }
    assert results["complete-file-atomic-replacement"] == "Pass"
    assert results["companion-first-publication"] == "Pass"
    assert results["in-place-tail-rewrite"] == "Fail"
    assert results["append-generation"] == "Fail"


def test_storage_history_is_head_bounded_and_follows_renames(tmp_path: Path) -> None:
    def git(*arguments: str) -> None:
        subprocess.run(
            ["git", *arguments], cwd=tmp_path, check=True, capture_output=True, text=True
        )

    git("init", "--quiet")
    git("config", "user.email", "qualification@durin.invalid")
    git("config", "user.name", "Durin Qualification")
    old_path = tmp_path / "Old.dasset"
    old_path.write_bytes(b"asset bytes")
    git("add", "Old.dasset")
    git("commit", "--quiet", "-m", "baseline")
    git("mv", "Old.dasset", "Renamed.dasset")
    git("commit", "--quiet", "-m", "rename")

    history = storage_qualification._history_for_path(tmp_path, "Renamed.dasset")
    assert history["commitTouches"] == 2
    assert history["uniqueMainGitBlobs"] == 1

@pytest.mark.parametrize(
    ("native_report", "expected"),
    [
        (report(package("/Game/Baseline")), 0),
        (report(package("/Game/Baseline", format_version=2, compatibility="Unsupported", code="UnsupportedPackageFormat")), 3),
        (report(package("/Game/Baseline", format_version=3)), 3),
        (report(package("/Game/Baseline", format_version=4, compatibility="Unsupported", code="UnsupportedPackageFormat")), 3),
        (report(package("/Game/Baseline", format_version=5, compatibility="Unsupported", code="UnsupportedPackageFormat")), 3),
        (report(package("/Game/Baseline", compatibility="Incompatible", code="UnknownField")), 3),
        (report(with_canonicalization_evidence(package("/Game/Baseline"))), 3),
        (report(with_deprecated_route_evidence(package("/Game/Baseline"))), 3),
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
        "asset_command": "check",
        "project_path": project,
        "format_name": "human",
        "baseline": True,
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
    assert calls == [[str(executable), "check", f"--project={project}", "--json"]]
    assert ("Asset baseline:" in output.getvalue()) == (expected == 0)


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
        REPOSITORY_ROOT / "Engine/Source/Runtime/AssetCore/Public/Asset/Compatibility.h"
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
        ([str(executable), "check", f"--project={project}", "--json"], tmp_path)
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
