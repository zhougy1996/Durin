"""Publish the exact selected native closure; never infer membership from DLL scans."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def publish(selection_file: Path, inputs: Path, output: Path):
    lines = inputs.read_text(encoding="utf-8").splitlines()
    identity = dict(zip(("EngineBuild", "Platform", "Architecture", "Configuration",
                         "RuntimeVariant", "ToolchainABI"), lines[:6]))
    boundary = lines.index("---")
    sources = dict(line.split("\t", 1) for line in lines[boundary + 1:])
    if "Core" in sources:
        identity["EngineBuild"] += ":" + hashlib.sha256(Path(sources["Core"]).read_bytes()).hexdigest()
    paths = {name: "Runtime/" + Path(path).name for name, path in sources.items()}
    if len(set(paths.values())) != len(paths):
        raise ValueError("Native dependency filenames collide in plugin build closure")
    for name, source in sources.items():
        target = output / paths[name]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    binaries = []
    for line in lines[6:boundary]:
        module, owner, dependencies = line.split("\t")
        binaries.append(dict(Module=module, Owner=owner, Path=paths[module],
                             RuntimeDependencies=sorted({paths[d] for d in dependencies.split(";") if d})))
    selection = json.loads(selection_file.read_text(encoding="utf-8"))
    manifest = dict(SchemaVersion=1, SelectionFingerprint=selection["Fingerprint"],
                    Identity=identity, Binaries=binaries)
    output.mkdir(parents=True, exist_ok=True)
    destination = output / "PluginBuild.json"
    temporary = destination.with_suffix(".tmp")
    temporary.write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    temporary.replace(destination)
    shutil.copy2(selection_file, output / "PluginSelection.json")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--selection", type=Path, required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    publish(args.selection, args.inputs, args.output)
