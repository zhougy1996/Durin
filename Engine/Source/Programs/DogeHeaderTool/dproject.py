import os
import sys
import json
import logging
from dataclasses import dataclass

import globals as g

@dataclass
class DogeModuleConfig:
    name: str = ""
    dir: str = ""
    owning_project: str = ""
    private_dependencies: list = None
    dht_headers: list = None

    def from_file(self, filepath) -> None:
        if not os.path.isfile(filepath):
            raise FileNotFoundError(f"Module file {filepath} does not exist.")
        
        with open(filepath, "r") as f:
            data = json.load(f)
            self.name = data.get("ModuleName", "")
            self.dir = os.path.abspath(os.path.dirname(filepath))
            self.private_dependencies = data.get("PrivateDependencies", [])
            self.dht_headers = data.get("DHTHeaders", [])

@dataclass
class DogeProjectConfig:
    name: str = ""
    dir: str = ""
    modules: dict[str, str] = None # module name -> module file path
    arch: str = "Win64"

    def from_file(self, filepath) -> None:
        if not os.path.isfile(filepath):
            raise FileNotFoundError(f"Project file {filepath} does not exist.")
        
        with open(filepath, "r") as f:
            data = json.load(f)
            self.name = data.get("ProjectName", "")
            self.dir = os.path.abspath(os.path.dirname(filepath))
            self.modules = data.get("Modules", {})

    def get_intermediate_dir(self) -> str:
        return os.path.join(self.dir, "Intermediate")
    
    def get_binary_dir(self) -> str:
        return os.path.join(self.dir, "Binaries")
    
    def get_module_dir(self, module_name: str) -> str:
        module_file = self.modules.get(module_name, "")
        if not module_file:
            logging.error(f"Module {module_name} not found in project {self.name}.")
            return ""
        return os.path.dirname(module_file)
    
    def get_module_dht_dir(self, module_name: str) -> str:
        intermediate_dir = self.get_intermediate_dir()
        return os.path.join(intermediate_dir, "Build", g.arch, g.build_mode, module_name, "DHT")

# load .dproject file
def load_dproject_config(filepath) -> DogeProjectConfig:
    proj = DogeProjectConfig()
    proj.from_file(filepath)
    return proj

# load .dmodule file
def load_module_config(filepath: str) -> DogeModuleConfig:
    module = DogeModuleConfig()
    module.from_file(filepath)
    return module

def get_module_dirs(dproj_filepath: str) -> list[str]:
    dproj = load_dproject_config(dproj_filepath)
    # Print the list of module directories
    module_dirs = []
    for module_file in dproj.modules.values():
        module_dirs.append(os.path.dirname(module_file))
    return module_dirs