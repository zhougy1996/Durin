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


def sync_agent_config(source_repo: Path, target_repo: Path, *, dry_run: bool = False) -> Path:
    source = config_path(source_repo)
    target = config_path(target_repo)

    if not source.is_file():
        print(
            f'Agent build config was not found in source worktree "{source_repo}"; '
            "creating the target config from its template."
        )
        return ensure_agent_config(target_repo, dry_run=dry_run)

    if target.is_file() and not target.is_symlink():
        try:
            if source.read_bytes() == target.read_bytes():
                print(f'Agent build config is already synchronized with "{source}".')
                return target
        except OSError as exc:
            raise AgentConfigError(f'Could not compare Agent build configs: {exc}') from exc
    elif target.exists() and not target.is_symlink():
        raise AgentConfigError(f'Agent build config path is not a regular file: "{target}"')

    if dry_run:
        print(f'[dry-run] copy Agent build config: "{target}" <- "{source}"')
        return target

    target.parent.mkdir(parents=True, exist_ok=True)
    if target.is_symlink():
        target.unlink()
    shutil.copy2(source, target)
    print(f'Copied Agent build config: "{target}" <- "{source}"')
    return target
