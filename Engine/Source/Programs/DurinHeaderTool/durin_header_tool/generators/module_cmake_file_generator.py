from pathlib import Path
import shutil
from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.model.generated_output import generated_output_names

def _append_module_configs_to_cmake_content(content: list[str], module_name: str) -> None:
    module_config: configs.DurinModuleConfig = configs.get_module_config(module_name)

    def _is_dependency_enabled(dep: str) -> bool:
        return configs.is_module_enabled_for_active_runtime_variant(dep)

    if len(module_config.reflect_headers) > 0:
        reflect_header_file_paths = [f"${{module_dir}}/{Path(header).as_posix()}" for header in module_config.reflect_headers]
        content.append("# Reflect headers for this module\n")
        content.append("set(module_reflect_headers\n")
        for header_path in reflect_header_file_paths:
            content.append(f"    \"{header_path}\"\n")
        content.append(")\n\n")

    public_dependencies = [dep for dep in module_config.public_dependencies if _is_dependency_enabled(dep)]
    if len(public_dependencies) > 0:
        content.append("# Public dependencies for this module\n")
        content.append("set(module_public_dependencies\n")
        for dep in public_dependencies:
            content.append(f"    {dep}\n")
        content.append(")\n\n")

    optional_public_dependencies = [dep for dep in module_config.optional_public_dependencies if _is_dependency_enabled(dep)]
    if len(optional_public_dependencies) > 0:
        content.append("# Optional public dependencies for this module\n")
        content.append("set(module_optional_public_dependencies\n")
        for dep in optional_public_dependencies:
            content.append(f"    {dep}\n")
        content.append(")\n\n")

    private_dependencies = [dep for dep in module_config.private_dependencies if _is_dependency_enabled(dep)]
    if len(private_dependencies) > 0:
        content.append("# Private dependencies for this module\n")
        content.append("set(module_private_dependencies\n")
        for dep in private_dependencies:
            content.append(f"    {dep}\n")
        content.append(")\n\n")

    optional_private_dependencies = [dep for dep in module_config.optional_private_dependencies if _is_dependency_enabled(dep)]
    if len(optional_private_dependencies) > 0:
        content.append("# Optional private dependencies for this module\n")
        content.append("set(module_optional_private_dependencies\n")
        for dep in optional_private_dependencies:
            content.append(f"    {dep}\n")
        content.append(")\n\n")

def _append_export_dependencies_to_cmake_content(content: list[str], module_name: str) -> None:
    module_export_file = utils.get_module_export_file_path(module_name)
    dependency_export_files = [
        export_file
        for export_file in configs.collect_all_dependent_module_export_files(module_name)
        if export_file != module_export_file
    ]
    content.append("# Dependency export files consumed by export resolution and reflection generation\n")
    content.append("set(module_export_dependencies\n")
    for dependency_export_file in dependency_export_files:
        content.append(f"    \"{dependency_export_file.as_posix()}\"\n")
    content.append(")\n\n")
    content.append("set(module_reflection_export_dependencies ${module_export_dependencies})\n\n")

def _append_module_paths_to_cmake_content(content: list[str], module_name: str) -> None:
    module_config = configs.get_module_config(module_name)
    content.append("# Paths related to this module\n")
    # This metadata is included from the module's CMakeLists.txt. Let CMake
    # retain its source-directory spelling, including any workspace symlink.
    content.append('set(module_dir "${CMAKE_CURRENT_SOURCE_DIR}")\n')
    content.append(f'set(module_config_file "${{module_dir}}/{module_config.config_file_path.name}")\n')
    if module_config.has_export_file():
        content.append(f"set(module_dht_output_dir \"{utils.get_module_dht_output_dir(module_name).as_posix()}\")\n")
        content.append(f"set(module_export_file \"{utils.get_module_export_file_path(module_name).as_posix()}\")\n")
    content.append("\n")

def _append_generated_sources(content: list[str], module_name: str) -> None:
    module_config = configs.get_module_config(module_name)
    if len(module_config.reflect_headers) == 0:
        return
    
    module_dht_output_dir = utils.get_module_dht_output_dir(module_name)

    content.append("# Generated source files for this module\n")
    content.append("set(module_generated_srcs\n")
    for output_name in generated_output_names(module_name, module_config.reflect_headers):
        content.append(f"    \"{(module_dht_output_dir / output_name).as_posix()}\"\n")
    content.append(")\n\n")

def _make_module_cmake_file_content(module_name: str) -> str:
    module_config = configs.get_module_config(module_name)
    content = [f"# CMake file for module {module_name} of project {module_config.owning_project}\n# Auto-generated by DurinHeaderTool. Do not edit manually.\n\n"]
    content.append(f"set(module_name \"{module_name}\")\n")
    content.append(f"set(module_link_type \"{module_config.link_type.upper()}\")\n\n")
    if module_config.pch != "Self":
        content.append(f"set(module_pch_target \"{module_config.pch}\")\n\n")

    _append_module_paths_to_cmake_content(content, module_name)
    _append_module_configs_to_cmake_content(content, module_name)

    if module_config.has_export_file():
        _append_export_dependencies_to_cmake_content(content, module_name)

    _append_generated_sources(content, module_name)
    return "".join(content)


def generate_module_cmake_file(module_name: str) -> None:
    content = _make_module_cmake_file_content(module_name)
    output_path = utils.get_module_cmake_file_path(module_name)
    utils.generate_file(output_path, content)

def generate_all_module_cmake_files_for_project(project_name: str) -> None:
    project_config = configs.get_project_config(project_name)
    enabled_module_names = sorted(
        configs.collect_enabled_modules_for_project(project_name, configs.RUNTIME_VARIANT)
    )
    disabled_module_names = sorted(set(project_config.modules.keys()) - set(enabled_module_names))

    for module_name in disabled_module_names:
        stale_module_build_dir = utils.get_module_intermediate_build_dir(module_name)
        if stale_module_build_dir.exists():
            shutil.rmtree(stale_module_build_dir)

    for module_name in enabled_module_names:
        generate_module_cmake_file(module_name)
