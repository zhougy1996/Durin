import os
import sys
import argparse
import json
import fnmatch

import config as cfg

_project_dir = ""


# File structure:
# - ProjectDir/
#   - Source/
#     - ModuleA/
#     - ModuleB/
#   - Intermediate/
#     - <ProjectName>.dogeproj.json

def process_arguments():
    parser = argparse.ArgumentParser(description="Build module index")
    parser.add_argument("--project_dir", help="Project root directory, used to resolve relative paths", required=True)
    args = parser.parse_args()

    global _project_dir
    _project_dir = os.path.abspath(args.project_dir)
                        
def main():
    process_arguments()

    project_name = os.path.basename(_project_dir)
    project_file = os.path.join(_project_dir, f"{project_name}.dproject")
    project_source_dir = os.path.join(_project_dir, "Source")
    module_exclude_dirs = []

    # read project file
    if not os.path.isfile(project_file):
        print(f"Project file not found: {project_file}")
        sys.exit(1)

    with open(project_file, "r") as f:
        project_data = json.load(f)
        project_name = project_data.get("ProjectName", project_name)
        project_source_dir = os.path.join(_project_dir, project_data.get("SourceDir"))
        module_exclude_dirs = project_data.get("ModuleExcludeDirs", []) # may be patterns
        
    output_file = os.path.join(_project_dir, "Intermediate", f"{project_name}.projectinfo")

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

    with open(output_file, "w") as f:
        output_dir = os.path.dirname(output_file)
        os.makedirs(output_dir, exist_ok=True)
        data = {
            "Project": project_name,
            "Modules": found_modules
        }
        json.dump(data, f, indent=4)

if __name__ == "__main__":
    main()
