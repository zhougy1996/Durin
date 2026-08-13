from pathlib import Path
from dataclasses import dataclass, field
from durin_header_tool.io.json_helper import load_json_descriptor
from .project_config import get_project_config, find_module


MODULE_SCHEMA = "durin-module.schema.json"

# Stores all loaded module configurations
MODULE_CONFIGS: dict[str, "DurinModuleConfig"] = {}
ENABLED_MODULES: dict[tuple[str, str], set[str]] = {}

@dataclass
class DurinModuleConfig:
    module_name: str = ""
    link_type: str = "Shared"
    pch: str = "Self"
    module_dir: Path = Path("")
    config_file_path: Path = Path("")
    owning_project: str = ""
    private_dependencies: list[str] = field(default_factory=list)
    public_dependencies: list[str] = field(default_factory=list)
    optional_private_dependencies: list[str] = field(default_factory=list)
    optional_public_dependencies: list[str] = field(default_factory=list)
    reflect_headers: list[str] = field(default_factory=list)
    api_macro: str = ""

    def __post_init__(self):
        self.api_macro = f"{self.module_name.upper()}_API"

    @classmethod
    def from_file(cls, module_config_file_path: Path) -> "DurinModuleConfig":
        module_config_file_path = module_config_file_path.resolve()

        raw_json_data = load_json_descriptor(module_config_file_path, MODULE_SCHEMA)
        instance = cls(
            module_name=raw_json_data["ModuleName"],
            link_type=raw_json_data.get("LinkType", "Shared"),
            pch=raw_json_data.get("PCH", "Self"),
            private_dependencies=list(raw_json_data.get("PrivateDependencies", [])),
            public_dependencies=list(raw_json_data.get("PublicDependencies", [])),
            optional_private_dependencies=list(
                raw_json_data.get("OptionalPrivateDependencies", [])
            ),
            optional_public_dependencies=list(
                raw_json_data.get("OptionalPublicDependencies", [])
            ),
            reflect_headers=list(raw_json_data.get("ReflectHeaders", [])),
        )
        instance.config_file_path = module_config_file_path
        instance.module_dir = instance.config_file_path.parent
        return instance
    
    def has_export_file(self) -> bool:
        return len(self.reflect_headers) > 0
    
# Load the configuration for a module from a file, and caches it
def _load_module_config_file(module_config_file_path: Path, owning_project: str) -> DurinModuleConfig:
    module_config = DurinModuleConfig.from_file(module_config_file_path)
    module_config.owning_project = owning_project
    MODULE_CONFIGS[module_config.module_name] = module_config
    return module_config

# Load the configuration for a module, optionally within a specific project, and caches it
def _load_module_config(module_name: str) -> DurinModuleConfig:
    owning_project_name = find_module(module_name)
    project_config = get_project_config(owning_project_name)
    module_config_path = project_config.project_dir / project_config.modules[module_name]
    return _load_module_config_file(module_config_path, owning_project=owning_project_name)

# Retrieves the configuration for a module
def get_module_config(module_name: str) -> DurinModuleConfig:
    if module_name in MODULE_CONFIGS:
        module_config = MODULE_CONFIGS[module_name]
        return module_config
    return _load_module_config(module_name)

def collect_all_dependent_modules(module_name: str, runtime_variant: str | None = None, visited=None) -> set[str]:
    if runtime_variant is None:
        from durin_header_tool import config as configs
        runtime_variant = configs.RUNTIME_VARIANT

    if visited is None:
        visited = set()
    
    if module_name in visited:
        return set()  # Avoid circular dependencies
    
    visited.add(module_name)
    module_config = get_module_config(module_name)
    all_deps = set(module_config.private_dependencies + module_config.public_dependencies)

    # Optional dependencies do not enable modules. Once enabled independently by
    # the active runtime variant, however, they are part of the module's real build graph
    # and DHT must parse and track their reflection exports like ordinary deps.
    optional_deps = module_config.optional_private_dependencies + module_config.optional_public_dependencies
    all_deps.update(
        dep
        for dep in optional_deps
        if is_module_enabled_for_active_runtime_variant(dep, runtime_variant)
    )
    
    for dep in all_deps.copy():
        all_deps.update(collect_all_dependent_modules(dep, runtime_variant, visited))
    
    return all_deps


def collect_enabled_modules_for_project(project_name: str, runtime_variant: str) -> set[str]:
    cache_key = (project_name, runtime_variant)
    if cache_key in ENABLED_MODULES:
        return ENABLED_MODULES[cache_key]

    project_config = get_project_config(project_name)
    enabled_modules: set[str] = set()
    visited_modules: set[str] = set()

    def _visit(module_name: str) -> None:
        if module_name in visited_modules:
            return
        visited_modules.add(module_name)

        module_config = get_module_config(module_name)
        if module_config.owning_project == project_name:
            enabled_modules.add(module_name)

        for dep in module_config.private_dependencies + module_config.public_dependencies:
            _visit(dep)

    for root_module in project_config.get_enabled_root_modules(runtime_variant):
        _visit(root_module)

    ENABLED_MODULES[cache_key] = enabled_modules
    return enabled_modules


def is_module_enabled_for_active_runtime_variant(
    module_name: str,
    runtime_variant: str | None = None,
) -> bool:
    if runtime_variant is None:
        from durin_header_tool import config as configs
        runtime_variant = configs.RUNTIME_VARIANT
    owning_project = get_module_config(module_name).owning_project
    return module_name in collect_enabled_modules_for_project(owning_project, runtime_variant)

# Collects all the dependencies of the module manifest file that have export files (self included).
def collect_all_dependent_module_with_export_file(module_name: str, runtime_variant: str | None = None) -> list[str]:
    all_deps = collect_all_dependent_modules(module_name, runtime_variant)
    all_deps.add(module_name)  # Also include the module itself, since it is also a dependency for the manifest file
    deps_with_export_file = [dep for dep in all_deps if get_module_config(dep).has_export_file()]
    sorted_deps_with_export_file = sorted(deps_with_export_file)
    return sorted_deps_with_export_file

def collect_all_dependent_module_export_files(module_name: str, runtime_variant: str | None = None) -> list[Path]:
    from durin_header_tool.io import path_helper
    deps = collect_all_dependent_module_with_export_file(module_name, runtime_variant)
    return [path_helper.get_module_export_file_path(dep) for dep in deps]
