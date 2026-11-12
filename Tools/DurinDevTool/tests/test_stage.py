from __future__ import annotations

import json
from pathlib import Path
import struct

import pytest

from durin_dev_tool import stage
from durin_dev_tool.errors import DevToolError


def write(path: Path, value: bytes | str | dict) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(value, dict):
        value = json.dumps(value)
    path.write_bytes(value.encode() if isinstance(value, str) else value)
    return path


@pytest.fixture
def deployment(tmp_path, monkeypatch):
    root = tmp_path / "source"
    project = write(root / "Game/Game.dproject", {"ProjectName": "Game", "BaseModules": [], "Plugins": {"Water": True}})
    write(root / "Engine/Engine.dproject", {"ProjectName": "Engine", "BaseModules": []})
    write(root / "Durin.dworkspace", {"Projects": ["Engine/Engine.dproject", "Game/Game.dproject"]})
    write(root / "Game/Plugins/Water/Water.dplugin", {
        "SchemaVersion": 1, "Id": "Water", "Modules": [{"Name": "Water", "Path": "Source"}]})
    write(root / "Game/Plugins/Water/Source/Water.dmodule", {"ModuleName": "Water"})
    selection, _ = stage.resolve_plugins(root / "Engine", project, platform="Win64", runtime_variant="DurinGame",
        project_files=[root / "Engine/Engine.dproject", project])
    runtime = root / "Engine/Binaries/Win64/Debug/Runtime/DurinGame"
    write(runtime / "DurinGame.exe", "game")
    write(runtime / "DurinGame-Core.dll", "core")
    identity = dict(Platform="Win64", Architecture="x64", Configuration="Debug", ToolchainABI="abi",
        EngineBuild="1:" + stage.sha256(runtime / "DurinGame-Core.dll"), RuntimeVariant="DurinGame")
    inventory = root / "Build/PluginBuild/PluginBuild.json"
    write(inventory.parent / "Runtime/Water.dll", "water")
    write(inventory.parent / "Runtime/native.dll", "native")
    write(inventory, dict(SchemaVersion=2, Identity=identity, Inputs=selection["Inputs"], Binaries=[
        dict(Module="Water", Owner="Water", Path="Runtime/Water.dll", RuntimeDependencies=["Runtime/native.dll"])]))
    snapshot = dict(SchemaVersion=1, EngineVersion="1", Selection=json.dumps(selection),
        ProjectDescriptor=project.read_text(), EngineGameDefaults="{}\n", Mounts=[], **{k: identity[k] for k in ("Platform", "Architecture", "Configuration", "ToolchainABI")},
        **{"Project.yaml": "{}", "DurinGame.yaml": "{}"})
    cook = root / "Cook"
    write(cook / "PluginCook.json", snapshot)
    write(cook / "CookManifest.bin", "manifest")
    write(cook / "Shaders/ShaderLibrary.dslb", "shader")
    info = dict(Platform="win64", Snapshot=json.dumps(snapshot), CookManifestSha256=stage.sha256(cook / "CookManifest.bin"),
        Files=[dict(Path=p, Sha256=stage.sha256(cook / p)) for p in ("PluginCook.json", "Shaders/ShaderLibrary.dslb")])
    output = tmp_path / "deployed"
    write(output / "previous.txt", "previous generation")
    monkeypatch.setattr(stage, "pe_imports", lambda p: {"Water.dll": ["native.dll"], "native.dll": ["leaf.dll"]}.get(p.name, []))
    write(runtime.parent.parent / "ThirdParty/leaf.dll", "leaf")
    return dict(root=root, project=project, cook=cook, output=output, game_runtime=runtime, inventory_file=inventory, info=info)


def test_stage_selects_runtime_and_transitive_native_closure(deployment):
    result = stage.stage_game(**deployment)
    runtime = Path(result["runtime"])
    assert result["plugins"] == ["Water"]
    assert not (deployment["output"] / "previous.txt").exists()
    manifest = json.loads((runtime / "PluginRuntime.json").read_text())
    files = {f["Path"] for f in manifest["Files"]}
    assert {"DurinGame.exe", "DurinGame-Core.dll", "Plugins/Runtime/Water.dll", "native.dll", "leaf.dll"} <= files
    assert all(stage.sha256(runtime / f["Path"]) == f["Sha256"] for f in manifest["Files"])
    assert not list(deployment["output"].rglob("*.dplugin"))
    assert not list(deployment["output"].rglob("*DurinEditor*"))
    assert (runtime / "Templates/TP_DurinGame.yaml").read_bytes() == b"{}\n"


def recapture(deployment):
    selection, _ = stage.resolve_plugins(deployment["root"] / "Engine", deployment["project"],
        platform="Win64", runtime_variant="DurinGame", project_files=[deployment["root"] / "Engine/Engine.dproject", deployment["project"]])
    snapshot = json.loads(deployment["info"]["Snapshot"])
    snapshot.update(Selection=json.dumps(selection), ProjectDescriptor=deployment["project"].read_text())
    deployment["info"]["Snapshot"] = json.dumps(snapshot)
    write(deployment["cook"] / "PluginCook.json", snapshot)
    for item in deployment["info"]["Files"]:
        item["Sha256"] = stage.sha256(deployment["cook"] / item["Path"])
    inventory = json.loads(deployment["inventory_file"].read_text())
    inventory["Inputs"] = selection["Inputs"]
    write(deployment["inventory_file"], inventory)


