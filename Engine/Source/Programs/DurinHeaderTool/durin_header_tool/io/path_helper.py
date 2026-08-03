from pathlib import Path
from durin_header_tool import config as configs

def get_dht_tool_dir() -> Path:
    return configs.environment.DHT_ROOT_DIR

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

def get_project_intermediate_build_dir(project_name: str) -> Path:
    return get_project_intermediate_dir(project_name) / "Build" / configs.ARCH / configs.RUNTIME_VARIANT

def get_dht_output_lock_dir() -> Path:
    return (
        configs.environment.DURIN_ENGINE_PROJECT_DIR
        / "Intermediate"
        / "Build"
        / ".dht-locks"
        / configs.ARCH
        / configs.RUNTIME_VARIANT
    )


def get_dht_runtime_variant_lock_file_path() -> Path:
    """Return the lock reserved for runtime-variant-wide indexes and cleanup."""
    return get_dht_output_lock_dir() / "runtime-variant.lock"


def get_dht_project_lock_file_path(project_name: str) -> Path:
    """Return the lock for generated project metadata."""
    return get_dht_output_lock_dir() / "projects" / f"{project_name}.lock"


def get_dht_module_lock_file_path(module_name: str) -> Path:
    """Return the lock shared by all writers to one module output directory."""
    return get_dht_output_lock_dir() / "modules" / f"{module_name}.lock"

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

def get_module_dht_cache_root(module_name: str) -> Path:
    project_name = configs.get_module_config(module_name).owning_project
    return get_project_intermediate_build_dir(project_name) / "DHTCache"

def get_module_export_file_path(module_name: str) -> Path:
    if not configs.get_module_config(module_name).has_export_file():
        return Path("")
    return get_module_dht_output_dir(module_name) / f"{module_name}.export"

def get_module_export_manifest_file_path(module_name: str) -> Path:
    if not configs.get_module_config(module_name).has_export_file():
        return Path("")
    return get_module_dht_output_dir(module_name) / f"{module_name}.export.manifest"
    
def get_module_manifest_file_path(module_name: str) -> Path:
    return get_module_dht_output_dir(module_name) / f"{module_name}.manifest"
