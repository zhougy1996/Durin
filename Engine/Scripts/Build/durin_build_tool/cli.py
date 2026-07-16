from __future__ import annotations

import argparse
import os
import shlex
import sys
from dataclasses import replace
from pathlib import Path
from time import perf_counter
from typing import Sequence

from rich.table import Table

from .config import (
    Action,
    BuildContext,
    BuildToolError,
    CommandRequest,
    preset_build_directory,
    preset_cache_string,
)
from .core import (
    create_context,
    derive_context,
    execute_context,
    interruption_marker_path,
)
from .output import BuildOutput


class BuildArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise BuildToolError(f"{message}\nRun '{self.prog} --help' for command usage.")


def add_common_options(parser: argparse.ArgumentParser, *, inherited: bool = False) -> None:
    default = argparse.SUPPRESS if inherited else ""
    parser.add_argument("--profile", default=default, help="host build profile")
    parser.add_argument("--preset", default=default, help="registered CMake configure preset")
    parser.add_argument("--cmake", default=default, help="CMake executable override")
    parser.add_argument("--environment-setup", default=default, help="toolchain environment script override")
    parser.add_argument(
        "--plain",
        action="store_true",
        default=argparse.SUPPRESS if inherited else False,
        help="disable colors and styled terminal output",
    )


def add_jobs(parser: argparse.ArgumentParser, *, inherited: bool = False) -> None:
    parser.add_argument(
        "--jobs",
        type=int,
        choices=range(1, 257),
        default=argparse.SUPPRESS if inherited else None,
        metavar="1..256",
        help="parallel build job limit",
    )


def make_parser() -> BuildArgumentParser:
    parser = BuildArgumentParser(
        prog="BuildTool",
        description="Configure, build, test, clean, and run Durin.",
    )
    parser.set_defaults(action=Action.SHELL.value)
    add_common_options(parser)
    add_jobs(parser)
    subparsers = parser.add_subparsers(dest="action", metavar="COMMAND")

    shell = subparsers.add_parser("shell", help="open the interactive shell")
    add_common_options(shell, inherited=True)
    add_jobs(shell, inherited=True)

    configure = subparsers.add_parser("configure", help="fresh-configure the selected preset")
    add_common_options(configure, inherited=True)
    add_jobs(configure, inherited=True)

    build = subparsers.add_parser("build", help="build a CMake target")
    add_common_options(build, inherited=True)
    add_jobs(build, inherited=True)
    build.add_argument("--target", required=True, help="CMake target to build")

    clean = subparsers.add_parser("clean", help="clean the selected preset")
    add_common_options(clean, inherited=True)
    add_jobs(clean, inherited=True)

    rebuild = subparsers.add_parser("rebuild", help="clean, configure, and build")
    add_common_options(rebuild, inherited=True)
    add_jobs(rebuild, inherited=True)
    rebuild.add_argument("--target", default="all", help="CMake target to build (default: all)")

    test = subparsers.add_parser("test", help="build and run a native test target")
    add_common_options(test, inherited=True)
    add_jobs(test, inherited=True)
    test.add_argument("--target", required=True, help="native test target")
    test.add_argument("--filter", default="", help="GoogleTest filter")

    purge = subparsers.add_parser("purge", help="delete generated build artifacts")
    add_common_options(purge, inherited=True)
    add_jobs(purge, inherited=True)
    purge.add_argument("--all-presets", action="store_true", help="purge every registered preset")
    purge.add_argument("--yes", action="store_true", help="skip purge confirmation")

    run = subparsers.add_parser("run", help="run the selected preset's existing application")
    add_common_options(run, inherited=True)
    add_jobs(run, inherited=True)
    run.add_argument(
        "--args",
        dest="run_arguments",
        nargs=argparse.REMAINDER,
        default=(),
        help="arguments passed to the application; must be the final BuildTool option",
    )
    return parser


def normalize_action(argv: Sequence[str]) -> list[str]:
    values = list(argv)
    commands = {action.value for action in Action}
    for index, value in enumerate(values):
        if value.lower() in commands:
            values[index] = value.lower()
            break
    return values


def namespace_request(args: argparse.Namespace) -> CommandRequest:
    return CommandRequest(
        action=Action(args.action or Action.SHELL.value),
        target=getattr(args, "target", ""),
        jobs=getattr(args, "jobs", None),
        test_filter=getattr(args, "filter", ""),
        run_arguments=tuple(getattr(args, "run_arguments", ())),
        profile=getattr(args, "profile", ""),
        preset=getattr(args, "preset", ""),
        cmake=getattr(args, "cmake", ""),
        environment_setup=getattr(args, "environment_setup", ""),
        all_presets=getattr(args, "all_presets", False),
        yes=getattr(args, "yes", False),
        plain=getattr(args, "plain", False),
    )


