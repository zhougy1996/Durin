#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import stat
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
SHARED_DIRECTORY_PATHS = (Path(".agents"), Path(".venv"), Path("Engine/External"))


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
    cwd: Path = REPO_ROOT,
    capture_output: bool = True,
) -> subprocess.CompletedProcess[str]:
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
    config_path = worktree / ".agents" / "build-config.json"
    if not config_path.is_file():
        print(
            f"WARNING: Agent config is missing for worktree '{worktree}'. "
            "Opening it without a configured environment; run Setup.bat there.",
            file=sys.stderr,
        )
        return []
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
        environment_setup = config.get("environmentSetup") or {}
        setup_script = environment_setup.get("script")
        arguments = environment_setup.get("arguments") or []
    except (OSError, json.JSONDecodeError, AttributeError) as exc:
        raise WorktreeToolError(f'Could not read Agent config "{config_path}": {exc}') from exc

    if not setup_script:
        return []
    setup_path = Path(setup_script)
    if not setup_path.is_file():
        raise WorktreeToolError(f'Environment setup script does not exist: "{setup_path}"')
    if not isinstance(arguments, list) or not all(isinstance(value, str) for value in arguments):
        raise WorktreeToolError(
            f'Agent config environmentSetup.arguments must be an array of strings: "{config_path}"'
        )
    return [str(setup_path), *arguments]


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
            arguments.extend(["move-focus", "left", ";"])
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

    setup_script = target / "Setup.bat"
    if not setup_script.is_file():
        raise WorktreeToolError(
            f'Worktree was created, but Setup.bat was not found: "{setup_script}"'
        )
    setup_result = subprocess.run(
        ["cmd.exe", "/d", "/c", "call", str(setup_script), "--no-pause"],
        check=False,
    )
    if setup_result.returncode != 0:
        raise WorktreeToolError(
            f'Worktree was created at "{target}", but Setup failed with '
            f"exit code {setup_result.returncode}. Fix the reported problem and rerun Setup.bat there."
        )
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


def require_registered_linked_worktree(target: Path, worktrees: Sequence[Worktree]) -> Worktree:
    matches = [worktree for worktree in worktrees if same_path(worktree.path, target)]
    if not matches:
        raise WorktreeToolError(f'Path is not a registered Git worktree: "{target}"')
    worktree = matches[0]
    if same_path(worktree.path, worktrees[0].path):
        raise WorktreeToolError("Refusing to remove the main worktree.")
    if worktree.locked:
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


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="WorktreeTool",
        description="Create, inspect, open, and safely remove Durin Git worktrees.",
    )
    subparsers = parser.add_subparsers(dest="action", required=True)

    open_parser = subparsers.add_parser("open", help="open terminals for every worktree")
    open_parser.add_argument("--dry-run", action="store_true", help="print without opening Windows Terminal")

    subparsers.add_parser("list", help="list registered worktrees")

    add_parser = subparsers.add_parser("add", help="create and prepare a worktree")
    add_parser.add_argument("path", help="new worktree path")
    add_parser.add_argument("commit_ish", nargs="?", help="commit or branch to check out")
    add_mode = add_parser.add_mutually_exclusive_group()
    add_mode.add_argument("-b", "--branch", help="create and check out a new branch")
    add_mode.add_argument("--detach", action="store_true", help="detach HEAD in the new worktree")

    remove_parser = subparsers.add_parser("remove", help="safely remove a linked worktree")
    remove_parser.add_argument("path", help="linked worktree path")
    remove_parser.add_argument(
        "--force",
        action="store_true",
        help="discard modified and untracked files",
    )
    remove_parser.add_argument("--dry-run", action="store_true", help="validate and print without removing")
    return parser


def parse_args(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    values = list(sys.argv[1:] if arguments is None else arguments)
    if not values or values[0] == "--dry-run":
        values.insert(0, "open")
    return create_parser().parse_args(values)


def main(arguments: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(arguments)
        if args.action == "open":
            open_worktree_terminals(dry_run=args.dry_run)
        elif args.action == "list":
            display_worktrees(get_worktrees())
        elif args.action == "add":
            add_worktree(args)
        elif args.action == "remove":
            remove_worktree(args)
        else:
            raise WorktreeToolError(f"Unsupported action: {args.action}")
    except WorktreeToolError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted.", file=sys.stderr)
        return 130
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
