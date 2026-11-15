"""Source-development client: no target, closure, build or publication policy."""
import json
from pathlib import Path
from .runtime_program import select_runtime, resolve_project, locate_executable, ExecutableDescription
from .stage import dispatch_worker
from .errors import DevToolError


def run(namespace, *, repository_context, stdout, stderr, **_kwargs):
    repo = repository_context
    host = select_runtime(repo, profile_name=str(namespace.profile or ""), preset_name=str(namespace.preset or ""))
    project = resolve_project(repo, namespace.project_path)
    worker = locate_executable(host, ExecutableDescription("Export tool", "DurinExportTool", "DurinExportTool"))
    file = worker.parent / "ExportComponent.json"
    if not file.is_file():
        raise DevToolError("Missing export component; build ExportComponent")
    component = json.loads(file.read_text(encoding="utf-8"))
    def path(key):
        return str((worker.parent / Path(component[key])).resolve())
    backend = component.get("DefaultBackend", "source")
    request = dict(ProtocolVersion=1, Kind="request", RequestId="DevTool1", Operation="export",
                   ProjectFile=str(project), EngineRoot=path("EngineRoot"), BackendMetadata=path("BackendMetadata"),
                   Platform="Win64", Configuration=namespace.configuration, Output=str((repo.root / namespace.output_dir).resolve()),
                   Backend=backend, Build=namespace.build, Cook=namespace.cook, Verify=namespace.verify,
                   Archive=namespace.archive, Preview=namespace.preview)
    if namespace.cook_dir is not None:
        request["CookDirectory"] = str((repo.root / namespace.cook_dir).resolve())
    result = dispatch_worker(worker, request, stdout=stdout, stderr=stderr)
    print(json.dumps(result) if namespace.output_format == "json" else result["Message"], file=stdout)
    return result["ExitCode"]
