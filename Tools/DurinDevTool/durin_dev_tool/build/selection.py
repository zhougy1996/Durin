from __future__ import annotations

import os
import platform
from pathlib import Path
from typing import Any, Mapping

from .config import BuildToolError
from .models import BuildProfile, ConfigurePreset


PROFILE_ENV_VAR = "DURIN_AGENT_BUILD_PROFILE"


def host_name(system_name: str | None = None) -> str:
    normalized = (system_name or platform.system()).lower()
    return {"darwin": "macos", "linux": "linux", "windows": "windows"}.get(normalized, normalized)


def select_profile(
    profiles: Mapping[str, BuildProfile],
    *,
    requested: str = "",
    configured: str = "",
    environment: Mapping[str, str] | None = None,
    current_host: str | None = None,
    profile_file: Path,
) -> BuildProfile:
    environment = os.environ if environment is None else environment
    detected_host = current_host or host_name()
    selected_name = requested or environment.get(PROFILE_ENV_VAR, "").strip() or configured
    if selected_name:
        selected = profiles.get(selected_name)
        if selected is None:
            raise BuildToolError(f'Unknown build profile "{selected_name}". Available profiles: {", ".join(sorted(profiles))}.')
        if selected.host != detected_host:
            raise BuildToolError(
                f'Build profile "{selected_name}" targets host "{selected.host}", '
                f'but the current host is "{detected_host}".'
            )
        return selected
    host_profiles = [profile for profile in profiles.values() if profile.host == detected_host]
    defaults = [profile for profile in host_profiles if profile.is_default]
    candidates = defaults or host_profiles
    if len(candidates) == 1:
        return candidates[0]
    if not candidates:
        raise BuildToolError(
            f'No build profile is registered for host "{detected_host}". '
            f'Add a profile to "{profile_file}" before building on this platform.'
        )
    raise BuildToolError(
        f'Multiple build profiles are available for host "{detected_host}"; '
        "select one with --profile or DURIN_AGENT_BUILD_PROFILE."
    )


def select_preset(
    profile: BuildProfile,
    presets: Mapping[str, ConfigurePreset],
    *,
    requested: str = "",
    preset_file: Path,
) -> ConfigurePreset:
    selected_name = requested or profile.default_preset
    if selected_name not in profile.presets:
        raise BuildToolError(
            f'Preset "{selected_name}" is not enabled for this build profile. '
            f'Available presets: {", ".join(profile.presets)}.'
        )
    if selected_name not in presets:
        raise BuildToolError(f'Enabled CMake preset "{selected_name}" was not found in "{preset_file}".')
    return presets[selected_name]


def preset_cache_string(preset: ConfigurePreset, name: str, *, required: bool = True) -> str:
    value = preset.values.get("cacheVariables", {}).get(name)
    if isinstance(value, dict):
        value = value.get("value")
    if value is None and not required:
        return ""
    if not isinstance(value, (str, int, float, bool)):
        raise BuildToolError(f'CMake preset "{preset.name}" must define {name}.')
    return str(value)


def preset_cache_bool(preset: ConfigurePreset, name: str) -> bool:
    return preset_cache_string(preset, name, required=False).strip().lower() in {"1", "on", "true", "yes", "y"}


def expand_preset_path(value: Any, preset: ConfigurePreset, *, root: Path) -> Path:
    root = root.resolve()
    if not isinstance(value, str) or not value:
        raise BuildToolError(f'CMake preset "{preset.name}" contains an invalid path.')
    expanded = value.replace("${sourceDir}", str(root)).replace("${presetName}", preset.name)
    path = Path(expanded).resolve()
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise BuildToolError(f'CMake preset build directory must stay inside the checkout: "{path}"') from exc
    return path


def preset_build_directory(preset: ConfigurePreset, *, root: Path) -> Path:
    binary_dir = preset.values.get("binaryDir")
    if not isinstance(binary_dir, str) or not binary_dir:
        raise BuildToolError(f'CMake preset "{preset.name}" must define binaryDir.')
    return expand_preset_path(binary_dir, preset, root=root)


def preset_install_directory(preset: ConfigurePreset, *, root: Path) -> Path | None:
    install_prefix = preset_cache_string(preset, "CMAKE_INSTALL_PREFIX", required=False)
    return expand_preset_path(install_prefix, preset, root=root) if install_prefix else None


def preset_output_configuration(preset: ConfigurePreset) -> str:
    configuration = preset_cache_string(preset, "CMAKE_BUILD_TYPE")
    preset_role = preset_cache_string(preset, "DURIN_PRESET_ROLE", required=False)
    return f"{configuration}-Profiling" if preset_role == "Profiling" else configuration
