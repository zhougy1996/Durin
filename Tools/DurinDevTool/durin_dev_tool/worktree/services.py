#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import stat
import subprocess
import sys
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Sequence

from ..bootstrap.preflight import validate_prerequisites
from ..build.config import BuildToolError, load_local_config
from ..configuration import load_repository_config
from ..repository import discover_repository_root


REPO_ROOT = discover_repository_root()
REPOSITORY_CONFIG = load_repository_config(REPO_ROOT)
AGENT_DIRECTORY = REPOSITORY_CONFIG.worktrees.agent_directory
VSCODE_DIRECTORY = REPOSITORY_CONFIG.worktrees.vscode_directory
PYTHON_ENVIRONMENT = REPOSITORY_CONFIG.worktrees.python_environment
EXTERNAL_DIRECTORY = REPOSITORY_CONFIG.worktrees.external_directory
SHARED_DIRECTORY_PATHS = REPOSITORY_CONFIG.worktrees.shared_directories
SOURCE_ENVIRONMENT_NAMES = (
    "DURIN_WORKTREE_SOURCE",
    "DURIN_EXTERNAL_SOURCE",
    "DURIN_EXTERNAL_ROOT",
)


class WorktreeToolError(RuntimeError):
    pass


@dataclass(frozen=True)
class Worktree:
    path: Path
    branch: str | None = None
    locked: bool = False


@dataclass(frozen=True)
class DetachedLink:
    path: Path
    target: Path
    kind: str


@contextmanager
def repository_paths(repository_root: Path) -> Iterator[None]:
    global REPO_ROOT
    previous_root = REPO_ROOT
    REPO_ROOT = repository_root.resolve()
    try:
        yield
    finally:
        REPO_ROOT = previous_root


def is_windows() -> bool:
    return os.name == "nt"


def normalized_path(path: Path) -> str:
    return os.path.normcase(os.path.abspath(path))


def same_path(left: Path, right: Path) -> bool:
    try:
        return os.path.samefile(left, right)
    except OSError:
        return normalized_path(left) == normalized_path(right)


def git_command(
    arguments: Sequence[str],
    *,
    cwd: Path | None = None,
    capture_output: bool = True,
) -> subprocess.CompletedProcess[str]:
    cwd = cwd or REPO_ROOT
    safe_paths = [REPO_ROOT]
    if not same_path(cwd, REPO_ROOT):
        safe_paths.append(cwd)
    command = [
        "git.exe" if is_windows() else "git",
    ]
    for safe_path in safe_paths:
        safe_path_text = str(safe_path).replace(os.sep, "/")
        command.extend(["-c", f"safe.directory={safe_path_text}"])
    command.extend(["-c", "core.quotePath=false", "-C", str(cwd), *arguments])
    try:
        return subprocess.run(
            command,
            check=False,
            capture_output=capture_output,
            text=True,
        )
    except OSError as exc:
        raise WorktreeToolError(f'Could not run Git: {exc}') from exc


def require_git_success(result: subprocess.CompletedProcess[str], operation: str) -> None:
    if result.returncode == 0:
        return
    detail = (result.stderr or result.stdout or "").strip()
    suffix = f"\n{detail}" if detail else ""
    raise WorktreeToolError(f"{operation} failed with exit code {result.returncode}.{suffix}")


def parse_worktrees(output: str) -> list[Worktree]:
    worktrees: list[Worktree] = []
    current_path: Path | None = None
    current_branch: str | None = None
    current_locked = False

    def append_current() -> None:
        if current_path is not None:
            worktrees.append(Worktree(current_path, current_branch, current_locked))

    for line in output.splitlines():
        if line.startswith("worktree "):
            append_current()
            current_path = Path(line.removeprefix("worktree "))
            current_branch = None
            current_locked = False
        elif line.startswith("branch refs/heads/"):
            current_branch = line.removeprefix("branch refs/heads/")
        elif line == "locked" or line.startswith("locked "):
            current_locked = True
    append_current()
    return worktrees


