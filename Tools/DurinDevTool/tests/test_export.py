import io
import json
from pathlib import Path
from types import SimpleNamespace
from unittest import mock
from durin_dev_tool import export
from durin_dev_tool.registry import CommandRegistry


def test_export_client_forwards_component_and_request_without_product_policy(tmp_path):
    project = tmp_path / "Project.dproject"
    project.write_text('{"ProjectName":"Example"}')
    worker = tmp_path / "tools/worker.exe"
    worker.parent.mkdir()
    (worker.parent / "ExportComponent.json").write_text(json.dumps(dict(EngineRoot="Metadata/Engine", BackendMetadata="backend.json", DefaultBackend="source")))
    _, args = CommandRegistry().parse(["export", "--project", str(project), "--output-dir", "Export1", "--configuration", "Debug", "--verify", "--archive"])
    result = dict(ExitCode=0, Message="completed")
    with mock.patch.object(export, "select_runtime"), mock.patch.object(export, "resolve_project", return_value=project), mock.patch.object(export, "locate_executable", return_value=worker), mock.patch.object(export, "dispatch_worker", return_value=result) as dispatch:
        assert export.run(args, repository_context=SimpleNamespace(root=tmp_path), stdout=io.StringIO(), stderr=io.StringIO()) == 0
    request = dispatch.call_args.args[1]
    assert request["Operation"] == "export" and request["Configuration"] == "Debug"
    assert request["Verify"] and request["Archive"]
    assert request["Build"] == request["Cook"] == "auto"
    assert request["BackendMetadata"] == str((worker.parent / "backend.json").resolve())
    assert "Receipt" not in request and "CookDirectory" not in request
    assert not (tmp_path / "Export1").exists()
