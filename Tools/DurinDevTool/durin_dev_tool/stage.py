"""Source backend metadata and worker dispatch; all staging policy belongs to GameExport."""
from __future__ import annotations
import json
from pathlib import Path
import subprocess

from .errors import DevToolError
from .build.selection import preset_cache_string
from .game_targets import read_backend, select_game
from .runtime_program import select_runtime, resolve_project, locate_executable, ExecutableDescription


def dispatch_worker(executable: Path, request: dict, *, stdout, stderr) -> dict:
    """Keep stdin owned until a terminal result; cancellation is an acknowledged request."""
    with subprocess.Popen([str(executable), "--protocol=1"], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=stderr, text=True, encoding="utf-8") as child:
        assert child.stdin is not None and child.stdout is not None
        child.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        child.stdin.flush()
        result = None
        sequence = -1
        try:
            for line in child.stdout:
                if len(line.encode("utf-8")) > 1024 * 1024:
                    raise DevToolError("Export worker event exceeds protocol limit")
                event = json.loads(line)
                if (event.get("ProtocolVersion") != 1 or event.get("RequestId") != request["RequestId"]
                        or event.get("Sequence", -1) <= sequence or result is not None):
                    raise DevToolError("Invalid export worker event sequence/version/correlation")
                sequence = event["Sequence"]
                if event.get("Kind") == "result":
                    result = event
        except KeyboardInterrupt:
            child.stdin.write(json.dumps(dict(ProtocolVersion=1, Kind="cancel", RequestId=request["RequestId"])) + "\n")
            child.stdin.flush()
            for line in child.stdout:
                event = json.loads(line)
                if event.get("Kind") == "result":
                    result = event
        finally:
            child.stdin.close()
        code = child.wait(timeout=30)
        if result is None or result.get("ExitCode") != code or not result.get("ChildrenRetired"):
            raise DevToolError("Export worker exited without a matching terminal/cleanup result")
        if code:
            raise DevToolError(f"Export {result.get('Category')}: {result.get('Message')}")
        return result


def run(namespace, *, repository_context, stdout, stderr, **_kwargs):
    repository = repository_context
    host = select_runtime(repository, profile_name=str(namespace.profile or ""), preset_name=str(namespace.preset or ""))
    project = resolve_project(repository, namespace.project_path)
    executable = locate_executable(host, ExecutableDescription("Export tool", "DurinExportTool", "DurinExportTool"))
    backend_path, backend = read_backend(host)
    configuration = getattr(namespace, "configuration", "") or preset_cache_string(host.preset, "CMAKE_BUILD_TYPE")
    game = select_game(backend, project=project, platform=host.profile.platform,
                       configuration=None if namespace.game_preset else configuration,
                       preset=namespace.game_preset, allowed=host.profile.presets)
    request = dict(ProtocolVersion=1, Kind="request", RequestId="DevTool1",
        Operation="preview" if getattr(namespace, "preview", False) else "stage",
        ProjectFile=str(project), Platform=host.profile.platform, Configuration=game["Configuration"],
        Output=str((repository.root / namespace.output_dir).resolve()), EngineRoot=str(repository.root / "Engine"),
        Backend="source", BackendMetadata=str(backend_path), Build="reuse", Cook="reuse", Verify=False, Archive=False,
        CookDirectory=str((repository.root / namespace.cook_dir).resolve()), Preset=game["Name"])
    report = dispatch_worker(executable, request, stdout=stdout, stderr=stderr)
    print(json.dumps(report) if namespace.output_format == "json" else report["Message"], file=stdout)
    return 0
