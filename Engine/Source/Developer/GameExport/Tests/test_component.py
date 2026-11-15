"""Tool component recipes remain independent of the source-development client."""
import importlib.util
import json
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[5]
spec = importlib.util.spec_from_file_location("export_component", ROOT / "Engine/Scripts/Build/export_component.py")
component = importlib.util.module_from_spec(spec)
spec.loader.exec_module(component)


def fixture(tmp_path):
    workspace = tmp_path / "workspace"
    engine = workspace / "Engine"
    project = workspace / "Example/Example.dproject"
    for file, data in [(workspace / "Durin.dworkspace", '{"Projects":["Engine/Engine.dproject","Example/Example.dproject"]}'),
                       (engine / "Engine.dproject", '{"ProjectName":"Engine"}'),
                       (project, '{"ProjectName":"Example"}'),
                       (engine / "Source/Runtime/Native/Native.dmodule", '{}'),
                       (engine / "Source/Runtime/Native/Secret.cpp", 'source unavailable after packaging'),
                       (engine / "Source/Developer/GameExport/Contracts/win64-system-v1.json", '{}'),
                       (engine / "Source/Developer/GameExport/Contracts/example.schema.json", '{}')]:
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(data)
    files = tmp_path / "inputs"
    files.mkdir()
    for name in ["DurinExportTool.exe", "DurinAssetTool.exe", "cmake.exe", "Native.dll"]:
        (files / name).write_text(name)
    recipe = tmp_path / "recipe"
    recipe.write_text("\n".join(str(p) for p in files.iterdir()))
    runtime = tmp_path / "runtime"
    backend = tmp_path / "backend.json"
    backend.write_text(json.dumps(dict(SchemaVersion=1, Kind="source-stage", Games=[dict(Project="Example", Configuration="Release", ArtifactsRoot=str(tmp_path / "artifacts"), DependenciesRoot=str(engine / "External"))])))
    component.publish(recipe, runtime, engine, "a" * 64, "test", backend)
    return engine, project, runtime, recipe


def test_component_inventory_binds_worker_tools_and_metadata(tmp_path):
    _, _, runtime, _ = fixture(tmp_path)
    value = json.loads((runtime / "ExportComponent.json").read_text())
    assert value["DefaultBackend"] == "source"
    files = {f["Path"]: f["Sha256"] for f in value["Files"]}
    assert {value["Worker"], value["BackendMetadata"], value["AssetTool"], "ExportPolicy.json"} <= files.keys()
    assert all(component.sha(runtime / p) == h for p, h in files.items())


def test_bundle_uses_receipt_products_and_catalog_metadata_without_native_source(tmp_path):
    engine, project, runtime, _ = fixture(tmp_path)
    product = engine / "Native.dll"
    product.write_text("native product")
    receipt = tmp_path / "TargetReceipt.json"
    receipt.write_text(json.dumps(dict(Identity=dict(Project="Example", Configuration="Release"), Products=[dict(Source=dict(Root="Engine", Path="Native.dll"), Sha256=component.sha(product))])))
    cook = tmp_path / "cook"
    (cook / "Logs").mkdir(parents=True)
    (cook / "CookManifest.bin").write_text("manifest")
    (cook / "Logs/discard.log").write_text("log")
    dest = tmp_path / "bundle"
    component.bundle(runtime / "ExportComponent.json", project, receipt, dest, cook)
    assert not list(dest.rglob("*.cpp"))
    assert (dest / "Metadata/Engine/Source/Runtime/Native/Native.dmodule").exists()
    assert (dest / "Metadata/Engine/Native.dll").read_bytes() == product.read_bytes()
    assert not (dest / "Cook/Logs").exists()
    data = json.loads((dest / "ExportComponent.json").read_text())
    assert data["DefaultBackend"] == "prebuilt" and "build-source" not in data["Capabilities"]
    assert all(component.sha(dest / f["Path"]) == f["Sha256"] for f in data["Files"])
    with pytest.raises(ValueError, match="new"):
        component.bundle(runtime / "ExportComponent.json", project, receipt, dest, cook)


def test_recipe_rejects_conflicting_loader_file_names(tmp_path):
    engine, _, runtime, recipe = fixture(tmp_path)
    another = tmp_path / "different/Native.dll"
    another.parent.mkdir()
    another.write_text("different")
    recipe.write_text(recipe.read_text() + "\n" + str(another))
    with pytest.raises(ValueError, match="Conflicting"):
        component.publish(recipe, runtime, engine, "a" * 64, "test", tmp_path / "backend.json")


def test_generated_products_follow_resolved_preset_paths_and_platforms(tmp_path):
    engine = tmp_path / "Engine"
    engine.mkdir()
    (engine / "Engine.dproject").write_text('{"ProjectName":"Example"}')
    (tmp_path / "Durin.dworkspace").write_text('{"Projects":["Engine/Engine.dproject"]}')
    (tmp_path / "CMakePresets.json").write_text(json.dumps(dict(version=3, configurePresets=[
        dict(name="base", hidden=True, vendor={"microsoft.com/VisualStudioSettings/CMake/1.0":{"hostOS":["Windows"]}}, cacheVariables=dict(DURIN_RUNTIME_VARIANT="DurinGame", CMAKE_BUILD_TYPE="Release")),
        dict(name="CustomGame", inherits="base", binaryDir="${sourceDir}/Different/Products"),
        dict(name="MacGame", binaryDir="${sourceDir}/MacProducts", cacheVariables=dict(DURIN_RUNTIME_VARIANT="DurinGame", CMAKE_BUILD_TYPE="Debug", CMAKE_SYSTEM_NAME="Darwin"))])))
    path = tmp_path / "backend.json"
    component.generate_backend(engine, tmp_path / "runtime", tmp_path / "cmake", path)
    games = json.loads(path.read_text())["Games"]
    assert len(games) == 2
    custom, mac = games
    assert custom["Name"] == "CustomGame" and custom["Platform"] == "Win64"
    assert Path(custom["TargetReceipt"]) == tmp_path / "Different/Products/TargetReceipts/Example/Release/TargetReceipt.json"
    assert mac["Platform"] == "MacOS" and mac["Configuration"] == "Debug"
