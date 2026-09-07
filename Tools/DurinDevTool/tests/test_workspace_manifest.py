import io
import json
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest

from durin_dev_tool.build import operations, scaffolding
from durin_dev_tool.build.errors import BuildToolError
from durin_dev_tool.build.native_test_impact import analyze_affected_tests
from durin_dev_tool.build.native_test_registry import NativeTestProject, NativeTestRegistry
from durin_dev_tool.build.output import BuildOutput
from durin_dev_tool.build.requests import NativeTestRequest
from durin_dev_tool.build.workspace_manifest import load_workspace_manifest, test_graph_fingerprint as fingerprint, write_cmake
from durin_dev_tool.json_contract import JsonContractError
from .test_native_test_impact import target
from .test_build_scaffolding import TestModuleScaffolding as _ModuleScaffolding
from .build_request_fixtures import parse_build_request


def workspace(root):
    _ModuleScaffolding.create_workspace(root)
    for name in ("Engine", "Sandbox"):
        (root / name / "CMakeLists.txt").write_text(f"add_durin_project({name})\n")
        path = root / name / f"{name}.dproject"
        data = json.loads(path.read_text())
        data["Tests"] = {"Native": {"Root": "Tests/Native"}}
        path.write_text(json.dumps(data))
    (root / "Durin.dworkspace").write_text(json.dumps({"Projects": ["Engine/Engine.dproject", "Sandbox/Sandbox.dproject"]}))
    (root / "CMakeLists.txt").write_text("add_durin_workspace()\n")
    return load_workspace_manifest(root)


def test_manifest_is_authoritative_and_preserves_empty_test_project(tmp_path):
    entries = workspace(tmp_path)
    ignored = tmp_path / "Unregistered"
    ignored.mkdir()
    (ignored / "Bad.dproject").write_text("invalid")
    assert [p.name for p in load_workspace_manifest(tmp_path)] == ["Engine", "Sandbox"]
    assert entries[1].test_root == tmp_path / "Sandbox/Tests/Native"
    write_cmake(tmp_path, tmp_path / "generated.cmake")
    generated = (tmp_path / "generated.cmake").read_text()
    assert '"name": "Sandbox"' in generated
    assert "DURIN_WORKSPACE_PROJECT_FILES" in generated


@pytest.mark.parametrize("value", ["../outside", ".", "/absolute", "Tests;Other"])
def test_test_root_rejects_unbounded_paths(tmp_path, value):
    workspace(tmp_path)
    path = tmp_path / "Sandbox/Sandbox.dproject"
    data = json.loads(path.read_text())
    data["Tests"]["Native"]["Root"] = value
    path.write_text(json.dumps(data))
    with pytest.raises(BuildToolError, match="Tests.Native.Root"):
        load_workspace_manifest(tmp_path)


@pytest.mark.parametrize("projects", [
    ["Engine/Engine.dproject", "Engine/Engine.dproject"],
    ["Sandbox/Sandbox.dproject", "Engine/Engine.dproject"],
    ["Engine/Engine.dproject", "../Elsewhere/Other.dproject"],
])
def test_manifest_rejects_invalid_membership(tmp_path, projects):
    workspace(tmp_path)
    (tmp_path / "Durin.dworkspace").write_text(json.dumps({"Projects": projects}))
    with pytest.raises((BuildToolError, JsonContractError)):
        load_workspace_manifest(tmp_path)


def test_fingerprint_detects_graph_addition_deletion_and_cmake_but_not_assertion_edit(tmp_path):
    entries = workspace(tmp_path)
    original = fingerprint(tmp_path, entries)
    tests = entries[1].test_root
    tests.mkdir(parents=True)
    source = tests / "NewTests.cpp"
    source.write_text("old assertion")
    added = fingerprint(tmp_path, entries)
    assert added != original
    source.write_text("new assertion")
    assert fingerprint(tmp_path, entries) == added
    cmake = tests / "CMakeLists.txt"
    cmake.write_text("add_durin_test(NewTests NewTests.cpp)")
    assert fingerprint(tmp_path, entries) != added
    cmake.unlink()
    source.unlink()
    assert fingerprint(tmp_path, entries) == original


def test_project_creation_updates_manifest_and_preserves_root_cmake(tmp_path):
    workspace(tmp_path)
    original = (tmp_path / "CMakeLists.txt").read_bytes()
    request = parse_build_request(["create", "project", "MyGame", "--path", "MyGame"])
    plan = scaffolding.plan_project_creation(request, tmp_path)
    scaffolding.execute_plan(plan)
    assert (tmp_path / "CMakeLists.txt").read_bytes() == original
    entries = load_workspace_manifest(tmp_path)
    assert entries[-1].name == "MyGame"
    assert entries[-1].test_root == tmp_path / "MyGame/Tests/Native"


