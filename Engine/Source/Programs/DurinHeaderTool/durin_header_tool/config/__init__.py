from . import environment
from .environment import (
    ARCH,
    DHT_ROOT_DIR,
    DURIN_ENGINE_PROJECT_DIR,
    DURIN_PROJECT_REGISTER_FILE_PATH,
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
    get_registered_project_names,
    is_project_config_loaded,
    is_project_config_registered,
    prepare_registered_project_config_file_paths,
)

def init_configs():
    prepare_registered_project_config_file_paths()
    get_project_config("Engine") # Preload the engine project config to ensure it's available when loading module configs that depend on it
