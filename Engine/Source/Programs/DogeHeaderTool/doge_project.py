import os
import sys
import json
import logging
from dataclasses import dataclass

import doge_globals as g

@dataclass
class DogeModuleConfig:
    name: str = ""
    dir: str = ""
    owning_project: str = ""
    private_dependencies: list = None
    dht_headers: list = None
    api_macro: str = ""

    def from_file(self, filepath) -> None:
        if not os.path.isfile(filepath):
            raise FileNotFoundError(f"Module file {filepath} does not exist.")
        
        with open(filepath, "r") as f:
            data = json.load(f)
            self.name = data.get("ModuleName", "")
            self.dir = os.path.abspath(os.path.dirname(filepath))
            self.private_dependencies = data.get("PrivateDependencies", [])
            self.dht_headers = data.get("DHTHeaders", [])
        
        self.api_macro = f"{self.name.upper()}_API"

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
        return os.path.join(intermediate_dir, "Build", g.ARCH, g.BUILD_MODE, module_name, "DHT")

@dataclass
class DogeModuleDependencyInfo:
    module_name: str
    deps: list[str]
    fingerprint: str
    reflect_headers: list[str]
    deps_fingerprint: dict[str, str]

    def from_file(self, filepath) -> None:
        if not os.path.isfile(filepath):
            raise FileNotFoundError(f"Dependency file {filepath} does not exist.")
        
        with open(filepath, "r") as f:
            data = json.load(f)
            self.module_name = data.get("ModuleName", "")
            self.deps = data.get("Dependencies", [])
            self.fingerprint = data.get("Fingerprint", "")
            self.reflect_headers = data.get("ReflectHeaders", [])
            self.deps_fingerprint = data.get("DependenciesFingerprint", {})
    
    def from_configs(self, module_config: DogeModuleConfig) -> None:
        self.module_name = module_config.name
        self.deps = module_config.private_dependencies if module_config.private_dependencies else []
        self.fingerprint = "" # TODO: compute fingerprint
        self.reflect_headers = module_config.dht_headers if module_config.dht_headers else []
        self.deps_fingerprint = {} # TODO: compute dependencies fingerprint

# load .dproject file
def load_project_config(filepath) -> DogeProjectConfig:
    proj = DogeProjectConfig()
    proj.from_file(filepath)
    return proj

# load .dmodule file
def load_module_config(filepath: str) -> DogeModuleConfig:
    module = DogeModuleConfig()
    module.from_file(filepath)
    return module

def get_module_dirs(project_cfg) -> list[str]:
    module_dirs = []
    for module_file in project_cfg.modules.values():
        module_dirs.append(os.path.dirname(module_file))
    return module_dirs

def generate_module_dependency_file(project_cfg, module_name: str) -> None:
    module_file = project_cfg.modules.get(module_name, "")
    if not module_file:
        logging.error(f"Module {module_name} not found in project {project_cfg.name}.")
        return
    
    module = load_module_config(module_file)
    dependency_file_path = os.path.join(project_cfg.get_module_dht_dir(module_name), f"{module_name}_Dependencies.txt")
    
    with open(dependency_file_path, "w") as f:
        for dep in module.private_dependencies:
            f.write(f"{dep}\n")
    
    logging.info(f"Generated dependency file at {dependency_file_path}")