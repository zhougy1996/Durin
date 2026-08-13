from . import environment
from .environment import (
    ARCH,
    DHT_ROOT_DIR,
    DURIN_ENGINE_PROJECT_DIR,
    DURIN_ROOT_DIR,
    RUNTIME_VARIANT,
    TOOL_FINGERPRINT,
)
from .module_config import (
    DurinModuleConfig,
    collect_all_dependent_module_export_files,
    collect_all_dependent_module_with_export_file,
    collect_all_dependent_modules,
    collect_enabled_modules_for_project,
    get_module_config,
    is_module_enabled_for_active_runtime_variant,
)
from .runtime_variant_config import DurinRuntimeVariantConfig, get_runtime_variant_config
from .project_config import (
    DurinProjectConfig,
    DurinProjectRuntimeVariantConfig,
    find_module,
    get_project_config,
    load_project_config_file,
)

def init_configs(project_files=()):
    engine_project_file = environment.DURIN_ENGINE_PROJECT_DIR / "Engine.dproject"
    load_project_config_file(engine_project_file)
    for project_file in project_files:
        load_project_config_file(project_file)
