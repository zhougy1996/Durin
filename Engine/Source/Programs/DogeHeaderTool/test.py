import config

config.initialize()
engine_project_config = config.get_project_config("Engine")
core_module_config = config.get_module_config("Core")
engine_module_config = config.get_module_config("Engine")
print(engine_project_config)
print(core_module_config)
print(engine_module_config)