from pathlib import Path
from durin_header_tool import config as configs
from durin_header_tool import io as utils

def get_module_api_macro(module_name: str) -> str:
    return f"{module_name.upper()}_API"

def append_dependent_module_api_macros(content: list[str], module_name: str) -> None:
    dependent_modules = configs.collect_sorted_dependent_modules(module_name)
    for dep_module in dependent_modules:
        api_macro = get_module_api_macro(dep_module)
        content.append(f"#define {api_macro} DLLIMPORT\n")
    content.append("\n")

def generate_module_definitions_header(module_name: str) -> None:
    module_config = configs.get_module_config(module_name)
    content = [f"// Auto-generated header for module {module_name} of project {module_config.owning_project}. Do not edit manually.\n\n"]
    content.append(f"#pragma once\n\n")
    content.append(f"#include \"HAL/Platform.h\"\n\n")
    if module_config.link_type.upper() == "SHARED":
        content.append(f"#define {get_module_api_macro(module_name)} DLLEXPORT\n\n")
    append_dependent_module_api_macros(content, module_name)
    output_path = utils.get_module_definitions_header_path(module_name)
    utils.generate_file(output_path, "".join(content))