def test_failed_project_creation_restores_manifest(tmp_path):
    workspace(tmp_path)
    original = (tmp_path / "Durin.dworkspace").read_bytes()
    request = parse_build_request(["create", "project", "MyGame", "--path", "MyGame"])
    plan = scaffolding.plan_project_creation(request, tmp_path)

    def fail(phase, index, path):
        if phase == "after-replace" and path.name == "Durin.dworkspace":
            raise RuntimeError("injected manifest failure")

    with pytest.raises(RuntimeError, match="injected manifest"):
        scaffolding.execute_plan(plan, failure_injector=fail)
    assert (tmp_path / "Durin.dworkspace").read_bytes() == original
    assert not (tmp_path / "MyGame").exists()


def owned_registry(tmp_path):
    return NativeTestRegistry(tmp_path / "registry.json", "debug", (
        replace(target("EngineTests"), project="Engine"),
        replace(target("RoadTests"), project="RoadWeaver"),
        replace(target("RoadSceneTests", kind="integration"), project="RoadWeaver"),
        replace(target("RoadQualificationTests", kind="qualification"), project="RoadWeaver"),
    ), (NativeTestProject("RoadWeaver", "RoadWeaver/RoadWeaver.dproject", "RoadWeaver/Tests/Native"),))


@pytest.mark.parametrize("path", ["CMakeLists.txt", "CMake/Targets.cmake", "Private/Added.cpp", "Private/Deleted.cpp"])
def test_project_test_changes_select_only_owner(tmp_path, path):
    result = analyze_affected_tests(owned_registry(tmp_path), ("RoadWeaver/Tests/Native/" + path,))
    assert not result.run_all
    assert result.names == ("RoadTests", "RoadSceneTests")


def test_named_case_file_is_bounded_and_prefix_collision_is_not_owned(tmp_path):
    registry = owned_registry(tmp_path)
    assert analyze_affected_tests(registry, ("RoadWeaver/Tests/Native/Private/RoadTests.cpp",)).names == ("RoadTests",)
    assert analyze_affected_tests(registry, ("RoadWeaver/Tests/NativeExtra/CMakeLists.txt",)).run_all
    assert analyze_affected_tests(registry, ("CMake/Project/ProjectTargets.cmake",)).run_all


@pytest.mark.parametrize("explain", [True, False])
def test_stale_execution_refreshes_before_selection_and_explain_is_read_only(tmp_path, monkeypatch, explain):
    workspace(tmp_path)
    old = owned_registry(tmp_path)
    old.path.touch()
    fresh = replace(old, test_graph_fingerprint=fingerprint(tmp_path, load_workspace_manifest(tmp_path)))
    context = SimpleNamespace(request=NativeTestRequest(test_operation="affected", test_explain_affected=explain))
    initial_request = context.request
    calls = []
    monkeypatch.setattr("durin_dev_tool.build.operations.registry_path", lambda _: old.path)
    monkeypatch.setattr(operations, "load_native_test_registry", lambda _: fresh if calls else old)
    monkeypatch.setattr(operations, "execute_context", lambda *args, **kwargs: calls.append("configure"))
    stream = io.StringIO()
    result = operations.load_affected_registry(context, BuildOutput(plain=True, stdout=stream), tmp_path)
    assert context.request is initial_request
    assert calls == ([] if explain else ["configure"])
    assert result == (old if explain else fresh)
    if explain:
        assert "stale" in stream.getvalue()


def test_failed_refresh_restores_original_request(tmp_path, monkeypatch):
    workspace(tmp_path)
    old = owned_registry(tmp_path)
    old.path.touch()
    context = SimpleNamespace(request=NativeTestRequest(test_operation="affected"))
    request = context.request
    monkeypatch.setattr("durin_dev_tool.build.operations.registry_path", lambda _: old.path)
    monkeypatch.setattr(operations, "load_native_test_registry", lambda _: old)

    def fail(*args, **kwargs):
        raise BuildToolError("configure failed")

    monkeypatch.setattr(operations, "execute_context", fail)
    with pytest.raises(BuildToolError, match="configure failed"):
        operations.load_affected_registry(context, BuildOutput(plain=True, stdout=io.StringIO()), tmp_path)
    assert context.request is request
