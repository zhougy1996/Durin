import importlib.util
import json
from pathlib import Path
import sys

import pytest
from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "Engine/Source/Programs/DurinHeaderTool"))
from durin_header_tool import config as configs, io as utils
from durin_header_tool.config import plugin_build
from durin_header_tool.config.plugin_config import resolve_plugins
from durin_header_tool.generators.project_cmake_file_generator import generate_project_cmake_file
from durin_header_tool.generators.module_cmake_file_generator import generate_all_module_cmake_files_for_project


@pytest.fixture
def workspace(tmp_path, monkeypatch):
    cases = json.loads((ROOT / "Engine/Tests/Native/PluginResolutionTests/Data/Cases.json").read_text(encoding="utf-8"))["Cases"]
    case = next(c for c in cases if c["Name"] == "DependencyEditor")
    for relative, text in case["Files"].items():
        path = tmp_path / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    monkeypatch.setattr(configs.project_config, "PROJECT_CONFIGS", {})
    monkeypatch.setattr(configs.module_config, "MODULE_CONFIGS", {})
    monkeypatch.setattr(configs.module_config, "ENABLED_MODULES", {})
    # Generators import this dictionary as well.
    from durin_header_tool.generators import project_cmake_file_generator
    monkeypatch.setattr(project_cmake_file_generator, "PROJECT_CONFIGS", configs.project_config.PROJECT_CONFIGS)
    monkeypatch.setattr(configs.environment, "DURIN_ENGINE_PROJECT_DIR", tmp_path / "Engine")
    monkeypatch.setattr(configs, "RUNTIME_VARIANT", "DurinEditor")
    monkeypatch.setattr(configs, "ARCH", "Win64")
    for name in ("CATALOGS", "LOCATIONS"):
        monkeypatch.setattr(plugin_build, name, {})
    monkeypatch.setattr(plugin_build, "DISCOVERY_DIRECTORIES", set())
    return tmp_path


def test_plugin_ownership_generation_disable_and_variant(workspace, monkeypatch):
    project = workspace / "Game/Game.dproject"
    configs.init_configs([project])
    runtime = configs.get_module_config("WaterRuntime")
    assert runtime.owning_project == "Game"
    assert runtime.owning_plugin == "Water"
    assert configs.get_project_config("Game").module_dirs == {}
    assert "WaterTools" not in configs.collect_enabled_modules_for_project("Game", "DurinGame")
    generate_project_cmake_file("Game")
    generate_all_module_cmake_files_for_project("Game")
    directory = utils.get_module_intermediate_build_dir("WaterRuntime")
    assert directory.parts[-3:] == ("Plugins", "Water", "WaterRuntime")
    assert (directory / "WaterRuntimeAPI.h").exists()
    assert "FoundationRuntime" in utils.get_module_cmake_file_path("WaterRuntime").read_text()
    assert "list(APPEND DURIN_PROJECT_PLUGIN_MODULES WaterRuntime)" in utils.get_project_cmake_file_path("Game").read_text()
    editor_directory = directory
    monkeypatch.setattr(configs, "RUNTIME_VARIANT", "DurinGame")
    configs.init_configs([project])
    assert "WaterTools" not in configs.module_config.MODULE_CONFIGS
    assert utils.get_module_intermediate_build_dir("WaterRuntime") != editor_directory
    data = json.loads(project.read_text())
    data["Plugins"] = {"Water": False}
    project.write_text(json.dumps(data))
    configs.init_configs([project])
    assert "WaterRuntime" not in configs.module_config.MODULE_CONFIGS
    generate_project_cmake_file("Game")
    assert "list(APPEND DURIN_PROJECT_PLUGIN_MODULES WaterRuntime)" not in utils.get_project_cmake_file_path("Game").read_text()
    data["Plugins"] = {"Water": True}
    project.write_text(json.dumps(data))
    configs.init_configs([project])
    assert configs.get_module_config("WaterRuntime").owning_plugin == "Water"


def test_foreign_project_roots_do_not_activate_current_project_plugins(workspace):
    project = workspace / "Game/Game.dproject"
    data = json.loads(project.read_text())
    data.update(ModuleDirs={"GameRoot": "Source/GameRoot"}, BaseModules=["GameRoot"])
    project.write_text(json.dumps(data))
    module = workspace / "Game/Source/GameRoot/GameRoot.dmodule"
    module.parent.mkdir(parents=True)
    module.write_text(json.dumps(dict(ModuleName="GameRoot", PublicDependencies=["WaterRuntime"])))
    configs.init_configs([project])
    assert "Plugins" not in plugin_build.CATALOGS["Engine"]  # build catalog, not startup selection
    assert plugin_build.CATALOGS["Game"]["Modules"]
    # A missing plugin requested by the active project still fails.
    data["Plugins"] = {"Water": False}
    project.write_text(json.dumps(data))
    with pytest.raises(ValueError, match="disabled or ineligible"):
        resolve_plugins(workspace / "Engine", project)


