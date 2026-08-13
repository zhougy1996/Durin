"""Worktree preparation/removal planning, ordering, and rollback."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from ..bootstrap.preflight import validate_prerequisites
from ..context import CommandIO, RepositoryContext
from . import git, links
from .models import DetachedLink, Worktree, WorktreeToolError

SOURCE_ENVIRONMENT_NAMES = (
    "DURIN_WORKTREE_SOURCE",
    "DURIN_EXTERNAL_SOURCE",
    "DURIN_EXTERNAL_ROOT",
)


def run_preflight(
    target: Path,
    *,
    repository: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
) -> None:
    """Validate prerequisites for a prepared worktree checkout."""
    validate_prerequisites(
        target,
        repository_context=(repository or RepositoryContext.load()).at_root(target),
        command_io=command_io or CommandIO.system(),
    )


@dataclass(frozen=True)
class PlannedLink:
    source: Path
    target: Path
    spec: links.SharedDirectorySpec


@dataclass(frozen=True)
class WorktreePreparationPlan:
    worktree: Worktree
    source: Path
    link_type: str
    links: tuple[PlannedLink, ...]


def require_registered_linked_worktree(
    target: Path,
    worktrees: Sequence[Worktree],
    *,
    require_unlocked: bool = True,
) -> Worktree:
    matches = [worktree for worktree in worktrees if git.same_path(worktree.path, target)]
    if not matches:
        raise WorktreeToolError(f'Path is not a registered Git worktree: "{target}"')
    worktree = matches[0]
    if git.same_path(worktree.path, worktrees[0].path):
        raise WorktreeToolError("Refusing to operate on the main worktree.")
    if require_unlocked and worktree.locked:
        raise WorktreeToolError("Refusing to remove a locked worktree. Unlock it explicitly first.")
    return worktree


def _source_worktree(source_value: str | None, target: Path, worktrees: Sequence[Worktree]) -> Path:
    selected = source_value or next(
        (os.environ[name] for name in SOURCE_ENVIRONMENT_NAMES if os.environ.get(name)),
        str(worktrees[0].path),
    )
    path = Path(selected).expanduser().resolve()
    source = (path.parent.parent if path.name.lower() == "external" else path).resolve()
    if git.same_path(source, target):
        raise WorktreeToolError(f'Source worktree is the same as the target: "{source}"')
    return source


def plan_preparation(
    target: Path,
    *,
    source_value: str | None,
    link_type: str,
    repository: RepositoryContext,
    command_io: CommandIO,
) -> WorktreePreparationPlan:
    target = target.expanduser().absolute()
    worktrees = git.get_worktrees(repository, command_io)
    worktree = require_registered_linked_worktree(target, worktrees, require_unlocked=False)
    source = _source_worktree(source_value, worktree.path, worktrees)
    selected_link_type = links.choose_link_type(link_type)
    planned = tuple(
        PlannedLink(source / spec.relative_path, worktree.path / spec.relative_path, spec)
        for spec in links.shared_directory_specs(repository)
    )
    missing = [f'{item.spec.label}: "{item.source}"' for item in planned if not item.source.is_dir()]
    if missing:
        raise WorktreeToolError("Prepared source directories are missing:\n" + "\n".join(f"  {item}" for item in missing))
    for item in planned:
        target_path = item.target
        if item.spec.preserve_existing:
            backup = target_path.with_name(f"{target_path.name}.pre-link-backup")
            if (
                target_path.is_dir()
                and not links.is_link_like(target_path)
                and not links.is_empty_directory(target_path)
                and (backup.exists() or links.is_link_like(backup))
            ):
                raise WorktreeToolError(
                    f'Cannot preserve the existing {item.spec.label} directory because the backup path exists: "{backup}"'
                )
        elif target_path.exists() and not links.is_link_like(target_path) and not links.is_empty_directory(target_path):
            raise WorktreeToolError(
                f'Target {item.spec.label} already exists and is not an empty directory or link: "{target_path}"\n'
                "Move it aside manually if you really want to replace it."
            )
    return WorktreePreparationPlan(worktree, source, selected_link_type, planned)


def execute_preparation(
    plan: WorktreePreparationPlan,
    *,
    dry_run: bool,
    repository: RepositoryContext,
    command_io: CommandIO,
) -> None:
    command_io.out(f'Source worktree: "{plan.source}"')
    command_io.out(f'Target worktree: "{plan.worktree.path}"')
    command_io.out(f"Link type: {plan.link_type}")
    for item in plan.links:
        links.prepare_shared_directory_link(
            plan.source,
            plan.worktree.path,
            item.spec,
            link_type=plan.link_type,
            dry_run=dry_run,
            command_io=command_io,
        )
    if dry_run:
        command_io.out("Dry run complete; preflight was not run.")
        return
    command_io.stdout.flush()
    validate_prerequisites(
        plan.worktree.path,
        repository_context=repository.at_root(plan.worktree.path),
        command_io=command_io,
    )
    command_io.out(f'Prepared worktree: "{plan.worktree.path}"')


def prepare_worktree(
    target: Path,
    *,
    source_value: str | None,
    link_type: str,
    dry_run: bool,
    repository: RepositoryContext,
    command_io: CommandIO,
) -> None:
    plan = plan_preparation(
        target,
        source_value=source_value,
        link_type=link_type,
        repository=repository,
        command_io=command_io,
    )
    execute_preparation(plan, dry_run=dry_run, repository=repository, command_io=command_io)


def remove_worktree(
    target: Path,
    *,
    force: bool,
    dry_run: bool,
    repository: RepositoryContext,
    command_io: CommandIO,
) -> None:
    worktrees = git.get_worktrees(repository, command_io)
    worktree = require_registered_linked_worktree(target.expanduser().absolute(), worktrees)
    git.require_clean_worktree(worktree, force=force, repository=repository, command_io=command_io)
    directory_links = links.validate_directory_links(worktree, repository)
    command_io.out(f'Worktree to remove: "{worktree.path}"')
    for link in directory_links:
        command_io.out(f'  detach "{link}" -> "{link.resolve(strict=False)}"')
    if dry_run:
        command_io.out("Dry run complete; no links or worktrees were removed.")
        return

    detached: list[DetachedLink] = []
    try:
        for link in directory_links:
            detached.append(links.detach_link(link))
        command = ["worktree", "remove"]
        if force:
            command.append("--force")
        command.append(str(worktree.path))
        result = git.git_command(command, repository=repository, command_io=command_io)
        git.require_git_success(result, "Removing Git worktree")
    except (OSError, WorktreeToolError) as exc:
        restore_errors: list[str] = []
        for link in reversed(detached):
            if link.path.exists() or links.is_link_like(link.path):
                continue
            try:
                links.restore_link(link)
            except (OSError, WorktreeToolError) as restore_exc:
                restore_errors.append(str(restore_exc))
        if restore_errors:
            raise WorktreeToolError(
                f"{exc}\nAdditionally, link restoration failed:\n" + "\n".join(restore_errors)
            ) from exc
        raise
    command_io.out(f'Removed worktree safely: "{worktree.path}"')