def get_worktrees() -> list[Worktree]:
    result = git_command(["worktree", "list", "--porcelain"])
    require_git_success(result, "Enumerating Git worktrees")
    worktrees = parse_worktrees(result.stdout)
    if not worktrees:
        raise WorktreeToolError("Git did not report any worktrees.")
    return worktrees


def display_worktrees(worktrees: Sequence[Worktree]) -> None:
    print(f"Durin worktrees ({len(worktrees)}):")
    for worktree in worktrees:
        branch = worktree.branch or "detached"
        locked = " [locked]" if worktree.locked else ""
        print(f"  [{branch}] {worktree.path}{locked}")


def ordered_worktrees(worktrees: Sequence[Worktree]) -> list[Worktree]:
    return [
        *(worktree for worktree in worktrees if worktree.branch == "main"),
        *(worktree for worktree in worktrees if worktree.branch == "dev"),
        *(worktree for worktree in worktrees if worktree.branch not in {"main", "dev"}),
    ]


def environment_arguments(worktree: Path) -> list[str]:
    config_path = worktree / REPOSITORY_CONFIG.paths.local_build_config
    if not config_path.is_file():
        print(
            f"WARNING: Agent config is missing for worktree '{worktree}'. "
            "Opening it without a configured environment; "
            "run 'DevTool worktree prepare' there.",
            file=sys.stderr,
        )
        return []
    try:
        config = load_local_config(config_path)
    except (BuildToolError, OSError) as exc:
        raise WorktreeToolError(f'Could not read Agent config "{config_path}": {exc}') from exc

    setup_script = config.environment_setup.script
    if not setup_script:
        return []
    setup_path = Path(setup_script)
    if not setup_path.is_file():
        raise WorktreeToolError(f'Environment setup script does not exist: "{setup_path}"')
    return [str(setup_path), *config.environment_setup.arguments]


def add_terminal_pane_arguments(
    arguments: list[str],
    *,
    action: str,
    worktree: Path,
    environment: Sequence[str],
    split_direction: str | None = None,
) -> None:
    arguments.append(action)
    if split_direction:
        arguments.append(split_direction)
    arguments.extend(
        [
            "--startingDirectory",
            str(worktree),
            "--title",
            worktree.name,
            "cmd.exe",
            "/k",
            *environment,
        ]
    )


def terminal_arguments(worktrees: Sequence[Worktree]) -> list[str]:
    arguments = ["-w", "new"]
    environments = {worktree.path: environment_arguments(worktree.path) for worktree in worktrees}
    for index, worktree in enumerate(worktrees):
        position = index % 4
        if index:
            arguments.append(";")
        if position == 0:
            add_terminal_pane_arguments(
                arguments,
                action="new-tab",
                worktree=worktree.path,
                environment=environments[worktree.path],
            )
        elif position == 1:
            add_terminal_pane_arguments(
                arguments,
                action="split-pane",
                split_direction="-V",
                worktree=worktree.path,
                environment=environments[worktree.path],
            )
        elif position == 2:
            add_terminal_pane_arguments(
                arguments,
                action="split-pane",
                split_direction="-H",
                worktree=worktree.path,
                environment=environments[worktree.path],
            )
        else:
            # The third pane is focused after it is created. Walk back twice in
            # creation order to select the original pane before splitting it.
            # "first" means the first leaf in Terminal's pane tree, whose order
            # does not necessarily match pane creation order and can produce a
            # column with three panes instead of a 2 x 2 grid.
            arguments.extend(
                [
                    "move-focus",
                    "previousInOrder",
                    ";",
                    "move-focus",
                    "previousInOrder",
                    ";",
                ]
            )
            add_terminal_pane_arguments(
                arguments,
                action="split-pane",
                split_direction="-H",
                worktree=worktree.path,
                environment=environments[worktree.path],
            )
    return arguments


def open_worktree_terminals(*, dry_run: bool) -> None:
    worktrees = ordered_worktrees(get_worktrees())
    display_worktrees(worktrees)
    arguments = terminal_arguments(worktrees)
    print("Layout: up to four panes per tab (2 x 2).")
    if dry_run:
        print("Dry run complete; Windows Terminal was not opened.")
        return
    try:
        result = subprocess.run(["wt.exe", *arguments], check=False)
    except OSError as exc:
        raise WorktreeToolError(
            "wt.exe was not found. Install Windows Terminal or enable its app execution alias."
        ) from exc
    if result.returncode != 0:
        raise WorktreeToolError(f"Windows Terminal exited with code {result.returncode}.")


