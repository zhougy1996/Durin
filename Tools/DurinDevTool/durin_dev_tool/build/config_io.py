from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Mapping

from ..json_contract import JsonContractError, load_json_contract
from .config import BuildToolError
from .models import BuildProfile, ConfigurePreset, EnvironmentProvider, EnvironmentSetup, LocalConfig


def load_json_object(path: Path, *, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise BuildToolError(f'{label} was not found: "{path}"') from exc
    except json.JSONDecodeError as exc:
        raise BuildToolError(
            f'{label} contains invalid JSON at line {exc.lineno}, column {exc.colno}: "{path}"'
        ) from exc
    except OSError as exc:
        raise BuildToolError(f'Could not read {label.lower()} "{path}": {exc}') from exc
    if not isinstance(value, dict):
        raise BuildToolError(f'{label} must contain a JSON object: "{path}"')
    return value


def optional_nullable_string(container: Mapping[str, Any], key: str, *, label: str) -> str:
    value = container.get(key)
    if value is None:
        return ""
    if not isinstance(value, str) or not value.strip():
        raise BuildToolError(f'{label} field "{key}" must be null or a non-empty string.')
    return value.strip()


def optional_string(container: Mapping[str, Any], key: str, *, label: str) -> str:
    value = container.get(key, "")
    if not isinstance(value, str):
        raise BuildToolError(f'{label} field "{key}" must be a string.')
    return value.strip()


def load_local_config(path: Path) -> LocalConfig:
    if not path.is_file():
        return LocalConfig()
    schema_path = Path(__file__).resolve().parents[2] / "DevTool.user.schema.json"
    try:
        raw = load_json_contract(path, label="DurinDevTool config", schema_path=schema_path)
    except JsonContractError as exc:
        raise BuildToolError(str(exc)) from exc
    assert isinstance(raw, dict)
    build = raw.get("build", {})
    jobs = build.get("parallelJobs", "auto")
    if jobs == "auto":
        resolved_jobs = 0
    else:
        resolved_jobs = jobs
    cmake = raw.get("cmake", {})
    toolchain = raw.get("toolchain", {})
    arguments = toolchain.get("environmentArguments", [])
    return LocalConfig(
        cmake_command=optional_nullable_string(cmake, "command", label="DurinDevTool config cmake"),
        default_build_profile=optional_nullable_string(build, "defaultProfile", label="DurinDevTool config build"),
        jobs=resolved_jobs,
        environment_setup=EnvironmentSetup(
            script=optional_nullable_string(toolchain, "environmentScript", label="DurinDevTool config toolchain"),
            arguments=tuple(arguments),
        ),
    )


def load_profiles(path: Path) -> dict[str, BuildProfile]:
    manifest = load_json_object(path, label="Build profile manifest")
    if manifest.get("version") != 2:
        raise BuildToolError('Build profile manifest field "version" must be 2.')
    raw_profiles = manifest.get("profiles")
    if not isinstance(raw_profiles, dict) or not raw_profiles:
        raise BuildToolError('Build profile manifest field "profiles" must be a non-empty object.')
    profiles: dict[str, BuildProfile] = {}
    for name, raw in raw_profiles.items():
        if not isinstance(name, str) or not name or not isinstance(raw, dict):
            raise BuildToolError("Each build profile must have a non-empty name and object value.")
        label = f'Build profile "{name}"'
        host = optional_string(raw, "host", label=label)
        default_preset = optional_string(raw, "defaultPreset", label=label)
        provider_value = optional_string(raw, "environmentProvider", label=label)
        platform_name = optional_string(raw, "platform", label=label)
        suffix = optional_string(raw, "testExecutableSuffix", label=label)
        is_default = raw.get("default", False)
        required_commands = raw.get("requiredCommands", [])
        presets = raw.get("presets", [])
        if not host or not default_preset or not provider_value or not platform_name:
            raise BuildToolError(f'{label} requires host, defaultPreset, environmentProvider, and platform.')
        try:
            provider = EnvironmentProvider(provider_value)
        except ValueError as exc:
            raise BuildToolError(f'{label} uses unsupported environment provider "{provider_value}".') from exc
        if not isinstance(is_default, bool):
            raise BuildToolError(f'{label} field "default" must be a boolean.')
        if not isinstance(required_commands, list) or not all(isinstance(command, str) and command for command in required_commands):
            raise BuildToolError(f'{label} field "requiredCommands" must be an array of names.')
        if not isinstance(presets, list) or not presets or not all(isinstance(preset, str) and preset for preset in presets):
            raise BuildToolError(f'{label} field "presets" must be a non-empty array of names.')
        if len(set(presets)) != len(presets):
            raise BuildToolError(f'{label} field "presets" contains duplicate names.')
        if default_preset not in presets:
            raise BuildToolError(f'{label} defaultPreset must also appear in its presets list.')
        profiles[name] = BuildProfile(
            name, host, default_preset, tuple(presets), provider, platform_name,
            suffix, is_default, tuple(required_commands),
        )
    return profiles


def load_configure_presets(path: Path) -> dict[str, ConfigurePreset]:
    manifest = load_json_object(path, label="CMake preset manifest")
    raw_presets = manifest.get("configurePresets")
    if not isinstance(raw_presets, list):
        raise BuildToolError('CMake preset manifest field "configurePresets" must be an array.')
    by_name: dict[str, dict[str, Any]] = {}
    for raw in raw_presets:
        if not isinstance(raw, dict):
            raise BuildToolError("Each CMake configure preset must be an object.")
        name = optional_string(raw, "name", label="CMake configure preset")
        if not name or name in by_name:
            raise BuildToolError("Each CMake configure preset must have a unique non-empty name.")
        by_name[name] = raw
    resolved: dict[str, ConfigurePreset] = {}
    resolving: set[str] = set()

    def resolve(name: str) -> ConfigurePreset:
        if name in resolved:
            return resolved[name]
        if name in resolving:
            raise BuildToolError(f'CMake configure preset inheritance contains a cycle at "{name}".')
        raw = by_name.get(name)
        if raw is None:
            raise BuildToolError(f'CMake configure preset "{name}" was not found.')
        resolving.add(name)
        merged: dict[str, Any] = {"cacheVariables": {}}
        inherits = raw.get("inherits", [])
        if isinstance(inherits, str):
            inherits = [inherits]
        if not isinstance(inherits, list) or not all(isinstance(item, str) and item for item in inherits):
            raise BuildToolError(f'CMake configure preset "{name}" has an invalid inherits field.')
        for parent_name in reversed(inherits):
            for key, value in resolve(parent_name).values.items():
                if key == "cacheVariables":
                    merged[key].update(value)
                else:
                    merged[key] = value
        for key, value in raw.items():
            if key == "cacheVariables":
                if not isinstance(value, dict):
                    raise BuildToolError(f'CMake configure preset "{name}" cacheVariables must be an object.')
                merged[key].update(value)
            elif key != "inherits":
                merged[key] = value
        merged["name"] = name
        resolving.remove(name)
        resolved[name] = ConfigurePreset(name, merged)
        return resolved[name]

    for preset_name in by_name:
        resolve(preset_name)
    return resolved
