"""Publish a completed native availability receipt; never infer products from directories."""
import argparse
import hashlib
import json
from pathlib import Path


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def publish(inputs, workspace, artifacts, project, catalog, output):
    # Retire the old success marker before processing. A failed publication cannot
    # leave a success receipt for this invocation.
    output.unlink(missing_ok=True)
    lines = inputs.read_text(encoding='utf-8-sig').splitlines()
    engine, platform, arch, config, variant, abi, name = lines[:7]
    if platform != 'Win64' or variant != 'DurinGame' or config not in ('Debug', 'Release'):
        raise ValueError('Unsupported receipt target')
    roots = {'Engine': workspace / 'Engine', 'Project': project.parent, 'Artifacts': artifacts, 'Dependencies': (workspace/'Engine/External').resolve(), 'Workspace': workspace}
    def source(path, host_input=False):
        path = path.resolve(strict=True)
        for root, base in roots.items():
            if root == "Workspace" and not host_input:
                continue
            if path.is_relative_to(base.resolve()):
                return dict(Root=root, Path=path.relative_to(base.resolve()).as_posix())
        if host_input:
            return dict(Root='Host', Path=path.as_posix())
        raise ValueError('Receipt source is outside declared roots: ' + str(path))
    modules, products, captured, destinations = {}, [], {}, set()
    crt_minimum = None
    cook_compatibility = None
    for line in lines[7:]:
        fields = line.split('\t')
        if fields[0] == 'crt':
            crt_minimum = fields[1]
        elif fields[0] == 'compat':
            cook_compatibility = fields[1]
        elif fields[0] == 'module':
            _, module, owner, linked, dynamic = fields
            if module in modules:
                raise ValueError('Colliding normalized target identity: ' + module)
            modules[module] = dict(Name=module, Owner=owner, Host='Runtime', Linked=sorted(set(filter(None, linked.split(';')))),
                                   Dynamic=sorted(set(filter(None, dynamic.split(';')))), RuntimeFiles=[])
        elif fields[0] in ('product', 'file'):
            _, module, *values = fields
            if fields[0] == 'product':
                kind, path, destination, license, provenance = values
            else:
                path, destination, kind, license, provenance = values
            if kind not in ('Executable', 'NativeLibrary', 'RuntimeData', 'License'):
                raise ValueError('Unknown runtime file category')
            if destination.casefold() in destinations or not destination or Path(destination).is_absolute() or any(v in destination for v in ('..', '\\', ':')):
                raise ValueError('Colliding/invalid deployment destination: ' + destination)
            destinations.add(destination.casefold())
            record = dict(Owner=modules[module]['Owner'], Module=module, Host='Runtime', Source=source(Path(path)),
                          Destination=destination, Kind=kind, Sha256=sha(Path(path).read_bytes()))
            if license or provenance:
                if not license or not provenance:
                    raise ValueError('Redistributed files require both license and provenance')
                record.update(License=license, Provenance=provenance)
            products.append(record)
            modules[module]['RuntimeFiles'].append(destination)
        elif fields[0] == 'input':
            path = Path(fields[1]); ref = source(path, host_input=True)
            captured[canonical(ref)] = dict(**ref, Sha256=sha(path.read_bytes()))
        else:
            raise ValueError('Unknown build input record')
    for module in modules.values():
        if any(edge not in modules for edge in module['Linked'] + module['Dynamic']):
            raise ValueError('Missing declared module edge')
        module['RuntimeFiles'].sort()
    spec_ref = source(inputs)
    captured[canonical(spec_ref)] = dict(**spec_ref, Sha256=sha(inputs.read_bytes()))
    catalog_data = json.loads(catalog.read_text(encoding='utf-8')) if catalog and catalog.is_file() else {'Modules': [], 'Inputs': []}
    # The completed receipt is also the exporter's plugin compatibility authority.
    for entry in catalog_data['Modules']:
        module = modules[entry['Name']]
        if module['Owner'] != entry['Owner'] or entry['Host'] != 'Runtime':
            raise ValueError('Plugin module ownership/host differs from the native build')
        binary = next(p for p in products if p['Module'] == entry['Name'] and p['Kind'] == 'NativeLibrary')
        module.update(Binary=binary['Destination'], Phase=entry['Phase'])
    # Build inventory is a superset. Startup-only overrides never enter these inputs.
    selection = dict(Modules=catalog_data['Modules'], Inputs=catalog_data['Inputs'])
    input_records = sorted(captured.values(), key=lambda x: (x['Root'], x['Path']))
    if not cook_compatibility:
        raise ValueError("Missing Cook schema/provider compatibility")
    if not crt_minimum:
        raise ValueError('Missing actual CRT toolset requirement')
    core = next(p for p in products if p['Module'] == 'Core' and p['Kind'] == 'NativeLibrary')
    receipt = dict(SchemaVersion=1, Status='succeeded', Identity=dict(EngineBuild=engine+':'+core['Sha256'],
        Project=name, Target='DurinLauncher', Platform=platform, Architecture='x64' if arch in ('AMD64','x86_64','x64') else arch,
        Configuration=config, RuntimeVariant=variant, ToolchainABI=abi),
        CookCompatibility=cook_compatibility, Inputs=input_records, BuildInputs=catalog_data["Inputs"], InputFingerprint=sha(''.join(x['Root']+'/'+x['Path']+':'+x['Sha256']+'\n' for x in input_records).encode()),
        SelectionFingerprint=sha(canonical(selection).encode()),
        Products=sorted(products, key=lambda x:x['Destination']), Modules=[modules[n] for n in sorted(modules)],
        Prerequisites=dict(SystemPolicy='win64-system-v1', CRT='developer-debug-crt' if config=='Debug' else 'vcredist-x64',
            MinimumVersion=crt_minimum, Provenance=abi, License='Microsoft Visual Studio runtime terms'))
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_suffix('.tmp'); temp.write_text(canonical(receipt), encoding='utf-8'); temp.replace(output)
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for flag in ('inputs', 'workspace', 'artifacts', 'project', 'catalog', 'output'):
        parser.add_argument('--'+flag, type=Path, required=True)
    args = parser.parse_args()
    publish(**vars(args))
