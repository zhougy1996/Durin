from dataclasses import dataclass
from typing import Dict

PROFILE_CONFIGS: Dict[tuple[str, str], "DurinProfileConfig"] = {}
BUILTIN_PROFILES: Dict[str, "DurinProfileConfig"] = {}


@dataclass
class DurinProfileConfig:
    profile_name: str = ""
    with_editor: bool = False
    app_config_name: str = ""
    project_name: str = ""


def _build_builtin_profiles() -> Dict[str, DurinProfileConfig]:
    return {
        "DurinEditor": DurinProfileConfig(
            profile_name="DurinEditor",
            with_editor=True,
            app_config_name="DurinEditorConfig.yaml",
        ),
        "DurinGame": DurinProfileConfig(
            profile_name="DurinGame",
            with_editor=False,
            app_config_name="DurinGameConfig.yaml",
        ),
    }


def get_profile_config(project_name: str, profile_name: str) -> DurinProfileConfig | None:
    cache_key = (project_name, profile_name)
    if cache_key in PROFILE_CONFIGS:
        return PROFILE_CONFIGS[cache_key]

    builtin_profile = BUILTIN_PROFILES.get(profile_name)
    if builtin_profile is None:
        return None

    profile_config = DurinProfileConfig(
        profile_name=builtin_profile.profile_name,
        with_editor=builtin_profile.with_editor,
        app_config_name=builtin_profile.app_config_name,
        project_name=project_name,
    )
    PROFILE_CONFIGS[cache_key] = profile_config
    return profile_config


BUILTIN_PROFILES = _build_builtin_profiles()
