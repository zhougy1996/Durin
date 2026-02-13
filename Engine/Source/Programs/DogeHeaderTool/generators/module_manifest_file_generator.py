import json
import configs
import utils
from pathlib import Path
from models.module_manifest import ModuleManifest

def generate_module_manifest_file(module_name: str) -> None:
    output_path = utils.get_module_manifest_file_path(module_name)
    manifest = ModuleManifest(module_name=module_name)

    dependent_modules = list(configs.collect_all_dependent_modules(module_name))
    dependent_modules.append(module_name)
    dependent_modules.sort()  # Sort to ensure consistent order

    for dep_module in dependent_modules:
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for dependent module '{dep_module}' not found at expected path: {export_file_path}")
        manifest.dep_module_exports[dep_module] = utils.get_file_fingerprint(export_file_path)

    for header in configs.get_module_config(module_name).reflect_headers:
        header_file_path = (configs.get_module_config(module_name).module_dir / header).resolve()
        if not header_file_path.exists():
            raise FileNotFoundError(f"Reflect header file '{header}' for module '{module_name}' not found at expected path: {header_file_path}")
        manifest.reflect_headers[header] = utils.get_file_fingerprint(header_file_path)

    json_data = utils.dict_from_dataclass(ModuleManifest, manifest, auto_convert=True)
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
