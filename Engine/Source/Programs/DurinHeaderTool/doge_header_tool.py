import sys
import os
import logging

from durin_project import DurinProjectConfig, DurinModuleConfig, load_project_config, load_module_config, get_all_dependent_modules
import durin_globals as g
import durin_parser as parser
import durin_generator as generator 
from durin_exports import ModuleManifest
import durin_type_database as db

def generate_module_manifest_file(dproject: DurinProjectConfig, module_name: str) -> None:
    dmodule_filepath = dproject.modules.get(module_name)
    module_info = load_module_config(dmodule_filepath) if dmodule_filepath else None
    if not module_info:
        logging.error(f"Module {module_name} not found in project {dproject.name}.")
        return None
    module_dir = dproject.get_module_dir(module_name)
    output_dir = dproject.get_module_dht_dir(module_name)
    module_manifest = ModuleManifest(module_name)

    module_manifest.exports = collect_module_exports(module_dir, module_info)
    module_manifest.all_dependencies = [mod.name for mod in get_all_dependent_modules(dproject, module_name)]
    
    os.makedirs(output_dir, exist_ok=True)
    output_file = os.path.join(output_dir, f"{module_name}.module.manifest")
    generator.generate_module_manifest_file(module_manifest, output_file)

def collect_module_exports(module_dir: str, module_info: DurinModuleConfig) -> dict:
    exports = {}
    for header_file in module_info.reflect_headers:
        full_header_path = os.path.join(module_dir, header_file)
        if not os.path.isfile(full_header_path):
            logging.warning(f"Header file {full_header_path} does not exist. Skipping.")
            continue
        exports[header_file] = parser.collect_header_exports(full_header_path)

    return exports

def parse_header(header_filepath: str):
    pass

def generate_files(header_filepath: str, output_dir: str):
    # output empty files for now
    header_filename = os.path.splitext(os.path.basename(header_filepath))[0]
    reflection_header_file = os.path.join(output_dir, f"{header_filename}.gen.h")
    reflection_source_file = os.path.join(output_dir, f"{header_filename}.gen.cpp")

    gen_header_content = "// Generated reflection header file\n"
    gen_source_content = "// Generated reflection source file\n"
    generator.generate_file(reflection_header_file, gen_header_content)
    generator.generate_file(reflection_source_file, gen_source_content)
    

def setup_environment(arch: str, build_mode: str) -> None:
    g.ARCH = arch
    g.BUILD_MODE = build_mode

def generate_reflection_files(dproject_filepath: str, module_name: str) -> None:
    try:
        project = load_project_config(dproject_filepath)
    except Exception as e:
        logging.error(f"Failed to load project file {dproject_filepath}: {e}")
        return
    
    g.type_db = db.build_database(project.name, module_name)

    # TODO: maybe don't use global variable
    g.project_meta = project
    
    dmodule_filepath = project.modules.get(module_name, "")
    if not dmodule_filepath:
        logging.error(f"Module {module_name} not found in project {project.name}.")
        return
    
    module_info = load_module_config(dmodule_filepath)

    # TODO: maybe don't use global variable
    g.module_meta = module_info

    module_dir = project.get_module_dir(module_name)
    module_dht_dir = project.get_module_dht_dir(module_name)
    os.makedirs(module_dht_dir, exist_ok=True)
    
    # Generate reflection module source file
    reflection_module_source = os.path.join(module_dht_dir, f"{module_name}.module.gen.cpp")
    with open(reflection_module_source, "w") as f:
        f.write("// Generated reflection module source file\n")

    parser.init(module_info)

    for header in module_info.reflect_headers:
        header_path = os.path.join(module_dir, header)
        if not os.path.isfile(header_path):
            logging.warning(f"Header file {header_path} does not exist. Skipping.")
            continue
        
        # header_data = parse_header(header_path)
        generate_files(header, module_dht_dir)

