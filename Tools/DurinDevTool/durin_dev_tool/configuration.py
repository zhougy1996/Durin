"""Tracked repository configuration for DurinDevTool."""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Mapping

from .errors import DevToolError
from .repository import discover_repository_root


CONFIG_RELATIVE_PATH = Path("Tools") / "DurinDevTool" / "DevTool.json"
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
FEATURE_NAMES = {
    "setup",
    "build",
    "scaffolding",
    "dependencies",
    "documentation",
    "worktrees",
}
WORKTREE_PATH_FIELDS = {
    "agentDirectory": "agent_directory",
    "vscodeDirectory": "vscode_directory",
    "pythonEnvironment": "python_environment",
    "externalDirectory": "external_directory",
}


class RepositoryConfigError(DevToolError):
    pass


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
    features: Mapping[str, bool]

    def resolve(self, path: Path) -> Path:
        return self.repository_root / path

    def feature_enabled(self, name: str) -> bool:
        if name not in FEATURE_NAMES:
            raise RepositoryConfigError(f'Unknown DurinDevTool feature "{name}".')
        return self.features[name]


def _load_json_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise RepositoryConfigError(
            f'DurinDevTool repository config was not found: "{path}"'
        ) from exc
    except json.JSONDecodeError as exc:
        raise RepositoryConfigError(
            "DurinDevTool repository config contains invalid JSON at "
            f'line {exc.lineno}, column {exc.colno}: "{path}"'
        ) from exc
    except OSError as exc:
        raise RepositoryConfigError(
            f'Could not read DurinDevTool repository config "{path}": {exc}'
        ) from exc
    if not isinstance(value, dict):
        raise RepositoryConfigError(
            f'DurinDevTool repository config must contain a JSON object: "{path}"'
        )
    return value


def _require_object(
    container: Mapping[str, Any],
    key: str,
    *,
    label: str,
) -> dict[str, Any]:
    value = container.get(key)
    if not isinstance(value, dict):
        raise RepositoryConfigError(f'{label} field "{key}" must be an object.')
    return value


def _reject_unknown(
    container: Mapping[str, Any],
    allowed: set[str],
    *,
    label: str,
) -> None:
    unknown = sorted(set(container) - allowed)
    if unknown:
        raise RepositoryConfigError(
            f'{label} contains unknown field(s): {", ".join(unknown)}.'
        )


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
    raw = _load_json_object(config_path)
    _reject_unknown(
        raw,
        {"$schema", "version", "paths", "worktrees", "features"},
        label="DurinDevTool repository config",
    )
    if raw.get("version") != 1:
        raise RepositoryConfigError(
            'DurinDevTool repository config field "version" must be 1.'
        )

    raw_paths = _require_object(
        raw,
        "paths",
        label="DurinDevTool repository config",
    )
    _reject_unknown(
        raw_paths,
        set(PATH_FIELDS),
        label="DurinDevTool repository config paths",
    )
    missing_paths = sorted(set(PATH_FIELDS) - set(raw_paths))
    if missing_paths:
        raise RepositoryConfigError(
            "DurinDevTool repository config paths is missing field(s): "
            + ", ".join(missing_paths)
            + "."
        )
    path_values = {
        attribute: _repository_path(raw_paths[field], field=f"paths.{field}")
        for field, attribute in PATH_FIELDS.items()
    }

    raw_worktrees = _require_object(
        raw,
        "worktrees",
        label="DurinDevTool repository config",
    )
    _reject_unknown(
        raw_worktrees,
        set(WORKTREE_PATH_FIELDS),
        label="DurinDevTool repository config worktrees",
    )
    missing_worktrees = sorted(set(WORKTREE_PATH_FIELDS) - set(raw_worktrees))
    if missing_worktrees:
        raise RepositoryConfigError(
            "DurinDevTool repository config worktrees is missing field(s): "
            + ", ".join(missing_worktrees)
            + "."
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

    raw_features = _require_object(
        raw,
        "features",
        label="DurinDevTool repository config",
    )
    _reject_unknown(
        raw_features,
        FEATURE_NAMES,
        label="DurinDevTool repository config features",
    )
    missing_features = sorted(FEATURE_NAMES - set(raw_features))
    if missing_features:
        raise RepositoryConfigError(
            "DurinDevTool repository config features is missing field(s): "
            + ", ".join(missing_features)
            + "."
        )
    if any(not isinstance(value, bool) for value in raw_features.values()):
        raise RepositoryConfigError(
            "Every DurinDevTool repository config feature must be a boolean."
        )

    return RepositoryConfig(
        repository_root=root,
        paths=RepositoryPaths(**path_values),
        worktrees=worktrees,
        features=dict(raw_features),
    )
