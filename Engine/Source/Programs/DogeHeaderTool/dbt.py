import logging
import sys
import os
import argparse
import json

import helper
import globals as g

class DogeModule:
    name: str
    module_file: str
    owner_project: 'DogeProject'

    def __init__(self, owner_project: 'DogeProject', module_file: str):
        self.name = os.path.splitext(os.path.basename(module_file))[0]
        self.module_file = module_file
        self.owner_project = owner_project


class DogeProject:
    name: str
    project_file: str
    project_dir: str
    modules: list[DogeModule]

    def __init__(self, project_file: str):
        self.project_file = project_file
        self.name = os.path.splitext(os.path.basename(project_file))[0]
        self.project_dir = os.path.dirname(project_file)
        self.modules = []

        if not os.path.isfile(self.project_file):
            logging.error(f"Project file {self.project_file} does not exist.")
            return

    # "Modules": [
    #     {
    #         "Name": "Core",
    #         "Path": "Source/Runtime/Core"
    #     },
        with open(self.project_file, "r") as f:
            try:
                data = json.load(f)
                module_entries = data.get("Modules", [])
                for module_entry in module_entries:
                    module_path = module_entry.get("Path", "")
                    full_module_path = os.path.join(self.project_dir, module_path)
                    module_file = os.path.join(full_module_path, f"{module_entry.get('Name', '')}.dmodule")
                    if os.path.isfile(module_file):
                        module = DogeModule(self, module_file)
                        self.modules.append(module)
                    else:
                        logging.warning(f"Module file {module_file} does not exist, skipping.")
            except json.JSONDecodeError as e:
                logging.error(f"Failed to load project file {self.project_file}: {e}")



if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Doge header tool.")
    subparsers = parser.add_subparsers(help="utility commands", dest="command")

    generate_headers_parser = subparsers.add_parser("generate_reflection_files", help="Generate headers for a module.")
    generate_headers_parser.add_argument("-p", "--project", help="Specify the doge project file.",
                                         required=True)
    args = parser.parse_args()

    helper.init_logging()

    # Output full project file path, not relative
    logging.info("project file: " + os.path.abspath(args.project))
    doge_project = DogeProject(args.project)