def add_worktree(args: argparse.Namespace) -> None:
    target = Path(args.path).expanduser().absolute()
    command = ["worktree", "add"]
    if args.branch:
        command.extend(["-b", args.branch])
    if args.detach:
        command.append("--detach")
    command.append(str(target))
    if args.commit_ish:
        command.append(args.commit_ish)

    result = git_command(command, capture_output=False)
    require_git_success(result, "Adding Git worktree")

    try:
        prepare_registered_worktree(
            target,
            source_value=args.source,
            link_type=args.link_type,
            dry_run=False,
        )
    except WorktreeToolError as exc:
        raise WorktreeToolError(
            f'Worktree was created at "{target}", but preparation failed.\n{exc}\n'
            f'Fix the reported problem and run DevTool worktree prepare "{target}".'
        ) from exc
    print(f'Created and prepared worktree: "{target}"')


def is_reparse_point(path: Path) -> bool:
    if not is_windows():
        return path.is_symlink()
    try:
        return bool(path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    except (AttributeError, OSError):
        return path.is_symlink()


def is_link_like(path: Path) -> bool:
    return path.is_symlink() or is_reparse_point(path)


def choose_link_type(requested: str) -> str:
    if requested == "auto":
        return "junction" if is_windows() else "symlink"
    return requested


def resolve_source_worktree(path_value: str) -> Path:
    path = Path(path_value).expanduser().resolve()
    candidate = path.parent.parent if path.name.lower() == "external" else path
    return candidate.resolve()


def preparation_source(
    source_value: str | None,
    *,
    target: Path,
    worktrees: Sequence[Worktree],
) -> Path:
    selected = source_value
    if not selected:
        selected = next(
            (os.environ[name] for name in SOURCE_ENVIRONMENT_NAMES if os.environ.get(name)),
            str(worktrees[0].path),
        )
    source = resolve_source_worktree(selected)
    if same_path(source, target):
        raise WorktreeToolError(f'Source worktree is the same as the target: "{source}"')
    return source


def is_empty_directory(path: Path) -> bool:
    return path.is_dir() and not any(path.iterdir())


def remove_link_or_empty_directory(path: Path, *, dry_run: bool) -> None:
    if dry_run:
        print(f'[dry-run] remove "{path}"')
        return
    if is_link_like(path) or is_empty_directory(path):
        path.rmdir()
        return
    raise WorktreeToolError(f'Refusing to remove non-empty real directory: "{path}"')


def create_directory_link(
    source: Path,
    target: Path,
    *,
    link_type: str,
    dry_run: bool,
) -> None:
    if link_type == "junction":
        if not is_windows():
            raise WorktreeToolError("Directory junctions are only supported on Windows.")
        command = ["cmd.exe", "/d", "/c", "mklink", "/J", str(target), str(source)]
        if dry_run:
            print(f"[dry-run] {' '.join(command)}")
            return
        result = subprocess.run(command, check=False, capture_output=True, text=True)
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "").strip()
            raise WorktreeToolError(
                f'Could not create junction "{target}" -> "{source}": {detail}'
            )
        return
    if link_type == "symlink":
        if dry_run:
            print(f'[dry-run] symlink "{target}" -> "{source}"')
            return
        os.symlink(source, target, target_is_directory=True)
        return
    raise WorktreeToolError(f"Unsupported link type: {link_type}")


def linked_to(path: Path, expected_target: Path) -> bool:
    return (
        path.exists()
        and is_link_like(path)
        and same_path(path.resolve(strict=False), expected_target.resolve(strict=False))
    )


