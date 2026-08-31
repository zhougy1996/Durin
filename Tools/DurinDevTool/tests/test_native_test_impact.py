from pathlib import Path

from durin_dev_tool.build.native_test_impact import (
    analyze_affected_tests,
    discover_changed_paths,
)
from durin_dev_tool.build.native_test_registry import NativeTestRegistry, NativeTestTarget


def target(
    name: str,
    *,
    kind: str = "feature",
    domains: tuple[str, ...] = (),
    modules: tuple[str, ...] = (),
) -> NativeTestTarget:
    return NativeTestTarget(
        name=name,
        kind=kind,
        domains=domains,
        modules=modules,
        backends=(),
        stacks=(),
        execution_host="direct",
        resolved_execution_host="direct",
        resource_locks=(),
        heavy_runtime=False,
        private_source_owner="",
        private_source_rationale="",
    )


def registry(tmp_path: Path) -> NativeTestRegistry:
    return NativeTestRegistry(
        tmp_path / "registry.json",
        "debug",
        (
            target("CoreTests", kind="contract", domains=("core",), modules=("core",)),
            target(
                "RenderContractTests",
                kind="contract",
                domains=("renderer",),
                modules=("rendercore",),
            ),
            target(
                "RendererIntegrationTests",
                kind="integration",
                domains=("renderer",),
                modules=("renderer", "rendercore"),
            ),
            target(
                "RendererQualificationTests",
                kind="qualification",
                domains=("renderer",),
                modules=("renderer",),
            ),
        ),
    )


def test_source_module_changes_select_all_ordinary_consumers(tmp_path: Path) -> None:
    affected = analyze_affected_tests(
        registry(tmp_path),
        ("Engine/Source/Runtime/RenderCore/Private/Shader.cpp",),
    )

    assert affected.modules == ("rendercore",)
    assert affected.names == ("RenderContractTests", "RendererIntegrationTests")
    assert not affected.run_all


def test_native_test_path_selects_its_registered_domain(tmp_path: Path) -> None:
    affected = analyze_affected_tests(
        registry(tmp_path),
        ("Engine/Tests/Native/EngineTests/Private/Renderer/SceneTests.cpp",),
    )

    assert affected.domains == ("renderer",)
    assert affected.names == ("RenderContractTests", "RendererIntegrationTests")


def test_native_test_filename_selects_its_exact_registered_target(tmp_path: Path) -> None:
    affected = analyze_affected_tests(
        registry(tmp_path),
        ("Engine/Tests/Native/Misc/Private/CoreTests.cpp",),
    )

    assert affected.names == ("CoreTests",)
    assert "changed native-test targets: CoreTests" in affected.reasons


def test_documentation_only_change_requires_no_native_tests(tmp_path: Path) -> None:
    affected = analyze_affected_tests(
        registry(tmp_path),
        ("Documentation/Agents/Testing.md", "AGENTS.md"),
    )

    assert not affected.run_all
    assert affected.names == ()


def test_shared_test_infrastructure_change_escalates_to_all(tmp_path: Path) -> None:
    affected = analyze_affected_tests(
        registry(tmp_path),
        ("Tools/DurinDevTool/durin_dev_tool/build/runtime.py",),
    )

    assert affected.run_all
    assert affected.names == ()
    assert "shared native-test" in affected.reasons[0]


def test_changed_path_discovery_combines_worktree_sets(monkeypatch, tmp_path: Path) -> None:
    outputs = iter(("tracked.cpp\0", "staged.cpp\0", "untracked.cpp\0"))
    monkeypatch.setattr(
        "durin_dev_tool.build.native_test_impact._run_git",
        lambda *_args, **_kwargs: next(outputs),
    )

    assert discover_changed_paths(tmp_path) == (
        "staged.cpp",
        "tracked.cpp",
        "untracked.cpp",
    )


def test_changed_path_discovery_uses_requested_base(monkeypatch, tmp_path: Path) -> None:
    calls: list[list[str]] = []

    def run_git(_root: Path, arguments: list[str], _operation: str) -> str:
        calls.append(arguments)
        return "base.cpp\0" if arguments[0] == "diff" else ""

    monkeypatch.setattr("durin_dev_tool.build.native_test_impact._run_git", run_git)

    assert discover_changed_paths(tmp_path, "origin/main") == ("base.cpp",)
    assert calls[0][-2:] == ["origin/main", "--"]
    assert len(calls) == 2