def parse_args(argv: Sequence[str] | None = None) -> CommandRequest:
    values = normalize_action(sys.argv[1:] if argv is None else argv)
    return namespace_request(make_parser().parse_args(values))


def command_request(
    base: CommandRequest,
    action: Action,
    *,
    preset: str,
    target: str = "",
    test_filter: str = "",
    run_arguments: Sequence[str] = (),
    all_presets: bool = False,
    yes: bool = False,
) -> CommandRequest:
    return replace(
        base,
        action=action,
        preset=preset,
        target=target,
        test_filter=test_filter,
        run_arguments=tuple(run_arguments),
        all_presets=all_presets,
        yes=yes,
    )


def confirm_purge(output: BuildOutput, paths: Sequence[Path], all_presets: bool) -> bool:
    scope = "all registered presets" if all_presets else "the current preset"
    output.warning(f"Purge will permanently remove build artifacts for {scope}:")
    for path in paths:
        try:
            display = path.relative_to(Path.cwd())
        except ValueError:
            display = path
        output.info(f"  {display}")
    expected = "PURGE ALL" if all_presets else "PURGE"
    try:
        return input(f'Type "{expected}" to continue: ').strip() == expected
    except (EOFError, KeyboardInterrupt):
        output.info("")
        return False


def print_shell_help(output: BuildOutput) -> None:
    output.info(
        "Commands:\n"
        "  /presets                  List presets and select one by number\n"
        "  /preset [full-name]       Show or select the current preset\n"
        "  /status                   Show resolved build context and recovery state\n"
        "  /configure                Configure the current preset\n"
        "  /build [target]           Build a target (default: all)\n"
        "  /clean                    Clean the current preset\n"
        "  /purge [options]          Delete artifacts (--all-presets, --yes)\n"
        "  /rebuild [target]         Clean, configure, and build (default: all)\n"
        "  /test <target> [filter]   Build and run a native test target\n"
        "  /run [arguments...]       Run the existing runtime executable\n"
        "  /help                     Show this help\n"
        "  /exit                     Leave the shell"
    )


def show_presets(output: BuildOutput, context: BuildContext, current_preset: str) -> None:
    if output.plain:
        for index, preset in enumerate(context.profile.presets, start=1):
            markers = []
            if preset == context.profile.default_preset:
                markers.append("default")
            if preset == current_preset:
                markers.append("current")
            suffix = f' [{", ".join(markers)}]' if markers else ""
            output.info(f"  {index:>2}  {preset}{suffix}")
        return
    table = Table(title="Registered presets")
    table.add_column("#", justify="right")
    table.add_column("Preset")
    table.add_column("State")
    for index, preset in enumerate(context.profile.presets, start=1):
        markers = []
        if preset == context.profile.default_preset:
            markers.append("default")
        if preset == current_preset:
            markers.append("current")
        table.add_row(str(index), preset, ", ".join(markers))
    output.console.print(table)


def resolve_shell_preset(value: str, context: BuildContext) -> str:
    matches = [preset for preset in context.profile.presets if preset.lower() == value.lower()]
    if len(matches) == 1:
        return matches[0]
    raise BuildToolError(f'Unknown preset "{value}". Use its full name or run /presets.')


def resolve_shell_preset_number(value: str, context: BuildContext) -> str:
    if value.isdigit() and 1 <= int(value) <= len(context.profile.presets):
        return context.profile.presets[int(value) - 1]
    raise BuildToolError(f'Invalid preset number "{value}". Enter a number shown by /presets.')


def show_status(output: BuildOutput, context: BuildContext, preset_name: str) -> None:
    status_context = derive_context(context, replace(context.request, preset=preset_name))
    marker = interruption_marker_path(preset_name)
    values = {
        "Profile": status_context.profile.name,
        "Preset": preset_name,
        "Build directory": preset_build_directory(status_context.preset),
        "Configuration": preset_cache_string(status_context.preset, "CMAKE_BUILD_TYPE"),
        "Parallel jobs": status_context.jobs,
        "CMake": status_context.cmake,
        "Recovery state": "rebuild required" if marker.is_file() else "clean",
    }
    if output.plain:
        for label, value in values.items():
            output.info(f"{label}: {value}")
        return
    table = Table(title="BuildTool status")
    table.add_column("Setting", style="bold cyan")
    table.add_column("Value")
    for label, value in values.items():
        table.add_row(label, str(value))
    output.console.print(table)


