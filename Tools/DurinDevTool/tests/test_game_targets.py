import io
import json
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
import pytest
from durin_dev_tool import game_targets, stage, cook
from durin_dev_tool.build.models import ConfigurePreset
from durin_dev_tool.errors import DevToolError
from durin_dev_tool.registry import CommandRegistry


def fixture(tmp_path):
    project=tmp_path/"Example.dproject";project.write_text('{"ProjectName":"Example"}')
    path=tmp_path/"Host/ExportBackend.json";path.parent.mkdir()
    data=dict(SchemaVersion=1,Kind="source-stage",Games=[dict(Name="Custom",Project="Example",Platform="Win64",Configuration="Release",TargetReceipt="unusual/receipt.json",ArtifactsRoot="products",DependenciesRoot="deps")])
    path.write_text(json.dumps(data))
    host=SimpleNamespace(repository=SimpleNamespace(root=tmp_path),preset=ConfigurePreset("Host",dict(binaryDir="${sourceDir}/Host",cacheVariables=dict(CMAKE_BUILD_TYPE="Debug"))),profile=SimpleNamespace(platform="Win64",presets=["Custom"]))
    return project,path,data,host


def test_clients_read_generated_product_paths_without_preset_name_conventions(tmp_path):
    project,path,data,host=fixture(tmp_path)
    actual,decoded=game_targets.read_backend(host)
    assert actual==path and decoded==data
    game=game_targets.select_game(decoded,project=project,platform="win64",configuration="Release",allowed=host.profile.presets)
    assert game["TargetReceipt"]=="unusual/receipt.json"
    with pytest.raises(DevToolError,match="one matching"):
        game_targets.select_game(decoded,project=project,platform="macos",configuration="Release")


def test_stage_forwards_same_backend_and_explicit_preset_without_temporary_metadata(tmp_path):
    project,path,data,host=fixture(tmp_path)
    _,args=CommandRegistry().parse(["stage","--project",str(project),"--cook-dir","Cook","--output-dir","Output","--game-preset","Custom"])
    with mock.patch.object(stage,"select_runtime",return_value=host),mock.patch.object(stage,"resolve_project",return_value=project),mock.patch.object(stage,"locate_executable",return_value=tmp_path/"worker"),mock.patch.object(stage,"dispatch_worker",return_value=dict(Message="done")) as dispatch:
        assert stage.run(args,repository_context=host.repository,stdout=io.StringIO(),stderr=io.StringIO())==0
    request=dispatch.call_args.args[1]
    assert request["BackendMetadata"]==str(path) and request["Preset"]=="Custom"
    assert request["Configuration"]=="Release" and not (tmp_path/"Output").exists()


def test_cook_uses_generated_receipt_location_for_target_configuration(tmp_path):
    project,path,data,host=fixture(tmp_path)
    _,args=CommandRegistry().parse(["cook","--project",str(project),"--output-dir","Cook","--configuration","Release","--format","json"])
    with mock.patch.object(cook,"select_runtime",return_value=host),mock.patch.object(cook,"invoke_runtime_program",return_value="report") as invoke,mock.patch.object(cook,"_read_report",return_value=dict(status="succeeded")):
        assert cook.run(args,repository_root=tmp_path,repository_context=host.repository,stdout=io.StringIO(),stderr=io.StringIO(),executable_resolver=lambda *_:tmp_path/"tool")==0
    assert "--target-receipt="+str((path.parent/"unusual/receipt.json").resolve()) in invoke.call_args.args[2]
