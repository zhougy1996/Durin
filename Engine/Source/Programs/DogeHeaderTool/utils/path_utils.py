from pathlib import Path
import config

def get_dht_root_dir() -> Path:
    return config.base_config.DHT_ROOT_DIR

def get_libclang_path() -> str:
    return config.base_config.LIBCLANG_PATH

def get_project_config_path(project_name: str) -> str:
    if project_name not in config.project_config.PROJECT_CONFIGS:
        raise ValueError(f"Project '{project_name}' not found in PROJECT_CONFIGS.")
    return config.project_config.PROJECT_CONFIGS[project_name].config_file_path

def get_module_config_path(module_name: str) -> str:
    if module_name not in config.module_config.MODULE_CONFIGS:
        raise ValueError(f"Module '{module_name}' not found in MODULE_CONFIGS.")
    return config.module_config.MODULE_CONFIGS[module_name].config_file_path

