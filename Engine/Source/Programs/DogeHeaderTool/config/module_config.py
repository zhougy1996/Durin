import os
from pathlib import Path
from dataclasses import dataclass, field
from utils.json_utils import load_json_file, dataclass_from_dict
from config.project_config import PROJECT_CONFIGS

# Stores all loaded module configurations
MODULE_CONFIGS: dict[str, "DogeModuleConfig"] = {}

@dataclass
class DogeModuleConfig:
    module_name: str = ""
    module_dir: str = ""
    config_file_path: Path = None
    owning_project: str = ""
    private_dependencies: list = field(default_factory=list)
    public_dependencies: list = field(default_factory=list)
    reflect_headers: list = field(default_factory=list, metadata={"json_key": "DHTHeaders"})
    api_macro: str = ""

    def __post_init__(self):
        self.api_macro = f"{self.module_name.upper()}_API"

    @classmethod
    def from_file(cls, module_config_file_path) -> "DogeModuleConfig":
        module_config_file_path = os.path.abspath(module_config_file_path)
        
        raw_json_data = load_json_file(module_config_file_path, required_fields=["ModuleName"])

        instance = dataclass_from_dict(cls, raw_json_data)
        instance.config_file_path = module_config_file_path
        instance.module_dir = os.path.dirname(instance.config_file_path)
        instance.__post_init__()
        return instance

# Load the configuration for a module from a file, and caches it
def load_module_config_file(module_config_file_path: str, owning_project: str = "") -> DogeModuleConfig:
    module_config = DogeModuleConfig.from_file(module_config_file_path)
    if module_config:
        module_config.owning_project = owning_project
        MODULE_CONFIGS[module_config.module_name] = module_config
        return module_config
    raise ValueError(f"Module config file '{module_config_file_path}' could not be loaded.")

# Load the configuration for a module, optionally within a specific project, and caches it
def load_module_config(module_name: str, project_name: str = None) -> DogeModuleConfig:
    # If the project name is specified, load the module from that project
    if project_name:
        if project_name not in PROJECT_CONFIGS:
            raise ValueError(f"Project '{project_name}' not found.")
        project_config = PROJECT_CONFIGS[project_name]
        if module_name not in project_config.modules:
            raise ValueError(f"Module '{module_name}' not found in project '{project_name}'.")
        module_config_path = project_config.modules[module_name]
        if not module_config_path:
            raise ValueError(f"Module '{module_name}' in project '{project_name}' has empty config path.")
        return load_module_config_file(module_config_path, owning_project=project_name)
    
    # If no project name is specified, search all projects for the module
    for project_name, project_config in PROJECT_CONFIGS.items():
        if module_name in project_config.modules:
            return load_module_config_file(project_config.modules[module_name], owning_project=project_name)

    raise ValueError(f"Module '{module_name}' not found in any project.")

# Retrieves the configuration for a module
def get_module_config(module_name: str) -> DogeModuleConfig:
    if module_name in MODULE_CONFIGS:
        module_config = MODULE_CONFIGS[module_name]
        return module_config
    return load_module_config(module_name)