@pytest.mark.parametrize("plugins", [{}, {"Water": True}])
def test_stage_handles_no_native_plugins(deployment, plugins):
    write(deployment["project"], dict(ProjectName="Game", BaseModules=[], Plugins=plugins))
    write(deployment["root"] / "Game/Plugins/Water/Water.dplugin", dict(SchemaVersion=1, Id="Water"))
    recapture(deployment)
    result = stage.stage_game(**deployment)
    runtime = Path(result["runtime"])
    manifest = json.loads((runtime / "PluginRuntime.json").read_text())
    assert manifest["Plugins"] == list(plugins)
    assert manifest["Binaries"] == []
    assert not list(runtime.rglob("Water.dll"))


def test_stage_preserves_ordinary_project_module_roots(deployment):
    descriptor = json.loads(deployment["project"].read_text())
    descriptor.update(BaseModules=["GameModule"], ModuleDirs={"GameModule": "Source/GameModule"})
    write(deployment["project"], descriptor)
    write(deployment["project"].parent / "Source/GameModule/GameModule.dmodule", dict(ModuleName="GameModule"))
    write(deployment["project"].parent / "Binaries/Win64/Debug/Runtime/DurinGame/DurinGame-GameModule.dll", "module")
    recapture(deployment)
    result = stage.stage_game(**deployment)
    assert (Path(result["runtime"]) / "DurinGame-GameModule.dll").read_text() == "module"


def test_stage_cannot_replace_build_inventory(deployment):
    deployment["output"] = deployment["inventory_file"].parent
    with pytest.raises(DevToolError, match="overlaps input"):
        stage.stage_game(**deployment)
    assert deployment["inventory_file"].is_file()


@pytest.mark.parametrize("failure", ["native", "cook", "selection", "identity", "shader", "platform", "stale"])
def test_stage_rejects_invalid_inputs_preserving_previous_generation(deployment, failure):
    if failure == "native":
        (deployment["game_runtime"].parent.parent / "ThirdParty/leaf.dll").unlink()
    elif failure == "cook":
        write(deployment["cook"] / "PluginCook.json", "corrupt")
    elif failure == "selection":
        descriptor = json.loads(deployment["project"].read_text())
        descriptor["Plugins"]["Water"] = False
        write(deployment["project"], descriptor)
    elif failure == "identity":
        inventory = json.loads(deployment["inventory_file"].read_text())
        inventory["Identity"]["Configuration"] = "Release"
        write(deployment["inventory_file"], inventory)
    elif failure == "shader":
        deployment["info"]["Files"] = [f for f in deployment["info"]["Files"] if not f["Path"].endswith(".dslb")]
    elif failure == "platform":
        deployment["info"]["Platform"] = "macos"
    else:
        inventory = json.loads(deployment["inventory_file"].read_text())
        for item in inventory["Inputs"]:
            if item["Path"].startswith("Plugin/Water"):
                item["Hash"] = "stale"
        write(deployment["inventory_file"], inventory)
    with pytest.raises(DevToolError):
        stage.stage_game(**deployment)
    assert (deployment["output"] / "previous.txt").read_text() == "previous generation"
    assert not list(deployment["output"].parent.glob("deployed.stage-*"))


def test_stage_rolls_back_failed_replacement(deployment, monkeypatch):
    replace = stage.os.replace
    def fail_publish(source, destination):
        if Path(source).name == "payload":
            raise OSError("injected publication failure")
        return replace(source, destination)
    monkeypatch.setattr(stage.os, "replace", fail_publish)
    with pytest.raises(OSError, match="injected"):
        stage.stage_game(**deployment)
    assert (deployment["output"] / "previous.txt").read_text() == "previous generation"


@pytest.mark.parametrize("occupied", [False, True])
def test_stage_preserves_backup_when_rollback_is_unavailable(deployment, monkeypatch, occupied):
    replace = stage.os.replace
    def fail_publish_and_restore(source, destination):
        if Path(source).name in {"payload", "previous"}:
            if occupied:
                write(Path(destination) / "foreign.txt", "occupied")
            raise OSError("injected rename failure")
        return replace(source, destination)
    monkeypatch.setattr(stage.os, "replace", fail_publish_and_restore)
    with pytest.raises(DevToolError, match="restore previous output"):
        stage.stage_game(**deployment)
    backups = list(deployment["output"].parent.glob("deployed.stage-*/previous/previous.txt"))
    assert len(backups) == 1 and backups[0].read_text() == "previous generation"


@pytest.mark.parametrize("relative", ["../outside", "/outside", "C:/outside", "a\\b", ""])
def test_stage_paths_cannot_escape(tmp_path, relative):
    with pytest.raises(DevToolError):
        stage.contained(tmp_path, relative)


@pytest.mark.parametrize("delay", [False, True])
def test_pe_reads_ordinary_and_delay_imports(tmp_path, delay):
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x86, 1)
    struct.pack_into("<H", data, 0x94, 240)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20b)
    struct.pack_into("<IIII", data, optional + 240 + 8, 512, 0x1000, 512, 512)
    struct.pack_into("<II", data, optional + 112 + (13 if delay else 1) * 8, 0x1000, 64)
    struct.pack_into("<I", data, 512 + (4 if delay else 12), 0x1080)
    if delay:
        struct.pack_into("<I", data, 512, 1)
    data[640:651] = b"native.dll\0"
    binary = write(tmp_path / "binary.dll", bytes(data))
    assert stage.pe_imports(binary) == ["native.dll"]
    binary.write_bytes(bytes(data[:520]))
    with pytest.raises(DevToolError, match="Invalid PE"):
        stage.pe_imports(binary)
