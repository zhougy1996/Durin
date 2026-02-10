from .base_config import *
from .project_config import *
from .module_config import *

def initialize():
    prepare_registered_project_config_file_paths()
    get_project_config("Engine") # Preload the engine project config to ensure it's available when loading module configs that depend on it