def test_plugin_dependency_enables_nonroot_module_in_another_project(workspace):
    engine = workspace / "Engine/Engine.dproject"
    engine.write_text(json.dumps(dict(ProjectName="Engine", ModuleDirs={"Helper": "Source/Helper"}, BaseModules=[])))
    helper = workspace / "Engine/Source/Helper/Helper.dmodule"
    helper.parent.mkdir(parents=True)
    helper.write_text(json.dumps(dict(ModuleName="Helper")))
    runtime = workspace / "Game/Plugins/Water/Source/WaterRuntime/WaterRuntime.dmodule"
    data = json.loads(runtime.read_text())
    data["PublicDependencies"].append("Helper")
    runtime.write_text(json.dumps(data))
    configs.init_configs([workspace / "Game/Game.dproject"])
    assert configs.collect_enabled_modules_for_project("Engine", "DurinEditor") == {"Helper"}
    assert configs.get_module_config("Helper").owning_plugin == ""


def test_inventory_is_exact_and_configuration_isolated(tmp_path):
    spec = importlib.util.spec_from_file_location("plugin_inventory", ROOT / "Engine/Scripts/Build/plugin_inventory.py")
    inventory = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(inventory)
    selection = tmp_path / "Selection.json"
    selection.write_text(json.dumps(dict(Inputs=[dict(Path="Plugin/Water", Hash="0123456789abcdef")])))
    module, dependency = tmp_path / "Module.dll", tmp_path / "Core.dll"
    module.write_bytes(b"module")
    dependency.write_bytes(b"core")
    inputs = tmp_path / "Inventory.inputs"
    schema = json.loads((ROOT / "Engine/Source/Programs/DurinHeaderTool/schemas/durin-plugin-build.schema.json").read_text())
    for configuration in ("Debug", "Release"):
        inputs.write_text(f"engine-build\nWin64\nx64\n{configuration}\nDurinGame\nMSVC-19\nModule\tWater\tCore;\n---\nModule\t{module}\nCore\t{dependency}\n")
        result = inventory.publish(selection, inputs, tmp_path / configuration)
        Draft202012Validator(schema).validate(result)
        assert result["Binaries"][0]["RuntimeDependencies"] == ["Runtime/Core.dll"]
    assert (tmp_path / "Debug/PluginBuild.json").read_bytes() != (tmp_path / "Release/PluginBuild.json").read_bytes()
    inputs.write_text("engine-build\nWin64\nx64\nDebug\nDurinGame\nMSVC-19\n---\n")
    result = inventory.publish(selection, inputs, tmp_path / "Debug")
    assert result["Binaries"] == []
    assert (tmp_path / "Debug/Runtime/Module.dll").exists()  # stale artifacts are not membership


def test_blocked_requests_remove_modules_from_build_catalog(workspace):
    project = workspace / "Game/Game.dproject"
    configs.init_configs([project])
    original = plugin_build.CATALOGS["Game"]
    data = json.loads(project.read_text())
    data["Plugins"] = {"Water": True, "Foundation": False}
    project.write_text(json.dumps(data))
    configs.init_configs([project])
    assert plugin_build.CATALOGS["Game"] != original
    assert plugin_build.CATALOGS["Game"]["Modules"] == []
    selection, _ = resolve_plugins(workspace / "Engine", project)
    assert selection["Plugins"] == [] and selection["Modules"] == []
    assert selection["InactivePlugins"] == [dict(Id="Water", Reason="Requires disabled plugin Foundation")]
    module = workspace / "Game/Plugins/Water/Source/WaterRuntime/WaterRuntime.dmodule"
    data["Plugins"] = {"Water": False}
    project.write_text(json.dumps(data))
    module.write_text('{"ModuleName":"Wrong"}')
    configs.init_configs([project])
    assert "WaterRuntime" not in configs.module_config.MODULE_CONFIGS


def test_build_discovers_plugins_in_other_workspace_projects(workspace):
    import shutil
    project = workspace / "Game/Game.dproject"
    other = workspace / "Other/Other.dproject"
    other.parent.mkdir()
    other.write_text(json.dumps(dict(ProjectName="Other", BaseModules=[])))
    shutil.move(str(workspace / "Game/Plugins"), str(other.parent / "Plugins"))
    game_data = json.loads(project.read_text())
    game_data["Plugins"] = {}
    project.write_text(json.dumps(game_data))
    other.write_text(json.dumps(dict(ProjectName="Other", BaseModules=[], Plugins={"Water": True})))
    configs.init_configs([project, other])
    assert configs.get_module_config("WaterRuntime").owning_project == "Other"
    assert plugin_build.CATALOGS["Game"]["Modules"] == []
    assert {m["Owner"] for m in plugin_build.CATALOGS["Other"]["Modules"]} == {"Water", "Foundation"}
    assert {m["Owner"] for m in resolve_plugins(workspace / "Engine", other, project_files=[project])[0]["Modules"]} == {"Water", "Foundation"}
