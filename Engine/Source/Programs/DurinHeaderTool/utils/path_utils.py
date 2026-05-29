from pathlib import Path
import configs

def get_dht_tool_dir() -> Path:
    return configs.base_config.DHT_ROOT_DIR

def get_project_dir(project_name: str) -> Path:
    project_config = configs.get_project_config(project_name)
    return project_config.project_dir

def get_project_source_dir(project_name: str) -> Path:
    return get_project_dir(project_name) / "Source"

def get_project_intermediate_dir(project_name: str) -> Path:
    return get_project_dir(project_name) / "Intermediate"

def get_project_binary_dir(project_name: str) -> Path:
    return get_project_dir(project_name) / "Binaries"

def get_project_config_dir(project_name: str) -> Path:
    return get_project_dir(project_name) / "Configs"

def get_project_cmake_dir(project_name: str) -> Path:
    return get_project_dir(project_name) / "CMake"

def get_project_profile_dir(project_name: str) -> Path:
    return get_project_dir(project_name) / "Profiles"

def get_project_intermediate_build_dir(project_name: str) -> Path:
    return get_project_intermediate_dir(project_name) / "Build" / configs.ARCH / configs.PROFILE_NAME

def get_project_cmake_file_path(project_name: str) -> Path:
    return get_project_intermediate_build_dir(project_name) / f"{project_name}.project.cmake"

def get_module_intermediate_build_dir(module_name: str) -> Path:
    project_name = configs.get_module_config(module_name).owning_project
    return get_project_intermediate_build_dir(project_name) / module_name

def get_module_cmake_file_path(module_name: str) -> Path:
    return get_module_intermediate_build_dir(module_name) / f"{module_name}.module.cmake"

def get_module_definitions_header_path(module_name: str) -> Path:
    return get_module_intermediate_build_dir(module_name) / "Definitions.h"

def get_module_dht_output_dir(module_name: str) -> Path:
    return get_module_intermediate_build_dir(module_name) / "DHT"

def get_module_export_file_path(module_name: str) -> Path:
    if not configs.get_module_config(module_name).has_export_file():
        return Path("")
    return get_module_dht_output_dir(module_name) / f"{module_name}.export"
    
def get_module_manifest_file_path(module_name: str) -> Path:
    return get_module_dht_output_dir(module_name) / f"{module_name}.manifest"
