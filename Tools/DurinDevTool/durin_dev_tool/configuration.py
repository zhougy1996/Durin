"""Tracked repository configuration for DurinDevTool."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

from .errors import DevToolError
from .json_contract import JsonContractError, decode_json_contract
from .repository import discover_repository_root


CONFIG_RELATIVE_PATH = Path("Tools") / "DurinDevTool" / "DevTool.json"
CONFIG_FIELDS = {"version", "paths", "worktrees"}
CONFIG_OPTIONAL_FIELDS = {"$schema"}
PATH_FIELDS = {
    "cmakePresets": "cmake_presets",
    "buildProfiles": "build_profiles",
    "localBuildConfig": "local_build_config",
    "localBuildConfigTemplate": "local_build_config_template",
    "stateDirectory": "state_directory",
    "lockDirectory": "lock_directory",
    "runtimeBinariesDirectory": "runtime_binaries_directory",
    "defaultGameProject": "default_game_project",
    "vscodeTemplates": "vscode_templates",
    "scaffoldingTemplates": "scaffolding_templates",
    "thirdPartyManifests": "third_party_manifests",
}
WORKTREE_PATH_FIELDS = {
    "agentDirectory": "agent_directory",
    "vscodeDirectory": "vscode_directory",
    "pythonEnvironment": "python_environment",
    "externalDirectory": "external_directory",
}


class RepositoryConfigError(DevToolError):
    pass


def _strict_object(
    value: Any,
    *,
    label: str,
    required: set[str],
    optional: set[str] = frozenset(),
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise RepositoryConfigError(f"{label} must be an object.")
    keys = set(value)
    if missing := sorted(required - keys):
        raise RepositoryConfigError(f"{label} is missing required fields: {missing}.")
    if unexpected := sorted(keys - required - optional):
        raise RepositoryConfigError(f"{label} contains unexpected fields: {unexpected}.")
    return value


@dataclass(frozen=True)
class RepositoryPaths:
    cmake_presets: Path
    build_profiles: Path
    local_build_config: Path
    local_build_config_template: Path
    state_directory: Path
    lock_directory: Path
    runtime_binaries_directory: Path
    default_game_project: Path
    vscode_templates: Path
    scaffolding_templates: Path
    third_party_manifests: Path


@dataclass(frozen=True)
class WorktreePaths:
    agent_directory: Path
    vscode_directory: Path
    python_environment: Path
    external_directory: Path

    @property
    def shared_directories(self) -> tuple[Path, ...]:
        return (
            self.agent_directory,
            self.vscode_directory,
            self.python_environment,
            self.external_directory,
        )


@dataclass(frozen=True)
class RepositoryConfig:
    repository_root: Path
    paths: RepositoryPaths
    worktrees: WorktreePaths

    def resolve(self, path: Path) -> Path:
        return self.repository_root / path

def _repository_path(value: Any, *, field: str) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise RepositoryConfigError(
            f'DurinDevTool repository config field "{field}" must be a non-empty string.'
        )
    normalized = PurePosixPath(value.replace("\\", "/"))
    if (
        not normalized.parts
        or normalized.is_absolute()
        or ".." in normalized.parts
        or ":" in normalized.parts[0]
    ):
        raise RepositoryConfigError(
            f'DurinDevTool repository config field "{field}" must stay inside the repository.'
        )
    return Path(*normalized.parts)


def load_repository_config(
    repository_root: Path | None = None,
    *,
    path: Path | None = None,
) -> RepositoryConfig:
    root = (repository_root or discover_repository_root()).resolve()
    config_path = path or root / CONFIG_RELATIVE_PATH
    if not config_path.is_absolute():
        config_path = root / config_path
    try:
        text = config_path.read_text(encoding="utf-8")
        decoded = decode_json_contract(
            text,
            label="DurinDevTool repository config",
            source=f'"{config_path}"',
        )
    except (OSError, UnicodeError, JsonContractError) as exc:
        raise RepositoryConfigError(str(exc)) from exc
    raw = _strict_object(
        decoded,
        label="DurinDevTool repository config",
        required=CONFIG_FIELDS,
        optional=CONFIG_OPTIONAL_FIELDS,
    )
    if type(raw["version"]) is not int or raw["version"] != 1:
        raise RepositoryConfigError(
            'DurinDevTool repository config field "version" must be 1.'
        )
    if "$schema" in raw and not isinstance(raw["$schema"], str):
        raise RepositoryConfigError(
            'DurinDevTool repository config field "$schema" must be a string.'
        )
    raw_paths = _strict_object(
        raw["paths"],
        label='DurinDevTool repository config field "paths"',
        required=set(PATH_FIELDS),
    )
    path_values = {
        attribute: _repository_path(raw_paths[field], field=f"paths.{field}")
        for field, attribute in PATH_FIELDS.items()
    }

    raw_worktrees = _strict_object(
        raw["worktrees"],
        label='DurinDevTool repository config field "worktrees"',
        required=set(WORKTREE_PATH_FIELDS),
    )
    worktree_values = {
        attribute: _repository_path(raw_worktrees[field], field=f"worktrees.{field}")
        for field, attribute in WORKTREE_PATH_FIELDS.items()
    }
    worktrees = WorktreePaths(**worktree_values)
    if len(set(worktrees.shared_directories)) != len(worktrees.shared_directories):
        raise RepositoryConfigError(
            "DurinDevTool repository config worktree directories must be unique."
        )
    if path_values["local_build_config"].parent != worktrees.agent_directory:
        raise RepositoryConfigError(
            'DurinDevTool repository config field "paths.localBuildConfig" must '
            'be inside "worktrees.agentDirectory".'
        )

    return RepositoryConfig(
        repository_root=root,
        paths=RepositoryPaths(**path_values),
        worktrees=worktrees,
    )
