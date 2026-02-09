import os
from dataclasses import dataclass, field
from utils.json_utils import load_json_file, dataclass_from_dict
from typing import Dict

# Global dictionary to store project configurations
PROJECT_CONFIGS: Dict[str, "DogeProjectConfig"] = {}

@dataclass
class DogeProjectConfig:
    project_name: str = ""
    project_dir: str = ""
    config_file_path: str = ""
    modules: dict[str, str] = field(default_factory=dict) # module name -> module config file path

    def __post_init__(self):
        pass

    @classmethod
    def from_file(cls, project_config_file_path) -> "DogeProjectConfig":
        project_config_file_path = os.path.abspath(project_config_file_path)
        
        raw_json_data = load_json_file(project_config_file_path, required_fields=["ProjectName"])

        instance = dataclass_from_dict(cls, raw_json_data)
        instance.config_file_path = project_config_file_path
        instance.project_dir = os.path.dirname(instance.config_file_path)
        instance.__post_init__()
        return instance
    
def load_project_config_file(project_config_file_path: str) -> DogeProjectConfig:
    project_config = DogeProjectConfig.from_file(project_config_file_path)
    if project_config:
        PROJECT_CONFIGS[project_config.project_name] = project_config
        return project_config
    raise ValueError(f"Project config file '{project_config_file_path}' could not be loaded.")

def get_project_config(project_name: str) -> DogeProjectConfig:
    if project_name in PROJECT_CONFIGS:
        return PROJECT_CONFIGS[project_name]
    raise ValueError(f"Project '{project_name}' not found.")
    