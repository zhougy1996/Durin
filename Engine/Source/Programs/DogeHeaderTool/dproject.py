import os
import sys
import json
import logging
from dataclasses import dataclass

@dataclass
class DogeModuleConfig:
    name: str = ""
    dir: str = ""
    owning_project: str = ""
    private_dependencies: list = None
    dht_headers: list = None

    def from_file(self, filepath) -> None:
        if not os.path.isfile(filepath):
            logging.error(f"Module file {filepath} does not exist.")
            return
        
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

    def from_file(self, filepath) -> None:
        if not os.path.isfile(filepath):
            logging.error(f"Project file {filepath} does not exist.")
            return
        
        with open(filepath, "r") as f:
            data = json.load(f)
            self.name = data.get("ProjectName", "")
            self.dir = os.path.abspath(os.path.dirname(filepath))
            self.modules = data.get("Modules", {})

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