import json
import sys
from pathlib import Path

import pytest
from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "Engine/Source/Programs/DurinHeaderTool"))
sys.path.insert(0, str(ROOT / "Tools/DurinDevTool"))

from durin_header_tool.config.plugin_config import canonical, resolve_plugins
from durin_header_tool.config.project_config import DurinProjectConfig
from durin_dev_tool.build.descriptors import load_project_descriptor
from durin_dev_tool.build.plugins import resolve_plugins as resolve_devtool

CASES = json.loads((ROOT / "Engine/Tests/Native/PluginResolutionTests/Data/Cases.json").read_text(encoding="utf-8"))["Cases"]
SCHEMAS = ROOT / "Engine/Source/Programs/DurinHeaderTool/schemas"


@pytest.mark.parametrize("case", CASES, ids=lambda case: case["Name"])
def test_shared_native_and_python_contract(case, tmp_path):
    for relative, text in case["Files"].items():
        path = tmp_path / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
    args = (tmp_path / "Engine", tmp_path / "Game/Game.dproject")
    kwargs = dict(runtime_variant=case["Variant"], platform=case.get("Platform", "Win64"))
    if "Error" in case:
        with pytest.raises(ValueError) as caught:
            resolve_plugins(*args, **kwargs)
        assert case["Error"] in str(caught.value)
        assert str(tmp_path) in str(caught.value)
        if "Chain" in case:
            assert case["Chain"] in str(caught.value)
    else:
        manifest, locations = resolve_plugins(*args, **kwargs)
        assert canonical(manifest) == case["Manifest"]
        assert str(tmp_path) not in canonical(manifest)
        assert all(path.is_absolute() for path in locations.values())
        assert resolve_devtool(*args, **kwargs)[0] == manifest
        schema = json.loads((SCHEMAS / "durin-plugin-selection.schema.json").read_text())
        Draft202012Validator(schema).validate(manifest)


@pytest.mark.parametrize("project,roots,editor", [
    ("Engine", ["DurinLauncher", "Renderer", "VulkanRHI", "MetalRHI"],
     ["AssetTools", "DurinEd", "MainFrame", "ContentBrowser", "LevelEditor", "MaterialEditor",
      "TextureEditor", "StaticMeshEditor", "AssetForgeBuiltins", "MeshBuilder", "DerivedDataCache",
      "TextureCompressor", "AssetMaintenance", "ShaderBuild", "TargetPlatform", "DurinMCP"]),
    ("Sandbox", ["Sandbox"], []),
    ("RoadWeaver", ["RoadWeaver"], ["RoadWeaverEditor"]),
])
def test_existing_project_roots_are_preserved(project, roots, editor):
    path = ROOT / project / (project + ".dproject")
    devtool = load_project_descriptor(path, schema_directory=SCHEMAS)
    dht = DurinProjectConfig.from_file(path)
    assert list(devtool.base_modules) == roots
    assert dht.get_enabled_root_modules("DurinGame") == roots
    assert dht.get_enabled_root_modules("DurinEditor") == roots + editor
    assert devtool.plugin_overrides == dht.plugin_overrides == {}
    for variant in ("DurinEditor", "DurinGame"):
        result, _ = resolve_plugins(ROOT / "Engine", path, runtime_variant=variant,
                                    project_files=[ROOT / name / (name + ".dproject")
                                                   for name in ("Engine", "Sandbox", "RoadWeaver")])
        assert result["Plugins"] == result["Modules"] == []


@pytest.mark.parametrize("value", [{}, {"Water": True, "Tools": False}])
def test_project_readers_preserve_overrides_without_activating_modules(tmp_path, value):
    path = tmp_path / "Project.dproject"
    path.write_text(json.dumps(dict(ProjectName="Project", Plugins=value)))
    assert load_project_descriptor(path, schema_directory=SCHEMAS).plugin_overrides == value
    assert DurinProjectConfig.from_file(path).plugin_overrides == value


@pytest.mark.parametrize("name", ["durin-plugin.schema.json", "durin-plugin-selection.schema.json", "durin-plugin-build.schema.json"])
def test_plugin_schemas(name):
    Draft202012Validator.check_schema(json.loads((SCHEMAS / name).read_text()))


