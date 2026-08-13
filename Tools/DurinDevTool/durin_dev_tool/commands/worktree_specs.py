from __future__ import annotations

from .specification import CommandSpec, argument


HANDLER = "durin_dev_tool.worktree.handler:run"

COMMAND_SPEC = CommandSpec(
    "worktree",
    "create, prepare, list, open, and remove worktrees",
    subcommands=(
        CommandSpec(
            "open",
            "open terminals for every worktree",
            HANDLER,
            arguments=(argument("--dry-run", action="store_true"),),
            defaults=(("worktree_action", "open"),),
        ),
        CommandSpec(
            "list",
            "list registered worktrees",
            HANDLER,
            defaults=(("worktree_action", "list"),),
        ),
        CommandSpec(
            "add",
            "create and prepare a worktree",
            HANDLER,
            arguments=(
                argument("path", help="new worktree path"),
                argument("commit_ish", nargs="?", help="commit or branch to check out"),
                argument("-b", "--branch", help="create and check out a new branch"),
                argument("--detach", action="store_true"),
                argument("--source", help="prepared source worktree or Engine/External path"),
                argument("--link-type", choices=("auto", "junction", "symlink"), default="auto"),
            ),
            defaults=(("worktree_action", "add"),),
        ),
        CommandSpec(
            "prepare",
            "prepare or repair a linked worktree",
            HANDLER,
            arguments=(
                argument("path", nargs="?", help="linked worktree path"),
                argument("--source", help="prepared source worktree or Engine/External path"),
                argument("--link-type", choices=("auto", "junction", "symlink"), default="auto"),
                argument("--dry-run", action="store_true"),
            ),
            defaults=(("worktree_action", "prepare"),),
        ),
        CommandSpec(
            "remove",
            "safely remove a linked worktree",
            HANDLER,
            arguments=(
                argument("path", help="linked worktree path"),
                argument("--force", action="store_true"),
                argument("--dry-run", action="store_true"),
            ),
            defaults=(("worktree_action", "remove"),),
        ),
    ),
    default_subcommand="list",
)
