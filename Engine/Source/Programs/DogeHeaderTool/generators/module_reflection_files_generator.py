import os

import configs
import utils
from models import ModuleManifest, load_module_manifest_file, save_module_manifest_file
from utils.file_utils import FileFingerprint

# Empty Implementation for generating reflection files for a module. This is used to create empty reflection files for modules to make sure the build system can find them, and to test the integration of the reflection file generation into the build process. The actual implementation of parsing headers and generating reflection files will be added later.
def _generate_empty_reflection_files(module_name, headers_to_regenerate):
    empty_cpp_content = "// This is an empty generated reflection source file.\n"
    empty_h_content = "// This is an empty generated reflection header file.\n"
    output_dir = utils.get_module_dht_output_dir(module_name)

    for header in headers_to_regenerate:
        header_filename = os.path.splitext(os.path.basename(header))[0]
        reflection_header_file = output_dir / f"{header_filename}.gen.h"
        reflection_source_file = output_dir / f"{header_filename}.gen.cpp"
        utils.generate_file(reflection_header_file, empty_h_content)
        utils.generate_file(reflection_source_file, empty_cpp_content)

    empty_module_cpp_content = "// This is an empty generated module source file for reflection.\n"
    utils.generate_file(output_dir / f"{module_name}.module.gen.cpp", empty_module_cpp_content)

def get_reflection_headers_requiring_regeneration(old_manifest: ModuleManifest, new_manifest: ModuleManifest) -> list[str]:
    if old_manifest is None:
        # If there is no existing manifest, we need to generate reflection files for all headers
        return list(new_manifest.reflect_headers.keys())
    
    # compare dependent module export file fingerprints
    for dep_module, new_fingerprint in new_manifest.dep_module_exports.items():
        old_fingerprint = old_manifest.dep_module_exports.get(dep_module)
        if old_fingerprint != new_fingerprint:
            # If the export file of a dependent module has changed, we need to regenerate reflection files for all headers in this module, since changes in the export file of a dependent module could potentially affect the generated reflection code for all headers in this module.
            return list(new_manifest.reflect_headers.keys())

    headers_requiring_regeneration = []
    for header, new_fingerprint in new_manifest.reflect_headers.items():
        old_fingerprint = old_manifest.reflect_headers.get(header)
        if old_fingerprint != new_fingerprint:
            headers_requiring_regeneration.append(header)
    return headers_requiring_regeneration

# Reuse the fingerprint for headers from the existing manifest for unchanged headers to avoid unnecessary regeneration of reflection files for those headers
def make_new_module_manifest(module_name: str, old_manifest: ModuleManifest = None) -> ModuleManifest:
    manifest = ModuleManifest(module_name=module_name)
    dependent_modules = configs.collect_all_dependent_modules_for_manifest(module_name)

    for dep_module in dependent_modules:
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for dependent module '{dep_module}' not found at expected path: {export_file_path}")
        manifest.dep_module_exports[dep_module] = utils.get_light_file_fingerprint(export_file_path)

    for header in configs.get_module_config(module_name).reflect_headers:
        header_file_path = (configs.get_module_config(module_name).module_dir / header).resolve()
        if not header_file_path.exists():
            raise FileNotFoundError(f"Reflect header file '{header}' for module '{module_name}' not found at expected path: {header_file_path}")
        
        old_header_fingerprint = old_manifest.reflect_headers.get(header) if old_manifest else None
        manifest.reflect_headers[header] = utils.get_file_fingerprint_with_old_cache(header_file_path, old_header_fingerprint)

    return manifest

def generate_reflection_files(module_name):
    manifest_file_path = utils.get_module_manifest_file_path(module_name)
    old_manifest: ModuleManifest = load_module_manifest_file(module_name) if manifest_file_path.exists() else None
    new_manifest = make_new_module_manifest(module_name, old_manifest)
    headers_to_regenerate = get_reflection_headers_requiring_regeneration(old_manifest, new_manifest)
    _generate_empty_reflection_files(module_name, headers_to_regenerate)
    save_module_manifest_file(new_manifest)
