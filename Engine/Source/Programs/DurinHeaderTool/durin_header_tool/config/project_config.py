from pathlib import Path
from dataclasses import dataclass, field
from durin_header_tool.io.json_helper import load_json_file, dataclass_from_dict

# Project configurations cached in memory, keyed by project name
PROJECT_CONFIGS: dict[str, "DurinProjectConfig"] = {}

@dataclass
class DurinProjectRuntimeVariantConfig:
    modules: list[str] = field(default_factory=list)


@dataclass
class DurinProjectConfig:
    project_name: Path = Path("")
    project_dir: Path = Path("")
    config_file_path: Path = Path("")
    base_modules: list[str] = field(default_factory=list)
    extra_modules: dict[str, DurinProjectRuntimeVariantConfig] = field(default_factory=dict)
    module_dirs: dict[str, str] = field(default_factory=dict) # module name -> module dir path relative to project dir
    modules: dict[str, str] = field(default_factory=dict) # module name -> module config file path relative to project dir

    def __post_init__(self):
        self.modules.clear()
        for module_name, module_dir in self.module_dirs.items():
            self.modules[module_name] = module_dir + "/" + module_name + ".dmodule"
        if not self.base_modules:
            self.base_modules = list(self.module_dirs.keys())

    def get_enabled_root_modules(self, runtime_variant: str) -> list[str]:
        enabled_root_modules = list(self.base_modules)
        runtime_variant_config = self.extra_modules.get(runtime_variant)
        if runtime_variant_config is not None:
            enabled_root_modules.extend(runtime_variant_config.modules)

        deduplicated_roots: list[str] = []
        seen_modules: set[str] = set()
        for module_name in enabled_root_modules:
            if module_name in seen_modules:
                continue
            if module_name not in self.modules:
                raise ValueError(
                    f"Project '{self.project_name}' does not define root module "
                    f"'{module_name}' for runtime variant '{runtime_variant}'."
                )
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
        return instance

def is_project_config_loaded(project_name: str) -> bool:
    return project_name in PROJECT_CONFIGS

def _load_project_config_file(project_config_file_path: Path) -> DurinProjectConfig:
    return DurinProjectConfig.from_file(project_config_file_path)

def load_project_config_file(project_config_file_path: Path) -> DurinProjectConfig:
    project_config = _load_project_config_file(project_config_file_path)
    existing = PROJECT_CONFIGS.get(project_config.project_name)
    if existing is not None and existing.config_file_path != project_config.config_file_path:
        raise ValueError(
            f"Duplicate project name '{project_config.project_name}' in "
            f"'{existing.config_file_path}' and '{project_config.config_file_path}'."
        )
    PROJECT_CONFIGS[project_config.project_name] = project_config
    return project_config

def get_project_config(project_name: str) -> DurinProjectConfig:
    if project_name in PROJECT_CONFIGS:
        return PROJECT_CONFIGS[project_name]
    raise ValueError(f"Project '{project_name}' was not supplied to DHT.")

# return the owning project name for a given module name, by searching through all loaded project configs
def find_module(module_name: str) -> str:
    for project_name, project_config in PROJECT_CONFIGS.items():
        if module_name in project_config.modules:
            return project_name
    raise ValueError(f"Module '{module_name}' not found in any project.")
