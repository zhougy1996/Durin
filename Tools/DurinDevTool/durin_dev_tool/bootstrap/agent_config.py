#!/usr/bin/env python3
from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Sequence

from ..context import CommandIO, RepositoryContext
from ..errors import DevToolError


class AgentConfigError(DevToolError):
    pass


def _repository(
    repo_root: Path,
    repository: RepositoryContext | None = None,
) -> RepositoryContext:
    return repository or RepositoryContext.load().at_root(repo_root)


def config_path(
    repo_root: Path,
    repository: RepositoryContext | None = None,
) -> Path:
    repository = _repository(repo_root, repository)
    return repo_root / repository.config.paths.local_build_config


def template_path(
    repo_root: Path,
    repository: RepositoryContext | None = None,
) -> Path:
    repository = _repository(repo_root, repository)
    return repo_root / repository.config.paths.local_build_config_template


def ensure_agent_config(
    repo_root: Path,
    repository: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
    *,
    dry_run: bool = False,
) -> Path:
    repository = _repository(repo_root, repository)
    command_io = command_io or CommandIO.system()
    target = config_path(repo_root, repository)
    template = template_path(repo_root, repository)

    if target.is_file() and not target.is_symlink():
        command_io.out(f'Agent build config already exists: "{target}"')
        return target
    if target.exists() or target.is_symlink():
        raise AgentConfigError(f'Agent build config path is not a regular file: "{target}"')
    if not template.is_file():
        raise AgentConfigError(f'Agent build config template does not exist: "{template}"')

    if dry_run:
        command_io.out(f'[dry-run] create Agent build config: "{target}" <- "{template}"')
        return target

    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(template, target)
    command_io.out(f'Created Agent build config: "{target}"')
    command_io.out(
        "Optional fields may be filled when automatic tool or environment detection is not sufficient."
    )
    return target


def save_toolchain_config(
    repo_root: Path,
    repository: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
    *,
    cmake_command: str,
    environment_script: Path | None,
    environment_arguments: Sequence[str],
) -> Path:
    repository = _repository(repo_root, repository)
    command_io = command_io or CommandIO.system()
    target = config_path(repo_root, repository)
    if not target.is_file() or target.is_symlink():
        raise AgentConfigError(
            f'Agent build config is not a regular file: "{target}"'
        )
    try:
        raw = json.loads(target.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AgentConfigError(f'Could not read Agent build config "{target}": {exc}') from exc
    if not isinstance(raw, dict):
        raise AgentConfigError(f'Agent build config must contain an object: "{target}"')

    cmake = raw.setdefault("cmake", {})
    toolchain = raw.setdefault("toolchain", {})
    if not isinstance(cmake, dict) or not isinstance(toolchain, dict):
        raise AgentConfigError(
            f'Agent build config contains invalid cmake or toolchain sections: "{target}"'
        )
    cmake["command"] = cmake_command
    toolchain["environmentScript"] = (
        environment_script.as_posix() if environment_script is not None else None
    )
    toolchain["environmentArguments"] = list(environment_arguments)

    temporary = target.with_name(f".{target.name}.tmp")
    try:
        temporary.write_text(
            json.dumps(raw, indent=2) + "\n",
            encoding="utf-8",
        )
        temporary.replace(target)
    except OSError as exc:
        raise AgentConfigError(f'Could not write Agent build config "{target}": {exc}') from exc
    finally:
        if temporary.exists():
            temporary.unlink()
    command_io.out(f'Updated toolchain settings in Agent build config: "{target}"')
    return target
