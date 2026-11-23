"""Publish/package the declared export tool component; never invoked by the worker."""
import argparse
import hashlib
import json
import os
import shutil
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2), encoding="utf-8")


def generate_backend(engine, runtime, cmake, output):
    """Configure-time product sets; clients never reconstruct build output paths."""
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "Tools/DurinDevTool"))
    from durin_dev_tool.build.config_io import load_configure_presets
    from durin_dev_tool.build.selection import preset_cache_string, preset_build_directory
    root = engine.parent
    manifest = root / "CMakePresets.json"
    visible = {p["name"] for p in json.loads(manifest.read_text())["configurePresets"] if not p.get("hidden", False)}
    projects = [json.loads((root / f).read_text())["ProjectName"] for f in json.loads((root / "Durin.dworkspace").read_text())["Projects"]]
    games = []
    for preset in load_configure_presets(manifest).values():
        cache = lambda key: preset_cache_string(preset, key, required=False)
        configuration = cache("CMAKE_BUILD_TYPE")
        if preset.name not in visible or cache("DURIN_RUNTIME_VARIANT") != "DurinGame" or configuration not in {"Debug", "Release"}:
            continue
        system = cache("CMAKE_SYSTEM_NAME")
        hosts = preset.values.get("vendor", {}).get("microsoft.com/VisualStudioSettings/CMake/1.0", {}).get("hostOS", [])
        platform = "MacOS" if system == "Darwin" or (not system and hosts == ["macOS"]) else "Win64" if system == "Windows" or (not system and hosts == ["Windows"]) else None
        if platform is None:
            raise ValueError("Game preset needs CMAKE_SYSTEM_NAME or an explicit hostOS: " + preset.name)
        artifacts = preset_build_directory(preset, root=root)
        for project in projects:
            games.append(dict(Project=project, Platform=platform, Name=preset.name, Configuration=configuration,
                              TargetReceipt=str(artifacts / "TargetReceipts" / project / configuration / "TargetReceipt.json"),
                              ArtifactsRoot=str(artifacts), DependenciesRoot=str(engine / "External")))
    backend = dict(SchemaVersion=1, Kind="source-stage", WorkingDirectory=str(root), CMake=str(cmake),
                   AssetTool=str(runtime / ("DurinAssetTool.exe" if os.name == "nt" else "DurinAssetTool")), Games=games)
    if os.name == "nt":
        backend["Environment"] = {k: os.environ[k] for k in ("PATH", "INCLUDE", "LIB", "LIBPATH") if k in os.environ}
    content = json.dumps(backend, indent=2)
    if not output.is_file() or output.read_text(encoding="utf-8") != content:
        output.write_text(content, encoding="utf-8")


def publish(recipe, runtime, engine, compatibility, abi, backend_file):
    runtime.mkdir(parents=True, exist_ok=True)
    files = {}
    for line in recipe.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        source = Path(line)
        dest = runtime / source.name
        digest = sha(source)
        if dest.name in files and files[dest.name] != digest:
            raise ValueError("Conflicting tool component file: " + dest.name)
        if source.resolve() != dest.resolve():
            shutil.copy2(source, dest)
        files[dest.name] = digest
    backend = runtime / "ExportBackend.json"
    data = json.loads(backend_file.read_text(encoding="utf-8"))
    data.update(Component="ExportComponent.json", AssetTool="DurinAssetTool.exe", PolicyFile="ExportPolicy.json")
    write(backend, data)
    shutil.copy2(engine / "Source/Developer/GameExport/Contracts/win64-system-v1.json", runtime / "ExportPolicy.json")
    for item in [backend, runtime / "ExportPolicy.json"]:
        files[item.name] = sha(item)
    for schema in (engine / "Source/Developer/GameExport/Contracts").glob("*.schema.json"):
        target = runtime / "Contracts" / schema.name
        target.parent.mkdir(exist_ok=True)
        shutil.copy2(schema, target)
        files[target.relative_to(runtime).as_posix()] = sha(target)
    component = dict(SchemaVersion=1, Name="DurinExportTool", ProtocolVersion=1,
                     EngineBuild="source:" + compatibility, Platform="Win64", Architecture="x64", ToolchainABI=abi,
                     DefaultBackend="source", EngineRoot=str(engine), Worker="DurinExportTool.exe", AssetTool="DurinAssetTool.exe",
                     Capabilities=["preview", "stage-prebuilt", "export-prebuilt", "build-source", "cook", "verify", "archive"],
                     Files=[dict(Path=p, Sha256=h) for p, h in sorted(files.items())],
                     Schemas=sorted(p for p in files if p.startswith("Contracts/")), ProviderMetadata=[], BackendMetadata="ExportBackend.json")
    write(runtime / "ExportComponent.json", component)


