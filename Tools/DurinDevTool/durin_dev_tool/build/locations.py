"""Typed repository build and artifact location resolution."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from ..context import RepositoryContext
from .config import (
    BuildProfile,
    BuildToolError,
    ConfigurePreset,
    preset_build_directory,
    preset_cache_string,
    preset_output_configuration,
)


LocationResolver = Callable[
    [Path, BuildProfile | None, ConfigurePreset | None, Path, Path],
    Path,
]


@dataclass(frozen=True)
class LocationSpec:
    name: str
    aliases: tuple[str, ...]
    requires_build_context: bool
    resolver: LocationResolver
    missing_recovery: str = ""


@dataclass(frozen=True)
class ResolvedLocation:
    spec: LocationSpec
    path: Path
    is_directory: bool


def _require_profile(profile: BuildProfile | None, name: str) -> BuildProfile:
    if profile is None:
        raise BuildToolError(f'Location "{name}" requires a selected build profile.')
    return profile


def _require_preset(preset: ConfigurePreset | None, name: str) -> ConfigurePreset:
    if preset is None:
        raise BuildToolError(f'Location "{name}" requires a selected CMake preset.')
    return preset


def _root(
    root: Path,
    _profile: BuildProfile | None,
    _preset: ConfigurePreset | None,
    _runtime_binaries: Path,
    _state_directory: Path,
) -> Path:
    return root


def _build(
    root: Path,
    _profile: BuildProfile | None,
    preset: ConfigurePreset | None,
    _runtime_binaries: Path,
    _state_directory: Path,
) -> Path:
    return preset_build_directory(_require_preset(preset, "build"), root=root)


def _binaries(
    root: Path,
    _profile: BuildProfile | None,
    _preset: ConfigurePreset | None,
    runtime_binaries: Path,
    _state_directory: Path,
) -> Path:
    return root / runtime_binaries


def _output(
    root: Path,
    profile: BuildProfile | None,
    preset: ConfigurePreset | None,
    runtime_binaries: Path,
    state_directory: Path,
) -> Path:
    selected_profile = _require_profile(profile, "output")
    selected_preset = _require_preset(preset, "output")
    return (
        _binaries(root, profile, preset, runtime_binaries, state_directory)
        / selected_profile.platform
        / preset_output_configuration(selected_preset)
    )


def _runtime(
    root: Path,
    profile: BuildProfile | None,
    preset: ConfigurePreset | None,
    runtime_binaries: Path,
    state_directory: Path,
) -> Path:
    selected_preset = _require_preset(preset, "runtime")
    return (
        _output(root, profile, selected_preset, runtime_binaries, state_directory)
        / "Runtime"
        / preset_cache_string(selected_preset, "DURIN_RUNTIME_VARIANT")
    )


def _tests(
    root: Path,
    profile: BuildProfile | None,
    preset: ConfigurePreset | None,
    runtime_binaries: Path,
    state_directory: Path,
) -> Path:
    selected_preset = _require_preset(preset, "tests")
    return (
        _output(root, profile, selected_preset, runtime_binaries, state_directory)
        / "Tests"
        / preset_cache_string(selected_preset, "DURIN_RUNTIME_VARIANT")
        / "Bin"
    )


def _saved(
    root: Path,
    profile: BuildProfile | None,
    preset: ConfigurePreset | None,
    runtime_binaries: Path,
    state_directory: Path,
) -> Path:
    return _runtime(root, profile, preset, runtime_binaries, state_directory) / "Saved"


def _configs(
    root: Path,
    profile: BuildProfile | None,
    preset: ConfigurePreset | None,
    runtime_binaries: Path,
    state_directory: Path,
) -> Path:
    return _saved(root, profile, preset, runtime_binaries, state_directory) / "Configs"


def _runtime_logs(
    root: Path,
    profile: BuildProfile | None,
    preset: ConfigurePreset | None,
    runtime_binaries: Path,
    state_directory: Path,
) -> Path:
    return _saved(root, profile, preset, runtime_binaries, state_directory) / "Logs"


def _logs(
    root: Path,
    _profile: BuildProfile | None,
    _preset: ConfigurePreset | None,
    _runtime_binaries: Path,
    state_directory: Path,
) -> Path:
    return root / state_directory / "logs"


LOCATION_SPECS = (
    LocationSpec("root", (), False, _root),
    LocationSpec(
        "build",
        (),
        True,
        _build,
        "Configure the selected preset first with configure.",
    ),
    LocationSpec("binaries", ("bin",), False, _binaries),
    LocationSpec(
        "output",
        (),
        True,
        _output,
        "Build the selected preset first with build --target all.",
    ),
    LocationSpec(
        "runtime",
        (),
        True,
        _runtime,
        "Build the complete runtime first with build --target all.",
    ),
    LocationSpec(
        "saved",
        (),
        True,
        _saved,
        "Run the selected runtime once to create its Saved directory.",
    ),
    LocationSpec(
        "configs",
        (),
        True,
        _configs,
        "Build the complete runtime first with build --target all.",
    ),
    LocationSpec(
        "runtime-logs",
        ("engine-logs",),
        True,
        _runtime_logs,
        "Run the selected runtime once to create its log directory.",
    ),
    LocationSpec(
        "tests",
        (),
        True,
        _tests,
        "Build a native test target first with test <target>.",
    ),
    LocationSpec(
        "logs",
        (),
        False,
        _logs,
        "Run a command that captures child output first.",
    ),
)

_LOCATIONS_BY_NAME = {
    candidate.casefold(): spec
    for spec in LOCATION_SPECS
    for candidate in (spec.name, *spec.aliases)
}


def location_names() -> tuple[str, ...]:
    return tuple(spec.name for spec in LOCATION_SPECS)


def resolve_location(
    name: str,
    *,
    profile: BuildProfile | None = None,
    preset: ConfigurePreset | None = None,
    root: Path | None = None,
    runtime_binaries_directory: Path | None = None,
    state_directory: Path | None = None,
) -> ResolvedLocation:
    repository = None
    if root is None or runtime_binaries_directory is None or state_directory is None:
        repository = RepositoryContext.load()
    root = root or repository.root
    runtime_binaries_directory = (
        runtime_binaries_directory
        or repository.config.paths.runtime_binaries_directory
    )
    state_directory = state_directory or repository.config.paths.state_directory
    spec = _LOCATIONS_BY_NAME.get(name.casefold())
    if spec is None:
        choices = ", ".join(location_names())
        raise BuildToolError(f'Unknown location "{name}". Available locations: {choices}.')
    path = spec.resolver(
        root,
        profile,
        preset,
        runtime_binaries_directory,
        state_directory,
    )
    return ResolvedLocation(spec, path, path.is_dir())


def resolve_all_locations(
    *,
    profile: BuildProfile | None = None,
    preset: ConfigurePreset | None = None,
    root: Path | None = None,
    runtime_binaries_directory: Path | None = None,
    state_directory: Path | None = None,
) -> tuple[ResolvedLocation, ...]:
    repository = None
    if root is None or runtime_binaries_directory is None or state_directory is None:
        repository = RepositoryContext.load()
    root = root or repository.root
    runtime_binaries_directory = (
        runtime_binaries_directory
        or repository.config.paths.runtime_binaries_directory
    )
    state_directory = state_directory or repository.config.paths.state_directory
    return tuple(
        resolve_location(
            spec.name,
            profile=profile,
            preset=preset,
            root=root,
            runtime_binaries_directory=runtime_binaries_directory,
            state_directory=state_directory,
        )
        for spec in LOCATION_SPECS
    )