def prepare_directory_link(
    source: Path,
    target: Path,
    *,
    label: str,
    link_type: str,
    dry_run: bool,
) -> None:
    source = source.expanduser().resolve()
    target = target.expanduser().absolute()
    if not source.is_dir():
        raise WorktreeToolError(f'Source {label} directory does not exist: "{source}"')
    if linked_to(target, source):
        print(f'{label} is already linked to "{source}".')
        return
    if target.exists() or is_link_like(target):
        if is_link_like(target) or is_empty_directory(target):
            remove_link_or_empty_directory(target, dry_run=dry_run)
        else:
            raise WorktreeToolError(
                f'Target {label} already exists and is not an empty directory or link: "{target}"\n'
                "Move it aside manually if you really want to replace it."
            )
    if dry_run:
        print(f'[dry-run] link {label}: "{target}" -> "{source}"')
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
    create_directory_link(source, target, link_type=link_type, dry_run=dry_run)
    print(f'Linked {label}: "{target}" -> "{source}"')


def prepare_agent_link(
    source_root: Path,
    target_root: Path,
    *,
    link_type: str,
    dry_run: bool,
) -> None:
    source = (source_root / AGENT_DIRECTORY).resolve()
    target = (target_root / AGENT_DIRECTORY).absolute()
    backup = target.with_name(f"{target.name}.pre-link-backup")
    if not source.is_dir():
        raise WorktreeToolError(
            f'Source {AGENT_DIRECTORY.as_posix()} directory does not exist: "{source}"'
        )
    if target.is_dir() and not is_link_like(target) and not is_empty_directory(target):
        if backup.exists() or is_link_like(backup):
            raise WorktreeToolError(
                f'Cannot preserve the existing Agent directory because the backup path exists: "{backup}"'
            )
        print(f'Preserving existing {AGENT_DIRECTORY.as_posix()}: "{target}" -> "{backup}"')
        if dry_run:
            print(f'[dry-run] move "{target}" -> "{backup}"')
            print(
                f'[dry-run] link {AGENT_DIRECTORY.as_posix()}: "{target}" -> "{source}"'
            )
            create_directory_link(source, target, link_type=link_type, dry_run=True)
            return
        target.rename(backup)
    prepare_directory_link(
        source,
        target,
        label=AGENT_DIRECTORY.as_posix(),
        link_type=link_type,
        dry_run=dry_run,
    )


def prepare_vscode_link(
    source_root: Path,
    target_root: Path,
    *,
    link_type: str,
    dry_run: bool,
) -> None:
    source = (source_root / VSCODE_DIRECTORY).resolve()
    target = (target_root / VSCODE_DIRECTORY).absolute()
    backup = target.with_name(f"{target.name}.pre-link-backup")
    if not source.is_dir():
        raise WorktreeToolError(
            f'Source {VSCODE_DIRECTORY.as_posix()} directory does not exist: "{source}"'
        )
    if target.is_dir() and not is_link_like(target) and not is_empty_directory(target):
        if backup.exists() or is_link_like(backup):
            raise WorktreeToolError(
                f'Cannot preserve the existing VS Code directory because the backup path exists: "{backup}"'
            )
        print(f'Preserving existing {VSCODE_DIRECTORY.as_posix()}: "{target}" -> "{backup}"')
        if dry_run:
            print(f'[dry-run] move "{target}" -> "{backup}"')
            print(
                f'[dry-run] link {VSCODE_DIRECTORY.as_posix()}: "{target}" -> "{source}"'
            )
            create_directory_link(source, target, link_type=link_type, dry_run=True)
            return
        target.rename(backup)
    prepare_directory_link(
        source,
        target,
        label=VSCODE_DIRECTORY.as_posix(),
        link_type=link_type,
        dry_run=dry_run,
    )


