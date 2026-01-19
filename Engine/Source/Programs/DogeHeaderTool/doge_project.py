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
    public_dependencies: list = None
    reflect_headers: list = None
    api_macro: str = ""

    def from_file(self, filepath) -> None:
        if not os.path.isfile(filepath):
            raise FileNotFoundError(f"Module file {filepath} does not exist.")
        
        with open(filepath, "r") as f:
            data = json.load(f)
            self.name = data.get("ModuleName", "")
            self.dir = os.path.abspath(os.path.dirname(filepath))
            self.private_dependencies = data.get("PrivateDependencies", [])
            self.public_dependencies = data.get("PublicDependencies", [])
            self.reflect_headers = data.get("DHTHeaders", [])
        
        self.api_macro = f"{self.name.upper()}_API"

    def get_manifest_path(self) -> str:
        project : DogeProjectConfig = g.projects.get(self.owning_project)
        if not project:
            logging.error(f"Owning project {self.owning_project} not found for module {self.name}.")
            return ""
        return project.get_module_manifest_path(self.name)

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
    
    def get_module_manifest_path(self, module_name: str) -> str:
        dht_dir = self.get_module_dht_dir(module_name)
        return os.path.join(dht_dir, f"{module_name}.module.manifest")

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

def generate_module_dependency_file(project_cfg: DogeProjectConfig, module_name: str) -> None:
    module_file = project_cfg.modules.get(module_name, "")
    if not module_file:
        logging.error(f"Module {module_name} not found in project {project_cfg.name}.")
        return
    
    module = load_module_config(os.path.join(project_cfg.dir, module_file))
    dependency_file_path = os.path.join(project_cfg.get_module_dht_dir(module_name), f"{module_name}_Dependencies.txt")
    
    with open(dependency_file_path, "w") as f:
        for dep in module.private_dependencies:
            f.write(f"{dep}\n")
    
    logging.info(f"Generated dependency file at {dependency_file_path}")

def generate_module_cmake_file(project_cfg: DogeProjectConfig, module_name: str, output_path: str) -> None:
    module_file = project_cfg.modules.get(module_name, "")
    if not module_file:
        logging.error(f"Module {module_name} not found in project {project_cfg.name}.")
        return
    
    dependent_modules = get_all_dependent_modules(project_cfg, module_name)
    dep_manifest_files = [project_cfg.get_module_manifest_path(mod.name) for mod in dependent_modules]

    module = load_module_config(os.path.join(project_cfg.dir, module_file))
    # pure data file, no logic yet
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w") as f:
        f.write(f"# CMake file for module {module_name} of project {project_cfg.name}\n")
        reflect_headers = "\n    ".join([f'"{os.path.join(module.dir, header).replace(os.sep, "/")}"' for header in module.reflect_headers])
        f.write(f"set(module_reflect_headers\n    {reflect_headers}\n)\n")
        private_dependencies = "\n    ".join(module.private_dependencies)
        f.write(f"set(module_private_dependencies\n    {private_dependencies}\n)\n")
        dep_manifest_files_str = "\n    ".join([f'"{path.replace(os.sep, "/")}"' for path in dep_manifest_files])
        f.write(f"set(module_dependency_manifests\n    {dep_manifest_files_str}\n)\n")

def get_all_dependent_modules(project_cfg: DogeProjectConfig, module_name: str) -> list[DogeModuleConfig]:
    visited = set()
    result = []

    # Depth-first search to find all dependencies, only public dependencies are considered
    def dfs(mod_name: str):
        if mod_name in visited:
            return
        visited.add(mod_name)
        module_file = project_cfg.modules.get(mod_name, "")
        if not module_file:
            logging.warning(f"Module {mod_name} not found in project {project_cfg.name}. Skipping.")
            return
        module = load_module_config(os.path.join(project_cfg.dir, module_file))
        for dep in module.public_dependencies:
            dfs(dep)
        result.append(module)

    input_module_file = project_cfg.modules.get(module_name, "")
    if not input_module_file:
        logging.error(f"Module {module_name} not found in project {project_cfg.name}.")
        return result
    
    input_module = load_module_config(os.path.join(project_cfg.dir, input_module_file))
    for dep in input_module.private_dependencies:
        dfs(dep)

    for dep in input_module.public_dependencies:
        dfs(dep)

    return result


