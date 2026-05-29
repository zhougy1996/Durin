from pathlib import Path
from dataclasses import dataclass
from typing import Dict

from utils.json_utils import load_json_file, dataclass_from_dict
from .project_config import get_project_config

PROFILE_CONFIGS: Dict[tuple[str, str], "DurinProfileConfig"] = {}


@dataclass
class DurinProfileConfig:
    profile_name: str = ""
    with_editor: bool = False
    app_config_name: str = ""
    project_name: str = ""
    config_file_path: Path = Path("")
    profile_dir: Path = Path("")

    @classmethod
    def from_file(cls, profile_config_file_path: Path) -> "DurinProfileConfig":
        profile_config_file_path = profile_config_file_path.resolve()

        raw_json_data = load_json_file(profile_config_file_path, required_fields=["ProfileName"])
        instance = dataclass_from_dict(cls, raw_json_data)
        instance.config_file_path = profile_config_file_path
        instance.profile_dir = profile_config_file_path.parent
        return instance


def _get_profile_config_file_path(project_name: str, profile_name: str) -> Path:
    project_config = get_project_config(project_name)
    return project_config.project_dir / "Profiles" / f"{profile_name}.dprofile"


def get_profile_config(project_name: str, profile_name: str) -> DurinProfileConfig | None:
    cache_key = (project_name, profile_name)
    if cache_key in PROFILE_CONFIGS:
        return PROFILE_CONFIGS[cache_key]

    profile_config_file_path = _get_profile_config_file_path(project_name, profile_name)
    if not profile_config_file_path.exists():
        return None

    profile_config = DurinProfileConfig.from_file(profile_config_file_path)
    profile_config.project_name = project_name
    PROFILE_CONFIGS[cache_key] = profile_config
    return profile_config