def validate_preparation_targets(target_root: Path) -> None:
    for label, target, backup in (
        (
            "Agent",
            target_root / AGENT_DIRECTORY,
            (target_root / AGENT_DIRECTORY).with_name(
                f"{AGENT_DIRECTORY.name}.pre-link-backup"
            ),
        ),
        (
            "VS Code",
            target_root / VSCODE_DIRECTORY,
            (target_root / VSCODE_DIRECTORY).with_name(
                f"{VSCODE_DIRECTORY.name}.pre-link-backup"
            ),
        ),
    ):
        if (
            target.is_dir()
            and not is_link_like(target)
            and not is_empty_directory(target)
            and (backup.exists() or is_link_like(backup))
        ):
            raise WorktreeToolError(
                f'Cannot preserve the existing {label} directory because the backup path exists: "{backup}"'
            )
    for label, target in (
        ("External", target_root / EXTERNAL_DIRECTORY),
        (PYTHON_ENVIRONMENT.as_posix(), target_root / PYTHON_ENVIRONMENT),
    ):
        if target.exists() and not is_link_like(target) and not is_empty_directory(target):
            raise WorktreeToolError(
                f'Target {label} already exists and is not an empty directory or link: "{target}"\n'
                "Move it aside manually if you really want to replace it."
            )


def run_preflight(target: Path) -> None:
    try:
        validate_prerequisites(target)
    except RuntimeError as exc:
        raise WorktreeToolError(str(exc)) from exc


def prepare_registered_worktree(
    target: Path,
    *,
    source_value: str | None,
    link_type: str,
    dry_run: bool,
) -> None:
    target = target.expanduser().absolute()
    worktrees = get_worktrees()
    worktree = require_registered_linked_worktree(target, worktrees, require_unlocked=False)
    source = preparation_source(source_value, target=worktree.path, worktrees=worktrees)
    selected_link_type = choose_link_type(link_type)
    required_sources = (
        (AGENT_DIRECTORY.as_posix(), source / AGENT_DIRECTORY),
        (VSCODE_DIRECTORY.as_posix(), source / VSCODE_DIRECTORY),
        ("External", source / EXTERNAL_DIRECTORY),
        (PYTHON_ENVIRONMENT.as_posix(), source / PYTHON_ENVIRONMENT),
    )
    missing_sources = [f'{label}: "{path}"' for label, path in required_sources if not path.is_dir()]
    if missing_sources:
        formatted = "\n".join(f"  {entry}" for entry in missing_sources)
        raise WorktreeToolError(f"Prepared source directories are missing:\n{formatted}")
    validate_preparation_targets(worktree.path)
    print(f'Source worktree: "{source}"')
    print(f'Target worktree: "{worktree.path}"')
    print(f"Link type: {selected_link_type}")
    prepare_agent_link(
        source,
        worktree.path,
        link_type=selected_link_type,
        dry_run=dry_run,
    )
    prepare_vscode_link(
        source,
        worktree.path,
        link_type=selected_link_type,
        dry_run=dry_run,
    )
    prepare_directory_link(
        source / EXTERNAL_DIRECTORY,
        worktree.path / EXTERNAL_DIRECTORY,
        label="External",
        link_type=selected_link_type,
        dry_run=dry_run,
    )
    prepare_directory_link(
        source / PYTHON_ENVIRONMENT,
        worktree.path / PYTHON_ENVIRONMENT,
        label=PYTHON_ENVIRONMENT.as_posix(),
        link_type=selected_link_type,
        dry_run=dry_run,
    )
    if dry_run:
        print("Dry run complete; preflight was not run.")
        return
    sys.stdout.flush()
    run_preflight(worktree.path)
    print(f'Prepared worktree: "{worktree.path}"')


def prepare_worktree_command(args: argparse.Namespace) -> None:
    target = Path(args.path).expanduser().absolute() if args.path else REPO_ROOT
    prepare_registered_worktree(
        target,
        source_value=args.source,
        link_type=args.link_type,
        dry_run=args.dry_run,
    )


