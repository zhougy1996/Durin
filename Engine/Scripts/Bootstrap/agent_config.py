#!/usr/bin/env python3
from __future__ import annotations

import shutil
from pathlib import Path


CONFIG_RELATIVE_PATH = Path(".agents") / "build-config.json"
TEMPLATE_RELATIVE_PATH = Path("Documentation") / "Setup" / "TP_AGENT_BUILD_CONFIG.json"


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
