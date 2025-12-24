import sys
import os
import logging
import globals as g

from dproject import DogeProjectConfig, DogeModuleConfig, load_dproject_config, load_module_config


def parse_header(header_filepath: str):
    pass

def generate_files(header_data, output_dir: str):
    pass

def setup_environment(arch: str, build_mode: str) -> None:
    g.arch = arch
    g.build_mode = build_mode

def run(dproject_filepath: str, module_name: str) -> None:
    try:
        project = load_dproject_config(dproject_filepath)
    except Exception as e:
        logging.error(f"Failed to load project file {dproject_filepath}: {e}")
        return
    
    dmodule_filepath = project.modules.get(module_name, "")
    if not dmodule_filepath:
        logging.error(f"Module {module_name} not found in project {project.name}.")
        return
    
    module_info = load_module_config(dmodule_filepath)

    module_dir = project.get_module_dir(module_name)
    module_dht_dir = project.get_module_dht_dir(module_name)
    os.makedirs(module_dht_dir, exist_ok=True)

    for header in module_info.dht_headers:
        header_path = os.path.join(module_dir, header)
        if not os.path.isfile(header_path):
            logging.warning(f"Header file {header_path} does not exist. Skipping.")
            continue
        
        header_data = parse_header(header_path)
        generate_files(header_data, module_dht_dir)