def require_link_like_status(path: Path) -> bool:
    try:
        status = path.lstat()
    except OSError as exc:
        raise WorktreeToolError(f'Could not inspect possible directory link "{path}": {exc}') from exc
    if stat.S_ISLNK(status.st_mode):
        return True
    return bool(
        is_windows()
        and getattr(status, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    )


def directory_links_under(root: Path) -> list[Path]:
    links: list[Path] = []

    def raise_walk_error(exc: OSError) -> None:
        raise WorktreeToolError(f'Could not inspect worktree directory links: {exc}') from exc

    for current_root, directory_names, _ in os.walk(
        root,
        topdown=True,
        onerror=raise_walk_error,
        followlinks=False,
    ):
        current = Path(current_root)
        retained: list[str] = []
        for directory_name in directory_names:
            path = current / directory_name
            if require_link_like_status(path):
                links.append(path)
            else:
                retained.append(directory_name)
        directory_names[:] = retained
    return links


def require_registered_linked_worktree(
    target: Path,
    worktrees: Sequence[Worktree],
    *,
    require_unlocked: bool = True,
) -> Worktree:
    matches = [worktree for worktree in worktrees if same_path(worktree.path, target)]
    if not matches:
        raise WorktreeToolError(f'Path is not a registered Git worktree: "{target}"')
    worktree = matches[0]
    if same_path(worktree.path, worktrees[0].path):
        raise WorktreeToolError("Refusing to operate on the main worktree.")
    if require_unlocked and worktree.locked:
        raise WorktreeToolError("Refusing to remove a locked worktree. Unlock it explicitly first.")
    return worktree


def require_clean_worktree(worktree: Worktree, *, force: bool) -> None:
    if force:
        return
    result = git_command(
        ["status", "--porcelain", "--untracked-files=all"],
        cwd=worktree.path,
    )
    require_git_success(result, "Checking worktree status")
    if result.stdout.strip():
        raise WorktreeToolError(
            "Worktree contains modified or untracked files. Commit or remove them, "
            "or pass --force to discard them."
        )


def validate_directory_links(worktree: Worktree) -> list[Path]:
    expected = [worktree.path / relative for relative in SHARED_DIRECTORY_PATHS]
    expected_keys = {normalized_path(path) for path in expected}
    discovered = directory_links_under(worktree.path)
    unexpected = [path for path in discovered if normalized_path(path) not in expected_keys]
    if unexpected:
        formatted = "\n".join(f'  "{path}"' for path in unexpected)
        raise WorktreeToolError(
            "Refusing to remove a worktree containing unexpected directory links:\n"
            f"{formatted}\nRemove or review these links explicitly first."
        )
    return [path for path in expected if is_link_like(path)]


def detach_link(path: Path) -> DetachedLink:
    target = path.resolve(strict=False)
    kind = "symlink" if path.is_symlink() else "junction"
    path.rmdir()
    return DetachedLink(path, target, kind)


def restore_link(link: DetachedLink) -> None:
    if link.kind == "junction":
        result = subprocess.run(
            ["cmd.exe", "/d", "/c", "mklink", "/J", str(link.path), str(link.target)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            detail = (result.stderr or result.stdout or "").strip()
            raise WorktreeToolError(
                f'Could not restore junction "{link.path}" -> "{link.target}": {detail}'
            )
        return
    os.symlink(link.target, link.path, target_is_directory=True)


def remove_worktree(args: argparse.Namespace) -> None:
    target = Path(args.path).expanduser().absolute()
    worktrees = get_worktrees()
    worktree = require_registered_linked_worktree(target, worktrees)
    require_clean_worktree(worktree, force=args.force)
    links = validate_directory_links(worktree)

    print(f'Worktree to remove: "{worktree.path}"')
    for link in links:
        print(f'  detach "{link}" -> "{link.resolve(strict=False)}"')
    if args.dry_run:
        print("Dry run complete; no links or worktrees were removed.")
        return

    detached: list[DetachedLink] = []
    try:
        for link in links:
            detached.append(detach_link(link))
        command = ["worktree", "remove"]
        if args.force:
            command.append("--force")
        command.append(str(worktree.path))
        result = git_command(command)
        require_git_success(result, "Removing Git worktree")
    except (OSError, WorktreeToolError) as exc:
        restore_errors: list[str] = []
        for link in reversed(detached):
            if link.path.exists() or is_link_like(link.path):
                continue
            try:
                restore_link(link)
            except (OSError, WorktreeToolError) as restore_exc:
                restore_errors.append(str(restore_exc))
        if restore_errors:
            joined = "\n".join(restore_errors)
            raise WorktreeToolError(f"{exc}\nAdditionally, link restoration failed:\n{joined}") from exc
        raise
    print(f'Removed worktree safely: "{worktree.path}"')
