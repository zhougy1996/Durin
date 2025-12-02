import logging
import sys
import os
import argparse
import json

import helper
import globals as g

class DogeModuleInfo:
    name: str
    path: str
    def __init__(self, name: str, relative_path: str):
        self.name = name
        self.path = relative_path

    def __repr__(self):
        return f"DogeModuleInfo(name=\"{self.name}\", path=\"{self.path}\")"

class DogeProject:
    name: str
    project_file: str
    project_dir: str
    modules: dict[str, DogeModuleInfo]

    def __init__(self, project_file: str):
        self.project_file = os.path.abspath(project_file)
        self.name = os.path.splitext(os.path.basename(self.project_file))[0]
        self.project_dir = os.path.dirname(self.project_file)
        self.modules = {}

        if not os.path.isfile(self.project_file):
            logging.error(f"Project file {self.project_file} does not exist.")
            return

        with open(self.project_file, "r") as f:
            try:
                data = json.load(f)
                module_entries = data.get("Modules", [])
                for module_entry in module_entries:
                    module_name = module_entry.get("Name")
                    module_path = module_entry.get("Path")
                    self.modules[module_name] = DogeModuleInfo(module_name, module_path)
            except json.JSONDecodeError as e:
                logging.error(f"Failed to load project file {self.project_file}: {e}")

    def get_module_dir(self, module_name: str) -> str:
        module_info = self.modules.get(module_name)
        if not module_info:
            logging.error(f"Module {module_name} not found in project.")
            return ""
        return os.path.join(self.project_dir, module_info.path)

    def get_module_file(self, module_name: str) -> str:
        module_dir = self.get_module_dir(module_name)
        return os.path.join(module_dir, f"{module_name}.dmodule")

    def __repr__(self):
        return f"DogeProject(name=\"{self.name}\", project_file=\"{self.project_file}\", modules={self.modules})"



if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Doge header tool.")
    subparsers = parser.add_subparsers(help="utility commands", dest="command")

    generate_headers_parser = subparsers.add_parser("generate_reflection_files", help="Generate headers for a module.")
    generate_headers_parser.add_argument("-p", "--project", help="Specify the doge project file.",
                                         required=True)
    generate_headers_parser.add_argument("-m", "--module", help="Specify the module to process.", required=True)
    args = parser.parse_args()

    helper.init_logging()

    doge_project = DogeProject(args.project)
    print(doge_project)

