import os
import sys
import argparse
import json
import fnmatch
from typing import LiteralString

# File structure:
# - ProjectDir/
#   - Source/
#     - ModuleA/
#     - ModuleB/
#   - Intermediate/
#     - <ProjectName>.projectinfo

def generate_project_info_file(project_filepath, output_filepath):
    project_filename = os.path.basename(project_filepath)
    project_dir = os.path.dirname(project_filepath)
    project_name = os.path.splitext(project_filename)[0]
    project_source_dir = os.path.join(project_dir, "Source")
    module_exclude_dirs: list[str] = []

    with open(project_filepath, "r") as f:
        project_file_content = f.read()
        if not project_file_content:
            print(f"Project file is empty: {project_filepath}")
            sys.exit(1)
        project_data = json.loads(project_file_content)
        project_name = project_data.get("ProjectName", project_name)
        relative_source_dir = project_data.get("SourceDir", "Source")
        if not isinstance(relative_source_dir, str):
            raise TypeError("relative_source_dir must be a string")
        project_source_dir = os.path.join(project_dir, relative_source_dir)
        module_exclude_dirs = project_data.get("ModuleExcludeDirs", []) # exclude patterns

    found_modules = []
    # scan source directory for modules, assuming each module is a folder with a .dmodule file with folder name
    folder_stack = [project_source_dir]
    while folder_stack:
        current_dir = folder_stack.pop()
        folder_name = os.path.basename(current_dir)
        if any(fnmatch.fnmatch(current_dir, os.path.join(project_source_dir, pattern)) for pattern in module_exclude_dirs):
            continue
        elif os.path.isfile(os.path.join(current_dir, f"{folder_name}.dmodule")):
            found_modules.append({"Name": folder_name, "Path": os.path.relpath(current_dir, project_source_dir).replace("\\", "/")})
        else:
            for item in os.listdir(current_dir):
                item_path = os.path.join(current_dir, item)
                if os.path.isdir(item_path):
                    folder_stack.append(item_path)

    with open(output_filepath, "w") as f:
        json.dump({"ProjectName": project_name, "Modules": found_modules}, f, indent=4)

def main():
    parser = argparse.ArgumentParser(description="Doge build tool.")
    subparsers = parser.add_subparsers(dest="func", required=True, help="Available functions")

    parser_collect_project_info = subparsers.add_parser("generate_project_info_file", help="Collect project information as a json file.")
    parser_collect_project_info.add_argument("--project", help=".dproject file path", type=str, required=True)
    parser_collect_project_info.add_argument("--output", help="Output file path", type=str, required=True)

    args = parser.parse_args()

    if args.func == "generate_project_info_file":
        generate_project_info_file(args.project, args.output)

if __name__ == "__main__":
    main()
