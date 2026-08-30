from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from ..context import RepositoryContext


PROFILE_ENV_VAR = "DURIN_AGENT_BUILD_PROFILE"
JOBS_ENV_VAR = "DURIN_AGENT_JOBS"
CMAKE_ENV_VARS = ("DURIN_CMAKE_COMMAND", "DURIN_CMAKE_PATH")


@dataclass(frozen=True)
class BuildPaths:
    root: Path
    profile_file: Path
    preset_file: Path
    local_config_file: Path
    state_directory: Path
    lock_directory: Path

    @classmethod
    def from_repository(cls, repository: RepositoryContext) -> "BuildPaths":
        paths = repository.config.paths
        return cls(
            root=repository.root,
            profile_file=repository.resolve(paths.build_profiles),
            preset_file=repository.resolve(paths.cmake_presets),
            local_config_file=repository.resolve(paths.local_build_config),
            state_directory=repository.resolve(paths.state_directory),
            lock_directory=repository.resolve(paths.lock_directory),
        )


def default_build_paths() -> BuildPaths:
    """Call-time compatibility paths for direct service consumers."""
    return BuildPaths.from_repository(RepositoryContext.load())
