from pathlib import Path
from durin_header_tool import config as configs
from durin_header_tool.config.project_config import PROJECT_CONFIGS
from durin_header_tool import io as utils


def _append_project_paths_to_cmake_content(content: list[str], project_name: str) -> None:
    project_config = configs.get_project_config(project_name)
    content.append("# Paths related to this project\n")
    content.append(f"set(DURIN_PROJECT_CONFIG_FILE \"{project_config.config_file_path.as_posix()}\")\n")
    content.append(f"set(DURIN_PROJECT_INTERMEDIATE_BUILD_DIR \"{utils.get_project_intermediate_build_dir(project_name).as_posix()}\")\n")
    content.append("set(DURIN_DHT_PROJECT_FILE_ARGS\n")
    for loaded_project in sorted(PROJECT_CONFIGS.values(), key=lambda project: str(project.config_file_path)):
        content.append(f"    --project-file \"{loaded_project.config_file_path.as_posix()}\"\n")
    content.append(")\n")
    content.append("\n")

def _append_project_global_variables_to_cmake_content(content: list[str], project_name: str) -> None:
    project_config = configs.get_project_config(project_name)
    content.append("# Global variables for durin project\n")
    content.append(f"set(DURIN_PROJECT_DIR \"{project_config.project_dir.as_posix()}\")\n")
    content.append(f"set(DURIN_PROJECT_SOURCE_DIR \"{utils.get_project_source_dir(project_name).as_posix()}\")\n")
    content.append(f"set(DURIN_PROJECT_INTERMEDIATE_DIR \"{utils.get_project_intermediate_dir(project_name).as_posix()}\")\n")
    content.append(f"set(DURIN_PROJECT_BINARY_DIR \"{utils.get_project_binary_dir(project_name).as_posix()}\")\n")
    content.append(f"set(DURIN_PROJECT_CONFIG_DIR \"{utils.get_project_config_dir(project_name).as_posix()}\")\n")
    content.append(f"set(DURIN_PROJECT_CMAKE_DIR \"{utils.get_project_cmake_dir(project_name).as_posix()}\")\n")
    content.append("\n")


def _append_project_runtime_variant_to_cmake_content(content: list[str], project_name: str) -> None:
    runtime_variant_config = configs.get_runtime_variant_config(
        project_name,
        configs.RUNTIME_VARIANT,
    )
    content.append("# Active built-in runtime variant for this project/build mode\n")
    if runtime_variant_config is None:
        raise ValueError(
            f"Unknown built-in runtime variant '{configs.RUNTIME_VARIANT}' "
            f"for project {project_name}."
        )

    content.append(
        f"set(DURIN_PROJECT_RUNTIME_VARIANT \"{runtime_variant_config.runtime_variant}\")\n"
    )
    content.append(
        f"set(DURIN_PROJECT_RUNTIME_VARIANT_WITH_EDITOR "
        f"{'ON' if runtime_variant_config.with_editor else 'OFF'})\n"
    )
    content.append("\n")


def _append_project_build_variables_to_cmake_content(content: list[str], project_name: str) -> None:
    runtime_variant_config = configs.get_runtime_variant_config(
        project_name,
        configs.RUNTIME_VARIANT,
    )
    if runtime_variant_config is None:
        raise ValueError(
            f"Unknown built-in runtime variant '{configs.RUNTIME_VARIANT}' "
            f"for project {project_name}."
        )
    with_editor = 1 if runtime_variant_config.with_editor else 0

    content.append("# Derived build variables for this project/runtime variant\n")
    content.append(f"set(DURIN_WITH_EDITOR {with_editor})\n")
    content.append("\n")
    content.append("set(DURIN_PROJECT_APP_CONFIG_FILE \"${DURIN_PROJECT_RUNTIME_VARIANT}.yaml\")\n")
    content.append("set(DURIN_PROJECT_OUTPUT_CONFIG \"$<CONFIG>\")\n")
    content.append("if(DURIN_BUILD_IDENTIFIER)\n")
    content.append("    set(DURIN_PROJECT_OUTPUT_CONFIG \"$<CONFIG>-${DURIN_BUILD_IDENTIFIER}\")\n")
    content.append("endif()\n")
    content.append("set(DURIN_PROJECT_BIN_ROOT \"${DURIN_PROJECT_BINARY_DIR}/${DURIN_ARCH}/${DURIN_PROJECT_OUTPUT_CONFIG}\")\n")
    content.append("set(DURIN_PROJECT_RUNTIME_OUTPUT_DIR \"${DURIN_PROJECT_BIN_ROOT}/Runtime/${DURIN_PROJECT_RUNTIME_VARIANT}\")\n")
    content.append("set(DURIN_PROJECT_THIRDPARTY_RUNTIME_DIR \"${DURIN_PROJECT_BIN_ROOT}/ThirdParty\")\n")
    content.append("set(DURIN_PROJECT_TEST_OUTPUT_ROOT \"${DURIN_PROJECT_BIN_ROOT}/Tests/${DURIN_PROJECT_RUNTIME_VARIANT}\")\n")
    content.append("set(DURIN_PROJECT_LIB_OUTPUT_ROOT \"${DURIN_PROJECT_BIN_ROOT}/Lib\")\n")
    content.append("set(DURIN_PROJECT_SYMBOL_OUTPUT_ROOT \"${DURIN_PROJECT_BIN_ROOT}/Symbols\")\n")
    content.append("set(DURIN_PROJECT_EXTERNAL_RUNTIME_DIR \"${DURIN_PROJECT_THIRDPARTY_RUNTIME_DIR}\")\n")
    content.append("\n")

def _append_module_dirs_to_cmake_content(content: list[str], project_name: str) -> None:
    project_config = configs.get_project_config(project_name)
    enabled_module_names = configs.collect_enabled_modules_for_project(
        project_name,
        configs.RUNTIME_VARIANT,
    )
    enabled_module_dirs = [project_config.module_dirs[module_name] for module_name in project_config.module_dirs if module_name in enabled_module_names]

    content.append("# Module directories for this project\n")
    content.append("set(DURIN_PROJECT_MODULE_DIRS\n")
    for module_dir in enabled_module_dirs:
        content.append(f"    \"{module_dir}\"\n")
    content.append(")\n\n")


def generate_project_cmake_file(project_name: str) -> None:
    content = [f"# CMake file for project {project_name}\n# Auto-generated by DurinHeaderTool. Do not edit manually.\n\n"]
    _append_project_paths_to_cmake_content(content, project_name)
    _append_project_global_variables_to_cmake_content(content, project_name)
    _append_project_runtime_variant_to_cmake_content(content, project_name)
    _append_project_build_variables_to_cmake_content(content, project_name)
    _append_module_dirs_to_cmake_content(content, project_name)
    output_path = utils.get_project_cmake_file_path(project_name)
    utils.generate_file(output_path, "".join(content))
