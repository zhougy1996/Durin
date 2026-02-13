from pathlib import Path
from dataclasses import dataclass, field
from utils.json_utils import load_json_file, dataclass_from_dict
from .project_config import get_project_config, find_module

# Stores all loaded module configurations
MODULE_CONFIGS: dict[str, "DogeModuleConfig"] = {}

@dataclass
class DogeModuleConfig:
    module_name: Path = Path("")
    module_dir: Path = Path("")
    config_file_path: Path = Path("")
    owning_project: str = ""
    private_dependencies: list = field(default_factory=list)
    public_dependencies: list = field(default_factory=list)
    reflect_headers: list = field(default_factory=list, metadata={"json_key": "DHTHeaders"})
    api_macro: str = ""

    def __post_init__(self):
        self.api_macro = f"{self.module_name.upper()}_API"

    @classmethod
    def from_file(cls, module_config_file_path: Path) -> "DogeModuleConfig":
        module_config_file_path = module_config_file_path.resolve()
        
        raw_json_data = load_json_file(module_config_file_path, required_fields=["ModuleName"])

        instance = dataclass_from_dict(cls, raw_json_data)
        instance.config_file_path = module_config_file_path
        instance.module_dir = instance.config_file_path.parent
        instance.__post_init__()
        return instance
    
    def get_reflect_header_paths(self) -> list[Path]:
        return [(self.module_dir / header).resolve() for header in self.reflect_headers]

# Load the configuration for a module from a file, and caches it
def _load_module_config_file(module_config_file_path: Path, owning_project: str) -> DogeModuleConfig:
    module_config = DogeModuleConfig.from_file(module_config_file_path)
    if module_config:
        module_config.owning_project = owning_project
        MODULE_CONFIGS[module_config.module_name] = module_config
        return module_config
    raise ValueError(f"Module config file '{module_config_file_path}' could not be loaded.")

# Load the configuration for a module, optionally within a specific project, and caches it
def _load_module_config(module_name: str) -> DogeModuleConfig:
    owning_project_name = find_module(module_name)
    if owning_project_name:
        project_config = get_project_config(owning_project_name)
        module_config_path = project_config.project_dir /  project_config.modules[module_name]
        return _load_module_config_file(module_config_path, owning_project=owning_project_name)

    raise ValueError(f"Module '{module_name}' not found in any project.")

# Retrieves the configuration for a module
def get_module_config(module_name: str) -> DogeModuleConfig:
    if module_name in MODULE_CONFIGS:
        module_config = MODULE_CONFIGS[module_name]
        return module_config
    return _load_module_config(module_name)

def collect_all_dependent_modules(module_name: str, visited=None) -> set[str]:
    if visited is None:
        visited = set()
    
    if module_name in visited:
        return set()  # Avoid circular dependencies
    
    visited.add(module_name)
    module_config = get_module_config(module_name)
    all_deps = set(module_config.private_dependencies + module_config.public_dependencies)
    
    for dep in all_deps.copy():
        all_deps.update(collect_all_dependent_modules(dep, visited))
    
    return all_deps

def collect_all_dependent_modules_for_manifest(module_name: str) -> list[str]:
    dependent_modules = collect_all_dependent_modules(module_name)
    dependent_modules.add(module_name)  # Also include the module itself, since its reflect headers are also dependencies for the manifest file
    return sorted(dependent_modules)
