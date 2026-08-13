from __future__ import annotations

from dataclasses import dataclass
from typing import Mapping

from ..context import RepositoryContext
from .models import Action, BuildProfile, ConfigurePreset, LocalConfig
from .requests import BuildRequest, ConcreteRequest, NativeTestRequest, RebuildRequest


@dataclass
class BuildContext:
    request: ConcreteRequest
    config: LocalConfig
    profile: BuildProfile
    presets: Mapping[str, ConfigurePreset]
    preset: ConfigurePreset
    current_host: str
    cmake: str = ""
    jobs: int = 0
    environment: dict[str, str] | None = None
    resolved_test_targets: tuple[str, ...] = ()
    test_selection_explanation: str = ""
    repository: RepositoryContext | None = None

    @property
    def target(self) -> str:
        if isinstance(self.request, (BuildRequest, RebuildRequest)):
            return self.request.target or "all"
        if isinstance(self.request, NativeTestRequest):
            return "all" if self.request.target.casefold() == "all" else self.request.target
        return ""


def create_build_context(
    request: ConcreteRequest,
    *,
    repository: RepositoryContext,
) -> BuildContext:
    from .config import (
        BuildPaths,
        host_name,
        load_configure_presets,
        load_local_config,
        load_profiles,
        select_preset,
        select_profile,
    )
    from .core import normalize_run_request, validate_request

    paths = BuildPaths.from_repository(repository)
    config = load_local_config(paths.local_config_file)
    if request.environment_setup:
        config = config.with_environment_script(request.environment_setup)
    profiles = load_profiles(paths.profile_file)
    current_host = host_name()
    profile = select_profile(
        profiles,
        requested=request.profile,
        configured=config.default_build_profile,
        current_host=current_host,
        profile_file=paths.profile_file,
    )
    presets = load_configure_presets(paths.preset_file)
    preset = select_preset(profile, presets, requested=request.preset, preset_file=paths.preset_file)
    request = normalize_run_request(
        request,
        preset=preset,
        root=repository.root,
        default_project=repository.config.paths.default_game_project,
    )
    validate_request(request, preset)
    return BuildContext(request, config, profile, presets, preset, current_host, repository=repository)


def derive_build_context(base: BuildContext, request: ConcreteRequest) -> BuildContext:
    from .config import BuildPaths, default_build_paths, select_preset
    from .core import normalize_run_request, validate_request

    paths = BuildPaths.from_repository(base.repository) if base.repository else default_build_paths()
    preset = select_preset(base.profile, base.presets, requested=request.preset, preset_file=paths.preset_file)
    request = normalize_run_request(
        request,
        preset=preset,
        root=paths.root,
        default_project=(
            base.repository.config.paths.default_game_project
            if base.repository
            else RepositoryContext.load(paths.root).config.paths.default_game_project
        ),
    )
    validate_request(request, preset)
    return BuildContext(
        request=request,
        config=base.config,
        profile=base.profile,
        presets=base.presets,
        preset=preset,
        current_host=base.current_host,
        cmake=base.cmake,
        jobs=base.jobs if request.jobs is None else request.jobs,
        environment=base.environment,
        repository=base.repository,
    )