def test_build_manifest_requires_complete_native_identity():
    validator = Draft202012Validator(json.loads((SCHEMAS / "durin-plugin-build.schema.json").read_text()))
    inventory = dict(SchemaVersion=1, SelectionFingerprint="0" * 16, Binaries=[],
                     Identity=dict(EngineBuild="revision", Platform="Win64", Architecture="x64",
                                   Configuration="Debug", RuntimeVariant="DurinGame", ToolchainABI="msvc-profile"))
    validator.validate(inventory)
    for field in list(inventory["Identity"]):
        invalid = json.loads(json.dumps(inventory))
        del invalid["Identity"][field]
        assert list(validator.iter_errors(invalid))
    inventory["Binaries"] = [dict(Module="Water", Owner="Water", Path="Runtime/Water.dll", RuntimeDependencies=[])]
    validator.validate(inventory)
    for path in ("../Escape.dll", "D:/Escape.dll", "/Escape.dll", "Runtime/../Escape.dll", "Runtime\\Escape.dll"):
        inventory["Binaries"][0]["Path"] = path
        assert list(validator.iter_errors(inventory))


def test_input_order_and_relocation_do_not_change_selection(tmp_path):
    case = next(case for case in CASES if case["Name"] == "DependencyEditor")
    results = []
    for folder, files in (("First", case["Files"].items()), ("Second", reversed(list(case["Files"].items())))):
        root = tmp_path / folder
        for relative, text in files:
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode("utf-8"))
        results.append(resolve_plugins(root / "Engine", root / "Game/Game.dproject")[0])
    assert results[0] == results[1]
    descriptor = tmp_path / "Second/Game/Plugins/Water/Water.dplugin"
    descriptor.write_bytes(descriptor.read_bytes() + b"\n")
    assert resolve_plugins(tmp_path / "Second/Engine", tmp_path / "Second/Game/Game.dproject")[0]["Fingerprint"] != results[0]["Fingerprint"]


@pytest.mark.parametrize("variant", ["DurinEditor", "DurinGame"])
def test_runtime_plugin_can_consume_real_engine_module_graph(tmp_path, variant):
    projects = []
    for name in ("Engine", "Sandbox", "RoadWeaver"):
        relative = Path(name) / (name + ".dproject")
        data = json.loads((ROOT / relative).read_text())
        if name == "Sandbox":
            data["Plugins"] = {"Probe": True}
        target = tmp_path / relative
        target.parent.mkdir(parents=True)
        target.write_text(json.dumps(data))
        projects.append(target)
        for module, directory in data["ModuleDirs"].items():
            relative_module = Path(name) / directory / (module + ".dmodule")
            dest = tmp_path / relative_module
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes((ROOT / relative_module).read_bytes())
    plugin_root = tmp_path / "Sandbox/Plugins/Probe"
    plugin_root.mkdir(parents=True)
    (plugin_root / "Probe.dplugin").write_text(json.dumps(dict(
        SchemaVersion=1, Id="Probe", Modules=[dict(Name="Probe", Path=".")])))
    (plugin_root / "Probe.dmodule").write_text(json.dumps(dict(ModuleName="Probe", PublicDependencies=["Engine"])))
    manifest, _ = resolve_plugins(tmp_path / "Engine", tmp_path / "Sandbox/Sandbox.dproject",
                                  runtime_variant=variant, project_files=projects)
    assert [module["Name"] for module in manifest["Modules"]] == ["Probe"]


def test_discovery_rejects_symbolic_root(tmp_path):
    engine = tmp_path / "Engine"
    engine.mkdir()
    project = engine / "Engine.dproject"
    project.write_text('{"ProjectName":"Engine"}')
    target = tmp_path / "Outside"
    target.mkdir()
    try:
        (engine / "Plugins").symlink_to(target, target_is_directory=True)
    except OSError:
        pytest.skip("symbolic link creation unavailable")
    with pytest.raises(ValueError, match="symbolic"):
        resolve_plugins(engine, project)