def bundle(component_file, project, receipt_file, destination, cook=None):
    """Build an isolated reuse-only component from explicit tool/product receipts."""
    if destination.exists():
        raise ValueError("Bundle destination must be new")
    component = json.loads(component_file.read_text(encoding="utf-8"))
    receipt = json.loads(receipt_file.read_text(encoding="utf-8"))
    root = component_file.parent
    destination.mkdir(parents=True)
    for record in component["Files"]:
        source = (root / record["Path"]).resolve()
        if not source.is_relative_to(root.resolve()) or sha(source) != record["Sha256"]:
            raise ValueError("Stale/invalid component input: " + record["Path"])
        target = destination / record["Path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    engine = Path(component["EngineRoot"])
    workspace = engine.parent
    # Only declared project catalog descriptors, not build trees or native source.
    workspace_file = workspace / "Durin.dworkspace"
    project_files = [workspace / p for p in json.loads(workspace_file.read_text(encoding="utf-8"))["Projects"]]
    metadata = [workspace_file, *project_files]
    for file in project_files:
        for folder, pattern in [("Source", "*.dmodule"), ("Plugins", "*.dmodule"), ("Plugins", "*.dplugin")]:
            metadata.extend((file.parent / folder).rglob(pattern))
    for source in metadata:
        target = destination / "Metadata" / source.relative_to(workspace)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    backend_source = json.loads((root / component["BackendMetadata"]).read_text(encoding="utf-8"))
    games = [g for g in backend_source["Games"] if g.get("Project") == receipt["Identity"]["Project"] and g["Configuration"] == receipt["Identity"]["Configuration"] and g.get("Platform", "Win64") == receipt["Identity"].get("Platform", "Win64")]
    if len(games) != 1:
        raise ValueError("One matching source product set is required")
    roots = dict(Engine=engine, Project=project.parent, Artifacts=Path(games[0]["ArtifactsRoot"]), Dependencies=Path(games[0]["DependenciesRoot"]))
    targets = dict(Engine=destination / "Metadata/Engine", Project=destination / "Products/Project",
                   Artifacts=destination / "Products/Artifacts", Dependencies=destination / "Products/Dependencies")
    for product in receipt["Products"]:
        source_record = product["Source"]
        source = (roots[source_record["Root"]] / source_record["Path"]).resolve()
        if not source.is_relative_to(roots[source_record["Root"]].resolve()) or sha(source) != product["Sha256"]:
            raise ValueError("Stale/invalid native product")
        target = targets[source_record["Root"]] / source_record["Path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    for target in targets.values():
        target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(receipt_file, destination / "TargetReceipt.json")
    backend = dict(SchemaVersion=1, Kind="prebuilt-stage", Component="ExportComponent.json",
                   TargetReceipt="TargetReceipt.json", ArtifactsRoot="Products/Artifacts",
                   DependenciesRoot="Products/Dependencies", ProjectProductsRoot="Products/Project",
                   PolicyFile="ExportPolicy.json", AssetTool="DurinAssetTool.exe", CMake="cmake.exe")
    write(destination / "ExportBackend.json", backend)
    if cook:
        shutil.copytree(cook, destination / "Cook", ignore=shutil.ignore_patterns("Logs", "DerivedDataCache"))
        backend["CookDirectory"] = "Cook"
        write(destination / "ExportBackend.json", backend)
    component.update(DefaultBackend="prebuilt", EngineRoot="Metadata/Engine", Capabilities=["preview", "stage-prebuilt", "export-prebuilt", "verify", "archive"])
    component["Files"] = [dict(Path=p.relative_to(destination).as_posix(), Sha256=sha(p))
                          for p in sorted(destination.rglob("*")) if p.is_file() and p.name != "ExportComponent.json"]
    write(destination / "ExportComponent.json", component)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--generate-backend", action="store_true")
    p.add_argument("--cmake", type=Path)
    p.add_argument("--recipe", type=Path)
    p.add_argument("--backend", type=Path)
    p.add_argument("--runtime", type=Path)
    p.add_argument("--engine", type=Path)
    p.add_argument("--compatibility")
    p.add_argument("--abi")
    p.add_argument("--bundle", type=Path)
    p.add_argument("--project", type=Path)
    p.add_argument("--receipt", type=Path)
    p.add_argument("--output", type=Path)
    p.add_argument("--cook", type=Path)
    a = p.parse_args()
    if a.generate_backend:
        generate_backend(a.engine.resolve(), a.runtime.resolve(), a.cmake.resolve(), a.output.resolve())
    elif a.bundle:
        bundle(a.bundle.resolve(), a.project.resolve(), a.receipt.resolve(), a.output.resolve(), a.cook.resolve() if a.cook else None)
    else:
        publish(a.recipe, a.runtime, a.engine, a.compatibility, a.abi, a.backend)
