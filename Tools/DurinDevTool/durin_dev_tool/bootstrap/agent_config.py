#!/usr/bin/env python3
from __future__ import annotations

import json
import shutil
from pathlib import Path
from typing import Sequence

from ..configuration import load_repository_config


REPOSITORY_CONFIG = load_repository_config()
CONFIG_RELATIVE_PATH = REPOSITORY_CONFIG.paths.local_build_config
TEMPLATE_RELATIVE_PATH = REPOSITORY_CONFIG.paths.local_build_config_template


class AgentConfigError(RuntimeError):
    pass


def config_path(repo_root: Path) -> Path:
    return repo_root / CONFIG_RELATIVE_PATH


def template_path(repo_root: Path) -> Path:
    return repo_root / TEMPLATE_RELATIVE_PATH


def ensure_agent_config(repo_root: Path, *, dry_run: bool = False) -> Path:
    target = config_path(repo_root)
    template = template_path(repo_root)

    if target.is_file() and not target.is_symlink():
        print(f'Agent build config already exists: "{target}"')
        return target
    if target.exists() or target.is_symlink():
        raise AgentConfigError(f'Agent build config path is not a regular file: "{target}"')
    if not template.is_file():
        raise AgentConfigError(f'Agent build config template does not exist: "{template}"')

    if dry_run:
        print(f'[dry-run] create Agent build config: "{target}" <- "{template}"')
        return target

    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(template, target)
    print(f'Created Agent build config: "{target}"')
    print("Optional fields may be filled when automatic tool or environment detection is not sufficient.")
    return target


def save_toolchain_config(
    repo_root: Path,
    *,
    cmake_command: str,
    environment_script: Path,
    environment_arguments: Sequence[str],
) -> Path:
    target = config_path(repo_root)
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
    toolchain["environmentScript"] = environment_script.as_posix()
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
    print(f'Updated toolchain settings in Agent build config: "{target}"')
    return target