def run_shell(request: CommandRequest, output: BuildOutput) -> None:
    # Shell initialization intentionally resolves the expensive toolchain environment only once.
    base = create_context(request)
    current_preset = base.preset.name
    output.info("Durin BuildTool shell")
    show_status(output, base, current_preset)
    output.info("Type /help for available commands.")
    selecting_preset = False
    while True:
        try:
            line = input("BuildTool> ").strip()
        except EOFError:
            output.info("")
            return
        except KeyboardInterrupt:
            output.warning("Use /exit to leave the shell.")
            continue
        if selecting_preset:
            selecting_preset = False
            if not line:
                continue
            if line.isdigit():
                try:
                    current_preset = resolve_shell_preset_number(line, base)
                    output.success(f'CMake preset selected: "{current_preset}"')
                except BuildToolError as exc:
                    output.failure(exc, None, 0.0)
                continue
        if not line:
            continue
        try:
            parts = shlex.split(line)
            command = parts[0].lower().lstrip("/")
            values = parts[1:]
            if command in {"exit", "quit"}:
                return
            if command in {"help", "?"}:
                print_shell_help(output)
                continue
            if command == "presets":
                show_presets(output, base, current_preset)
                output.info("Enter a preset number, or press Enter to keep the current preset.")
                selecting_preset = True
                continue
            if command == "status":
                show_status(output, base, current_preset)
                continue
            if command == "preset":
                if not values:
                    output.info(f'CMake preset: "{current_preset}"')
                    continue
                if len(values) != 1:
                    raise BuildToolError("/preset accepts one full preset name.")
                current_preset = resolve_shell_preset(values[0], base)
                output.success(f'CMake preset selected: "{current_preset}"')
                continue
            if command in {"configure", "clean"}:
                if values:
                    raise BuildToolError(f"/{command} does not accept positional arguments.")
                action = Action(command)
                child_request = command_request(request, action, preset=current_preset)
            elif command == "purge":
                allowed = {"--all-presets", "--yes"}
                if any(value not in allowed for value in values) or len(set(values)) != len(values):
                    raise BuildToolError("/purge accepts only --all-presets and --yes.")
                child_request = command_request(
                    request,
                    Action.PURGE,
                    preset=current_preset,
                    all_presets="--all-presets" in values,
                    yes="--yes" in values,
                )
            elif command in {"build", "rebuild"}:
                if len(values) > 1:
                    raise BuildToolError(f"/{command} accepts at most one target.")
                child_request = command_request(
                    request,
                    Action(command),
                    preset=current_preset,
                    target=values[0] if values else "all",
                )
            elif command == "test":
                if not 1 <= len(values) <= 2:
                    raise BuildToolError("/test requires a target and accepts an optional GoogleTest filter.")
                child_request = command_request(
                    request,
                    Action.TEST,
                    preset=current_preset,
                    target=values[0],
                    test_filter=values[1] if len(values) == 2 else "",
                )
            elif command == "run":
                child_request = command_request(
                    request,
                    Action.RUN,
                    preset=current_preset,
                    run_arguments=values,
                )
            else:
                raise BuildToolError(f'Unknown shell command "{parts[0]}". Type /help for available commands.')
            child_context = derive_context(base, child_request)
            execute_context(
                child_context,
                output,
                confirm_purge=lambda paths, all_presets: confirm_purge(output, paths, all_presets),
            )
        except (BuildToolError, ValueError) as exc:
            error = exc if isinstance(exc, BuildToolError) else BuildToolError(f"Invalid command: {exc}")
            output.failure(error, None, 0.0)


def main(argv: Sequence[str] | None = None) -> int:
    started = perf_counter()
    output: BuildOutput | None = None
    context: BuildContext | None = None
    try:
        request = parse_args(argv)
        output = BuildOutput(plain=request.plain)
        if request.action is Action.SHELL:
            run_shell(request, output)
            return 0
        prepare_tools = request.action not in {Action.PURGE, Action.RUN}
        context = create_context(request, prepare_tools=prepare_tools)
        execute_context(
            context,
            output,
            confirm_purge=lambda paths, all_presets: confirm_purge(output, paths, all_presets),
        )
        return 0
    except BuildToolError as exc:
        output = output or BuildOutput(plain="--plain" in (argv or sys.argv[1:]))
        output.failure(exc, context, perf_counter() - started)
        return 1
    except OSError as exc:
        output = output or BuildOutput(plain="--plain" in (argv or sys.argv[1:]))
        output.failure(BuildToolError(f"Operating system error: {exc}"), context, perf_counter() - started)
        return 1
