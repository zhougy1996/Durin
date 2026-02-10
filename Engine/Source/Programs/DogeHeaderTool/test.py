import config
import utils

config.initialize()
engine_project_config = config.get_project_config("Engine")
core_module_config = config.get_module_config("Core")

non_existent_module_config = config.get_module_config("NonExistentModule")
