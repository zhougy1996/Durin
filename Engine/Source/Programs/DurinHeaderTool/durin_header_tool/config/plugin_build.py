"""Bind selected project plugins to the existing module pipeline."""
from . import environment, project_config, module_config
from .plugin_config import enumerate_plugin_build, canonical

CATALOGS = {}
LOCATIONS = {}
DISCOVERY_DIRECTORIES = {}


def load_plugin_build_context():
    from durin_header_tool import config as configs
    projects = list(project_config.PROJECT_CONFIGS.values())
    CATALOGS.clear()
    LOCATIONS.clear()
    DISCOVERY_DIRECTORIES.clear()
    for name in list(module_config.MODULE_CONFIGS):
        if module_config.MODULE_CONFIGS[name].owning_plugin:
            del module_config.MODULE_CONFIGS[name]
    module_config.ENABLED_MODULES.clear()
    engine_root = environment.DURIN_ENGINE_PROJECT_DIR
    if (not (engine_root / "BuiltinPlugins.json").exists()
            and not any(p.plugin_dependencies or
                        (p.project_dir != engine_root and (p.project_dir / "Plugins").exists())
                        for p in projects)):
        return
    # Preserve ordinary descriptor schema diagnostics before graph enumeration.
    for project in projects:
        for relative in project.modules.values():
            module_config.DurinModuleConfig.from_file(project.project_dir / relative)
    files = [p.config_file_path for p in projects]
    for project in projects:
        watched = set()
        catalog, locations = enumerate_plugin_build(
            engine_root, project.config_file_path,
            platform=configs.ARCH, runtime_variant=configs.RUNTIME_VARIANT,
            project_files=files, discovery_directories=watched)
        CATALOGS[project.project_name] = catalog
        LOCATIONS[project.project_name] = locations
        DISCOVERY_DIRECTORIES[project.project_name] = watched
        for entry in catalog["Modules"]:
            name, owner = entry["Name"], entry["Owner"]
            path = locations[f"Plugin/{owner}/{name}"]
            plugin_file = locations[f"Plugin/{owner}"]
            owning_project = next(p.project_name for p in projects
                                  if plugin_file.is_relative_to(p.project_dir / "Plugins"))
            existing = next((m for m in module_config.MODULE_CONFIGS.values()
                             if m.module_name.casefold() == name.casefold()), None)
            if existing:
                if existing.config_file_path != path or existing.owning_plugin != owner:
                    raise ValueError(f"Conflicting workspace module {name}: {path}")
                continue
            module = module_config._load_module_config_file(path, owning_project)
            module.owning_plugin = owner
            module.plugin_host = entry["Host"]
            if module.link_type != "Shared":
                raise ValueError(f"{path}: native plugin modules require Shared linkage")
    module_config.ENABLED_MODULES.clear()


def append_plugin_cmake(content, project_name):
    from durin_header_tool import io as utils
    project_root = project_config.get_project_config(project_name).project_dir
    if project_root != environment.DURIN_ENGINE_PROJECT_DIR:
        # Detect creation even when the project has no plugin catalog yet.
        content.append(f'file(GLOB _durin_plugin_entries CONFIGURE_DEPENDS "{project_root.as_posix()}/Plugins")\n')
    for directory in sorted(DISCOVERY_DIRECTORIES.get(project_name, ())):
        content.append(f'file(GLOB _durin_plugin_entries CONFIGURE_DEPENDS "{directory.as_posix()}/*")\n')
    content.append('set(DURIN_PROJECT_PLUGIN_MODULES)\n')
    catalog = CATALOGS.get(project_name)
    if catalog is None:
        return
    catalog_path = utils.get_project_intermediate_build_dir(project_name) / "PluginCatalog.json"
    utils.generate_file(catalog_path, canonical(catalog))
    for path in LOCATIONS[project_name].values():
        content.append(f'set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "{path.as_posix()}")\n')
    content.append(f'durin_register_plugin_catalog("{project_name}" "{catalog_path.as_posix()}")\n')
    for module in sorted(module_config.MODULE_CONFIGS.values(), key=lambda m: m.module_name):
        if module.owning_project != project_name or not module.owning_plugin:
            continue
        path = utils.get_module_cmake_file_path(module.module_name)
        content.append(f'list(APPEND DURIN_PROJECT_PLUGIN_MODULES {module.module_name})\n')
        content.append(f'set(DURIN_MODULE_CMAKE_FILE_{module.module_name} "{path.as_posix()}")\n')
        content.append(f'set(DURIN_PLUGIN_SOURCE_{module.module_name} "{module.module_dir.as_posix()}")\n')
        content.append(f'set(DURIN_PLUGIN_BINARY_{module.module_name} "${{CMAKE_BINARY_DIR}}/Plugins/{project_name}/{module.owning_plugin}/{module.module_name}")\n')
