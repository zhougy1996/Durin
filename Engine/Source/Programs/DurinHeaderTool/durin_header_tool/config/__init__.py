from . import environment
from .environment import (
    ARCH,
    BUILD_CONFIG,
    BUILD_IDENTIFIER,
    DHT_ROOT_DIR,
    DURIN_ENGINE_PROJECT_DIR,
    DURIN_ROOT_DIR,
    PROFILE_NAME,
    init_clang,
)
from .module_config import (
    DurinModuleConfig,
    collect_all_dependent_module_export_files,
    collect_all_dependent_module_with_export_file,
    collect_all_dependent_modules,
    collect_enabled_modules_for_project,
    collect_sorted_dependent_modules,
    get_module_config,
    is_module_enabled_for_active_profile,
)
from .profile_config import DurinProfileConfig, get_profile_config
from .project_config import (
    DurinProjectConfig,
    DurinProjectProfileConfig,
    find_module,
    get_project_config,
    is_project_config_loaded,
    load_project_config_file,
)

def init_configs(project_files=()):
    engine_project_file = environment.DURIN_ENGINE_PROJECT_DIR / "Engine.dproject"
    load_project_config_file(engine_project_file)
    for project_file in project_files:
        load_project_config_file(project_file)
