from __future__ import annotations

import argparse
from pathlib import Path

from .common_specs import CONTEXT_ARGUMENTS, DISPLAY_ARGUMENTS, PLAIN, PRESET, PROFILE
from .specification import ArgumentSpec, CommandSpec, argument


CMAKE = argument("--cmake", help="CMake executable override")
ENVIRONMENT_SETUP = argument("--environment-setup", help="toolchain environment script override")
JOBS = argument("--jobs", type=int, choices=range(1, 257), metavar="1..256", help="parallel build job limit")
AGENT = argument("--agent", action="store_true", help="use stable compact output with liveness heartbeats for Agent execution")
OUTPUT_MODE = argument(
    "--output", dest="output_mode", choices=("auto", "compact", "progress", "full"),
    default=None, help="child output mode (default: auto)",
)
CHILD_OUTPUT_ARGUMENTS = (PLAIN, OUTPUT_MODE)
TOOL_ARGUMENTS = (PROFILE, PRESET, CMAKE, ENVIRONMENT_SETUP, JOBS, AGENT, PLAIN, OUTPUT_MODE)
STATUS_ARGUMENTS = (PROFILE, PRESET, CMAKE, ENVIRONMENT_SETUP, JOBS, PLAIN)
HANDLER = "durin_dev_tool.build.handler:run"


def build_command(
    name: str,
    summary: str,
    arguments: tuple[ArgumentSpec, ...],
    *,
    action: str | None = None,
    epilog: str = "",
) -> CommandSpec:
    return CommandSpec(
        name, summary, HANDLER,
        arguments=arguments,
        required_modules=("rich",),
        defaults=(("build_action", action or name),),
        epilog=epilog,
    )


CREATE_MODULE = build_command(
    "module", "create and register a module",
    (
        argument("create_name", metavar="NAME"),
        argument("--project", dest="project_path", type=Path, required=True),
        argument("--path", dest="destination_path", type=Path, default=None),
        argument("--kind", dest="module_kind", choices=("runtime", "editor", "developer"), default="runtime"),
        argument("--link", dest="link_type", choices=("shared", "static"), default="shared"),
        argument("--pch", default=""),
        argument("--public-dependency", dest="public_dependencies", action="append", default=None),
        argument("--private-dependency", dest="private_dependencies", action="append", default=None),
        argument("--optional-public-dependency", dest="optional_public_dependencies", action="append", default=None),
        argument("--optional-private-dependency", dest="optional_private_dependencies", action="append", default=None),
        argument("--enable", dest="enablements", action="append", default=None),
        argument("--dry-run", action="store_true"),
        PLAIN,
    ),
    action="create-module",
)
CREATE_PROJECT = build_command(
    "project", "create and register a workspace project",
    (
        argument("create_name", metavar="NAME"),
        argument("--path", dest="destination_path", type=Path, required=True),
        argument("--dry-run", action="store_true"),
        PLAIN,
    ),
    action="create-project",
)

COMMAND_SPECS = (
    build_command("stop", "stop the active build operation", (PLAIN,)),
    build_command("presets", "list registered presets", CONTEXT_ARGUMENTS + DISPLAY_ARGUMENTS),
    build_command("preset", "inspect a selected preset", (argument("selected_preset", nargs="?", default=""), PROFILE, PLAIN)),
    build_command("status", "show build context and toolchain state", STATUS_ARGUMENTS),
    build_command(
        "path", "print a registered repository location",
        CONTEXT_ARGUMENTS + DISPLAY_ARGUMENTS + (argument("location", nargs="?"), argument("--all", dest="all_locations", action="store_true")),
    ),
    build_command("open", "open a registered repository location", CONTEXT_ARGUMENTS + DISPLAY_ARGUMENTS + (argument("location"),)),
    build_command(
        "configure", "configure the selected preset",
        TOOL_ARGUMENTS + (
            argument("--fresh", action="store_true"),
            argument(
                "-D", "--define",
                dest="defines",
                action="append",
                default=[],
                metavar="NAME=VALUE",
                help="set a CMake cache value; may be repeated",
            ),
        ),
    ),
    build_command("build", "build a CMake target", TOOL_ARGUMENTS + (argument("--target", default="all"),)),
    build_command("clean", "clean the selected preset", TOOL_ARGUMENTS),
    build_command("recover", "resume an interrupted build incrementally", TOOL_ARGUMENTS),
    build_command(
        "purge", "delete generated build artifacts",
        CONTEXT_ARGUMENTS + DISPLAY_ARGUMENTS + (argument("--all-presets", action="store_true"), argument("--yes", action="store_true")),
    ),
    build_command("rebuild", "clean, configure, and build", TOOL_ARGUMENTS + (argument("--target", default="all"),)),
    build_command(
        "test", "list, explain, build, and run native-test selections",
        TOOL_ARGUMENTS + (
            argument("selection", nargs="?", default="", help="target, affected, fast-all, @set selector, all, list [query], or explain <selection>"),
            argument("case_filter", nargs="?", default=""),
            argument("--filter", default="", help="GoogleTest filter for a single native test target"),
            argument("--mode", choices=("routine", "isolation", "stress", "report", "characterization", "qualification"), default="routine", help="execution scenario (default: routine)"),
            argument("--report", type=Path, default=None, help="JUnit path for report mode (default: preset result directory)"),
            argument("--timeout", type=int, choices=range(0, 86401), default=300, metavar="0..86400", help="test timeout in seconds; 0 disables it (default: 300)"),
            argument("--base", default="", metavar="REF", help="Git base for test affected (default: current staged, unstaged, and untracked changes)"),
            argument("--explain", dest="explain_affected", action="store_true", help="explain test affected without building or running"),
        ),
        epilog=(
            "Common examples:\n"
            "  DevTool test CoreUtilityTests\n"
            "  DevTool test CoreUtilityTests Suite.Case\n"
            "  DevTool test affected\n"
            "  DevTool test affected --base HEAD~1 --explain\n"
            "  DevTool test fast-all\n"
            "  DevTool test \"@viewport\"\n"
            "  DevTool test all"
        ),
    ),
    build_command(
        "run", "run the selected preset's existing application",
        CONTEXT_ARGUMENTS + CHILD_OUTPUT_ARGUMENTS + (
            argument("--project", dest="project_path", type=Path, default=None),
            argument("--args", dest="run_arguments", nargs=argparse.REMAINDER, default=()),
        ),
    ),
)

SCAFFOLDING_COMMAND_SPEC = CommandSpec(
    "create", "create a module or workspace project",
    subcommands=(CREATE_MODULE, CREATE_PROJECT),
)
