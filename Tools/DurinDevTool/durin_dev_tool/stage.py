"""Select built Game artifacts and transactionally publish a complete cooked runtime."""
from __future__ import annotations
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import tempfile

from .errors import DevToolError
from .build.config_io import load_configure_presets
from .build.selection import preset_cache_string, preset_build_directory, preset_output_configuration
from .build.plugins import resolve_plugins
from .build.output import BuildOutput
from .build.models import OutputMode
from .runtime_program import (select_runtime, resolve_project, locate_executable,
    invoke_runtime_program, ExecutableDescription, RuntimeProcessPolicy)


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def contained(base: Path, relative: str) -> Path:
    path = Path(relative)
    if not relative or path.is_absolute() or ".." in path.parts or ":" in relative or "\\" in relative:
        raise DevToolError(f"Invalid relative staged path: {relative}")
    result = (base / path).resolve()
    if not result.is_relative_to(base.resolve()):
        raise DevToolError(f"Staged path escapes its root: {relative}")
    return result


def pe_imports(path: Path) -> list[str]:
    """Read ordinary and delay-load PE imports without invoking a compiler/toolchain."""
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise DevToolError(f"Not a PE runtime binary: {path}")
    try:
        pe = struct.unpack_from("<I", data, 0x3c)[0]
        if data[pe:pe+4] != b"PE\0\0": raise ValueError("PE signature")
        count, optional_size = struct.unpack_from("<H",data,pe+6)[0],struct.unpack_from("<H",data,pe+20)[0]
        optional=pe+24; magic=struct.unpack_from("<H",data,optional)[0]
        directories=optional+(112 if magic==0x20b else 96)
        image_base=struct.unpack_from("<Q" if magic==0x20b else "<I",data,optional+(24 if magic==0x20b else 28))[0]
        sections=[struct.unpack_from("<IIII",data,optional+optional_size+i*40+8) for i in range(count)]
        def offset(rva):
            for virtual_size,virtual,raw_size,raw in sections:
                if virtual<=rva<virtual+max(virtual_size,raw_size):return raw+rva-virtual
            raise ValueError("Unmapped import RVA")
        result=[]
        for index,stride in ((1,20),(13,32)):
            rva,size=struct.unpack_from("<II",data,directories+index*8)
            if not rva:continue
            cursor=offset(rva)
            for _ in range(min(size//stride+1,65536)):
                entry=struct.unpack_from("<"+"I"*(stride//4),data,cursor)
                if not any(entry):break
                name=entry[3] if index==1 else entry[1]-(0 if entry[0]&1 else image_base)
                start=offset(name);end=data.index(b"\0",start)
                result.append(data[start:end].decode("ascii").casefold());cursor+=stride
        return result
    except (ValueError,IndexError,struct.error,UnicodeError) as error:
        raise DevToolError(f"Invalid PE import table: {path}: {error}") from error


def stage_game(root: Path, project: Path, cook: Path, output: Path, game_runtime: Path,
               inventory_file: Path, info: dict, *, before_publish=None) -> dict:
    root=root.resolve();project=project.resolve();cook=cook.resolve();output=output.resolve();game_runtime=game_runtime.resolve()
    if output==root or root.is_relative_to(output):raise DevToolError("Stage output overlaps repository root")
    for source in (cook,game_runtime,project.parent):
        if output.is_relative_to(source) or source.is_relative_to(output):raise DevToolError(f"Stage output overlaps input: {source}")
    snapshot=json.loads(info["Snapshot"])
    if snapshot.get("SchemaVersion")!=1:raise DevToolError("Unsupported Cook plugin snapshot")
    frozen=json.loads(snapshot["Selection"])
    workspace=json.loads((root/"Durin.dworkspace").read_text(encoding="utf-8"))
    current,_=resolve_plugins(root/"Engine",project,platform=snapshot["Platform"],runtime_variant="DurinGame",
        project_files=[root/p for p in workspace["Projects"]])
    if current!=frozen:raise DevToolError("Project/plugin selection or inputs changed since Cook; recook before staging")
    descriptor=json.loads(snapshot["ProjectDescriptor"])
    if descriptor!=json.loads(project.read_text(encoding="utf-8")):raise DevToolError("Project descriptor changed since Cook")
    if snapshot["Platform"]!="Win64":raise DevToolError("Native staging is currently qualified for Win64 only")
    expected={key:snapshot[key] for key in ("Platform","Architecture","Configuration","ToolchainABI")}
    expected.update(EngineBuild=snapshot["EngineVersion"]+":"+sha256(game_runtime/"DurinGame-Core.dll"),RuntimeVariant="DurinGame")
    selected={p["Id"] for p in frozen["Plugins"]}
    modules=frozen["Modules"]
    build={"Binaries":[],"Inputs":[],"Identity":expected}
    if selected:
        if not inventory_file.is_file():raise DevToolError("Game plugin inventory missing; build it explicitly before staging")
        build=json.loads(inventory_file.read_text(encoding="utf-8"))
        if build.get("SchemaVersion")!=2 or build["Identity"]!=expected:raise DevToolError("Incompatible Game plugin build inventory")
        inputs={item["Path"]:item["Hash"] for item in build["Inputs"]}
        for item in frozen["Inputs"]:
            key=item["Path"]
            relevant=(key.startswith("Plugin/") and key.split("/")[1] in selected) or (not key.startswith("Plugin/") and key.count("/")==2)
            if relevant and inputs.get(key)!=item["Hash"]:raise DevToolError(f"Stale Game plugin build input: {key}")
    by_module={b["Module"]:b for b in build["Binaries"]}
    if len(by_module)!=len(build["Binaries"]):raise DevToolError("Duplicate plugin module inventory")
    inventory_root=inventory_file.parent.resolve()
    all_plugin_files={contained(inventory_root,b["Path"]):b["Module"] for b in build["Binaries"]}
    names={m["Name"] for m in modules}
    output.parent.mkdir(parents=True,exist_ok=True)
    lock=output.parent/(output.name+".stage-lock")
    try:lock.mkdir()
    except FileExistsError as error:raise DevToolError("Another stage writer owns this output") from error
    temporary=Path(tempfile.mkdtemp(prefix=output.name+".stage-",dir=output.parent)).resolve()
    backup=temporary/"previous";payload=temporary/"payload"
    runtime_relative=Path("Engine/Binaries")/snapshot["Platform"]/snapshot["Configuration"]/"Runtime/DurinGame"
    runtime=payload/runtime_relative;runtime.mkdir(parents=True)
    (payload/"Engine/Content").mkdir(parents=True)
    (runtime.parent.parent/"ThirdParty").mkdir(parents=True)
    copies={};verified={}
    def copy(source: Path,relative: str,expected_hash=None):
        source=source.resolve();target=contained(runtime,relative)
        if not source.is_file():raise DevToolError(f"Missing required runtime dependency: {source}")
        digest=sha256(source)
        if expected_hash and digest!=expected_hash:raise DevToolError(f"Cook input/output changed: {source}")
        key=relative.casefold()
        if key in copies:
            if copies[key]!=digest:raise DevToolError(f"Conflicting runtime files: {relative}")
            return relative
        target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,target)
        if sha256(target)!=digest:raise DevToolError(f"Runtime copy verification failed: {relative}")
        copies[key]=digest;return relative
    try:
        # Engine-owned Game modules are already host-filtered by the Game build.
        engine_binaries=[game_runtime/"DurinGame.exe",*sorted(game_runtime.glob("DurinGame-*.dll"))]
        for binary in engine_binaries:copy(binary,binary.name)
        third=game_runtime.parent.parent/"ThirdParty"
        available={p.name.casefold():p for p in third.rglob("*.dll")}
        queue=list(engine_binaries);visited=set()
        while queue:
            binary=queue.pop()
            if binary in visited:continue
            visited.add(binary)
            for dependency in pe_imports(binary):
                if dependency in available:
                    file=available[dependency]
                    # Keep external DLLs beside the executable; no SDK/developer tools are staged.
                    copy(file,file.name);queue.append(file)
        for item in info["Files"]:copy(contained(cook,item["Path"]),item["Path"],item["Sha256"])
        copy(cook/"CookManifest.bin","CookManifest.bin",info["CookManifestSha256"])
        binaries=[]
        for module in modules:
            source=by_module.get(module["Name"])
            if source is None or source["Owner"]!=module["Owner"] or module["Host"]!="Runtime":raise DevToolError("Missing/mismatched Game plugin module")
            origin=contained(inventory_root,source["Path"])
            relative=copy(origin,"Plugins/Runtime/"+origin.name)
            dependencies=[]
            for raw in source["RuntimeDependencies"]:
                dependency=contained(inventory_root,raw)
                if dependency in all_plugin_files and all_plugin_files[dependency] not in names:raise DevToolError("Native dependency on an inactive plugin")
                # Engine DLLs bind the copies from the matching Game build, not an old inventory copy.
                destination=dependency.name if dependency.name.startswith("DurinGame-") else "Plugins/Runtime/"+dependency.name
                dependencies.append(copy(dependency,destination))
            binaries.append(dict(Module=module["Name"],Owner=module["Owner"],Phase=module["Phase"],Path=relative,RuntimeDependencies=dependencies))
        # Only the runtime project view/configuration is deployed; no source module trees.
        descriptor.pop("$schema",None);descriptor.pop("NativeTestRoot",None)
        if "ExtraModules" in descriptor:descriptor["ExtraModules"]={k:v for k,v in descriptor["ExtraModules"].items() if k=="DurinGame"}
        for mount in descriptor.get("Mounts",[]):
            mount["Root"]="CookMounts/"+mount["VirtualRoot"].strip("/");mount["ContentPath"]=".";mount.pop("AuthoringWritable",None);mount["ContentWritable"]=False
        project_relative="Project/"+descriptor["ProjectName"]+".dproject"
        target=contained(runtime,project_relative);target.parent.mkdir(parents=True,exist_ok=True);target.write_text(json.dumps(descriptor,indent=2),encoding="utf-8")
        (target.parent/"Content").mkdir(exist_ok=True)
        for mount in snapshot["Mounts"]:contained(runtime,mount["VirtualRoot"].strip("/")).mkdir(parents=True,exist_ok=True)
        configuration=[]
        for name in ("Project.yaml","DurinGame.yaml"):
            relative="Project/Configs/"+name;file=contained(runtime,relative);file.parent.mkdir(parents=True,exist_ok=True);file.write_text(snapshot[name],encoding="utf-8");configuration.append(relative)
        # Asset entries stay solely in CMNF. Admission verifies module/configuration and Cook identity.
        required={project_relative,"CookManifest.bin","PluginCook.json",*configuration}
        if (runtime/"Shaders/ShaderLibrary.dslb").is_file():required.add("Shaders/ShaderLibrary.dslb")
        for binary in binaries:required.add(binary["Path"]);required.update(binary["RuntimeDependencies"])
        files=[dict(Path=p,Sha256=sha256(contained(runtime,p))) for p in sorted(required)]
        manifest=dict(SchemaVersion=1,Identity=expected,ProjectFile=project_relative,Plugins=[p["Id"] for p in frozen["Plugins"]],
            Binaries=binaries,Mounts=snapshot["Mounts"],ConfigurationFiles=configuration,Files=files)
        (runtime/"PluginRuntime.json").write_text(json.dumps(manifest,indent=2),encoding="utf-8")
        # A relocatable entry command supplies the explicit project path without source lookup.
        launcher='@echo off\n"%~dp0'+str(runtime_relative).replace('/','\\')+'\\DurinGame.exe" --project="%~dp0'+str(runtime_relative).replace('/','\\')+'\\'+project_relative.replace('/','\\')+'" %*\n'
        (payload/"RunGame.bat").write_text(launcher,encoding="utf-8")
        if before_publish:before_publish(payload)
        if output.exists():os.replace(output,backup)
        try:os.replace(payload,output)
        except BaseException:
            if backup.exists():os.replace(backup,output)
            raise
        return dict(status="succeeded",output=str(output),runtime=str(output/runtime_relative),project=project_relative,plugins=manifest["Plugins"],files=len(copies))
    finally:
        assert temporary.parent==output.parent and temporary.name.startswith(output.name+".stage-")
        shutil.rmtree(temporary)
        lock.rmdir()


def run(namespace,*,repository_root,repository_context,stdout,stderr,**_kwargs):
    repository=repository_context
    host=select_runtime(repository,profile_name=str(namespace.profile or ""),preset_name=str(namespace.preset or ""))
    project=resolve_project(repository,namespace.project_path)
    cook=(repository.root/namespace.cook_dir).resolve();output=(repository.root/namespace.output_dir).resolve()
    native=invoke_runtime_program(host,ExecutableDescription("Asset tool","DurinAssetTool","DurinAssetTool"),
        ["stage-info",f"--project={project}",f"--output={cook}","--json"],
        output=BuildOutput(color="never",output_mode=OutputMode.COMPACT,stdout=io.StringIO(),stderr=stderr),
        policy=RuntimeProcessPolicy(interruption_message="Stage preflight cancelled",capture_output=True))
    info=json.loads(native);snapshot=json.loads(info["Snapshot"])
    presets=load_configure_presets(host.paths.preset_file)
    candidates=[p for p in presets.values() if p.name in host.profile.presets
        and preset_cache_string(p,"DURIN_RUNTIME_VARIANT",required=False)=="DurinGame"
        and preset_cache_string(p,"CMAKE_BUILD_TYPE",required=False)==snapshot["Configuration"]]
    if namespace.game_preset:
        candidates=[p for p in candidates if p.name==namespace.game_preset]
    if len(candidates)!=1:raise DevToolError("Select one matching --game-preset")
    game=select_runtime(repository,profile_name=host.profile.name,preset_name=candidates[0].name)
    runtime=locate_executable(game,ExecutableDescription("Game","DurinLauncher")).parent
    inventory=preset_build_directory(game.preset,root=repository.root)/"PluginBuild"/json.loads(snapshot["ProjectDescriptor"])["ProjectName"]/preset_output_configuration(game.preset)/"PluginBuild.json"
    try:report=stage_game(repository.root,project,cook,output,runtime,inventory,info)
    except (OSError,ValueError,KeyError,TypeError) as error:raise DevToolError(f"Game staging failed: {error}") from error
    if namespace.output_format=="json":print(json.dumps(report),file=stdout)
    else:print(f"Staged Game: {output} ({len(report['plugins'])} plugin(s))",file=stdout)
    return 0
