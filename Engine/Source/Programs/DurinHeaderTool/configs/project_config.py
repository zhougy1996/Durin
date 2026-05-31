from pathlib import Path
from dataclasses import dataclass, field
from typing import Dict
from utils.json_utils import load_json_file, dataclass_from_dict

# project name -> project config file path, loaded from the registered projects JSON file
REGISTERED_DURIN_PROJECTS: Dict[str, Path] = {} 
# Project configurations cached in memory, keyed by project name
PROJECT_CONFIGS: Dict[str, "DurinProjectConfig"] = {}

@dataclass
class DurinProjectProfileConfig:
    modules: list[str] = field(default_factory=list)


@dataclass
class DurinProjectConfig:
    project_name: Path = Path("")
    project_dir: Path = Path("")
    config_file_path: Path = Path("")
    base_modules: list[str] = field(default_factory=list)
    extra_modules: dict[str, DurinProjectProfileConfig] = field(default_factory=dict)
    module_dirs: dict[str, str] = field(default_factory=dict) # module name -> module dir path relative to project dir
    modules: dict[str, str] = field(default_factory=dict) # module name -> module config file path relative to project dir

    def __post_init__(self):
        self.modules.clear()
        for module_name, module_dir in self.module_dirs.items():
            self.modules[module_name] = module_dir + "/" + module_name + ".dmodule"
        if not self.base_modules:
            self.base_modules = list(self.module_dirs.keys())

    def get_enabled_root_modules(self, profile_name: str) -> list[str]:
        enabled_root_modules = list(self.base_modules)
        profile_config = self.extra_modules.get(profile_name)
        if profile_config is not None:
            enabled_root_modules.extend(profile_config.modules)

        deduplicated_roots: list[str] = []
        seen_modules: set[str] = set()
        for module_name in enabled_root_modules:
            if module_name in seen_modules:
                continue
            if module_name not in self.modules:
                raise ValueError(f"Project '{self.project_name}' does not define root module '{module_name}' for profile '{profile_name}'.")
            seen_modules.add(module_name)
            deduplicated_roots.append(module_name)
        return deduplicated_roots

    @classmethod
    def from_file(cls, project_config_file_path: Path) -> "DurinProjectConfig":
        project_config_file_path = project_config_file_path.resolve()
        
        raw_json_data = load_json_file(project_config_file_path, required_fields=["ProjectName"])

        instance = dataclass_from_dict(cls, raw_json_data)
        instance.config_file_path = project_config_file_path
        instance.project_dir = instance.config_file_path.parent
        instance.__post_init__()
        return instance

# Prepares the registered project config file paths by loading them from the JSON file and caching them in memory    
def prepare_registered_project_config_file_paths() -> None:
    from configs.base_config import DURIN_ENGINE_PROJECT_DIR, DURIN_PROJECT_REGISTER_FILE_PATH, DURIN_ROOT_DIR
    REGISTERED_DURIN_PROJECTS["Engine"] = DURIN_ENGINE_PROJECT_DIR / "Engine.dproject"
    # Load the registered projects from the JSON file
    if DURIN_PROJECT_REGISTER_FILE_PATH.exists():
        registered_projects_data = load_json_file(DURIN_PROJECT_REGISTER_FILE_PATH, required_fields=["Projects"])
        for project_name, project_path in registered_projects_data["Projects"].items():
            registered_project_path = Path(project_path)
            if not registered_project_path.is_absolute():
                registered_project_path = DURIN_ROOT_DIR / registered_project_path
            REGISTERED_DURIN_PROJECTS[project_name] = registered_project_path

def is_project_config_registered(project_name: str) -> bool:
    return project_name in REGISTERED_DURIN_PROJECTS

def is_project_config_loaded(project_name: str) -> bool:
    return project_name in PROJECT_CONFIGS

def _load_project_config_file(project_config_file_path: Path) -> DurinProjectConfig:
    project_config = DurinProjectConfig.from_file(project_config_file_path)
    PROJECT_CONFIGS[project_config.project_name] = project_config
    return project_config

def _load_project_config(project_name: str) -> DurinProjectConfig:
    if project_name in REGISTERED_DURIN_PROJECTS:
        return _load_project_config_file(REGISTERED_DURIN_PROJECTS[project_name])
    raise ValueError(f"Project '{project_name}' is not registered.")

def get_project_config(project_name: str) -> DurinProjectConfig:
    if project_name in PROJECT_CONFIGS:
        return PROJECT_CONFIGS[project_name]
    return _load_project_config(project_name)


def get_registered_project_names() -> list[str]:
    return list(REGISTERED_DURIN_PROJECTS.keys())

# return the owning project name for a given module name, by searching through all loaded project configs
def find_module(module_name: str) -> str:
    for project_name, project_config in PROJECT_CONFIGS.items():
        if module_name in project_config.modules:
            return project_name
    raise ValueError(f"Module '{module_name}' not found in any project.")
