import os
import sys
import argparse
import json

import config as cfg

_project_name = ""
_project_dir = ""
_project_intermediate_dir = ""
_project_source_dir = ""
_output_file = ""
_search_paths = []

def process_arguments():
    parser = argparse.ArgumentParser(description="Build module index")
    parser.add_argument("--project", help="Project name", required=True) # Engine or Game
    parser.add_argument("--project_dir", help="Project root directory, used to resolve relative paths", required=True)
    parser.add_argument("--search_paths", help="Paths to search for modules, separated by semicolons", required=True)
    args = parser.parse_args()

    global _project_name, _project_dir, _project_intermediate_dir, _project_source_dir, _search_paths
    _project_name = args.project
    _project_dir = os.path.abspath(args.project_dir)
    _project_intermediate_dir = os.path.join(_project_dir, "Intermediate")
    _project_source_dir = os.path.join(_project_dir, "Source")
    _output_file = os.path.abspath(args.output)
    _search_paths = [os.path.abspath(p.strip()) for p in args.search_paths.split(";") if p.strip()]
                        
def main():
    process_arguments()

    found_modules = []
    for path in _search_paths:
        if not path.startswith(_project_dir): # Ensure path must be inside project dir
            continue

        if not os.path.isdir(path): # Skip non-directory paths
            continue
        
        for module_name in os.listdir(path):
            module_dir = os.path.join(path, module_name)
            if os.path.exists(os.path.join(module_dir, "CMakeLists.txt")):
                rel_module_dir = os.path.relpath(module_dir, _project_dir).replace("\\", "/")
                found_modules.append({"Name": module_name, "Path": rel_module_dir}) # Store relative path with forward slashes

    with open(_output_file, "w") as f:
        output_dir = os.path.dirname(_output_file)
        os.makedirs(output_dir, exist_ok=True)
        data = {
            "Project": _project_name,
            "Modules": found_modules
        }
        json.dump(data, f, indent=4)

if __name__ == "__main__":
    main()
