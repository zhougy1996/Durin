
import json
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import pytest

REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
from durin_dev_tool.build.errors import BuildToolError
from durin_dev_tool.build.native_test_registry import (
    filter_targets,
    is_test_set_selection,
    NativeTestRegistry,
    NativeTestTarget,
    load_native_test_registry,
    resolve_selection,
)


def target(
    name: str,
    *,
    kind: str = "feature",
    domains: tuple[str, ...] = ("viewport",),
    modules: tuple[str, ...] = ("engine",),
    backends: tuple[str, ...] = (),
) -> NativeTestTarget:
    return NativeTestTarget(
        name=name,
        kind=kind,
        domains=domains,
        modules=modules,
        backends=backends,
        stacks=(),
        execution_host="direct",
        resolved_execution_host="direct",
        resource_locks=(),
        heavy_runtime=False,
        private_source_owner="",
        private_source_rationale="",
    )


@pytest.fixture
def registry(tmp_path: Path) -> NativeTestRegistry:
    return NativeTestRegistry(
        tmp_path / "registry.json",
        "debug",
        (
            target("EngineViewportTests"),
            target("MonaViewportTests", modules=("mona",)),
            target(
                "VulkanViewportTests",
                kind="integration",
                backends=("vulkan",),
            ),
            target("LaunchCrashTests", kind="characterization", domains=("launch",)),
            target("RendererQualificationTests", kind="qualification", domains=("renderer",)),
        ),
    )


def test_selector_unions_within_dimension_and_intersects_dimensions(
    registry: NativeTestRegistry,
) -> None:
    resolved = resolve_selection(
        registry,
        "@kind=feature+integration,domain=viewport,backend=vulkan",
    )
    assert resolved.names == ("VulkanViewportTests",)
    assert "kind=feature + integration" in resolved.explanation


def test_domain_shorthand_and_exact_target_precedence(registry: NativeTestRegistry) -> None:
    assert resolve_selection(registry, "@viewport").names == (
        "EngineViewportTests",
        "MonaViewportTests",
        "VulkanViewportTests",
    )
    assert resolve_selection(registry, "MonaViewportTests").explanation == "exact target name"


def test_fast_all_profile_excludes_integration_and_explicit_admission_kinds(
    registry: NativeTestRegistry,
) -> None:
    resolved = resolve_selection(registry, "fast-all")
    assert resolved.names == ("EngineViewportTests", "MonaViewportTests")
    assert "integration excluded" in resolved.explanation
    assert is_test_set_selection("fast-all")
    assert is_test_set_selection("@viewport")
    assert not is_test_set_selection("EngineViewportTests")


def test_empty_and_characterization_selections_are_explicit(
    registry: NativeTestRegistry,
) -> None:
    with pytest.raises(BuildToolError, match="matched no configured targets"):
        resolve_selection(registry, "@domain=missing")
    with pytest.raises(BuildToolError, match="characterization-only"):
        resolve_selection(registry, "LaunchCrashTests")
    assert resolve_selection(
        registry,
        "@kind=characterization",
        admit_characterization=True,
    ).names == ("LaunchCrashTests",)


def test_qualification_selections_are_explicit(
    registry: NativeTestRegistry,
) -> None:
    with pytest.raises(BuildToolError, match="qualification-only"):
        resolve_selection(registry, "RendererQualificationTests")
    assert resolve_selection(
        registry,
        "@kind=qualification",
        admit_qualification=True,
    ).names == ("RendererQualificationTests",)


def test_private_source_report_selects_owned_seams(
    tmp_path: Path,
) -> None:
    private_source = replace(
        target("VulkanTests", kind="integration"),
        private_source_owner="RenderCore",
        private_source_rationale="Owned shader compiler seam.",
    )
    private_source_registry = NativeTestRegistry(
        tmp_path / "registry.json",
        "debug",
        (target("StructuredTests"), private_source),
    )
    assert tuple(
        item.name for item in filter_targets(private_source_registry, "private-sources")
    ) == (
        "VulkanTests",
    )
    completed_registry = replace(
        private_source_registry,
        targets=(target("StructuredTests"),),
    )
    assert filter_targets(completed_registry, "private-sources") == ()


def test_registry_loader_rejects_wrong_preset_identity(tmp_path: Path) -> None:
    document = {
        "schemaVersion": 4,
        "identity": {
            "sourceDir": str(REPOSITORY_ROOT),
            "binaryDir": str(tmp_path),
            "preset": "release",
            "configuration": "Debug",
        },
        "targets": [],
    }
    (tmp_path / "DurinNativeTestRegistry.json").write_text(
        json.dumps(document), encoding="utf-8"
    )
    context = SimpleNamespace(preset=SimpleNamespace(name="debug"))
    with mock.patch(
        "durin_dev_tool.build.native_test_registry.preset_build_directory",
        return_value=tmp_path,
    ):
        with pytest.raises(BuildToolError, match="does not match"):
            load_native_test_registry(context)  # type: ignore[arg-type]
