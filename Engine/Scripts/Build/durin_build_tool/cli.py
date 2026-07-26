from __future__ import annotations

import argparse
import os
import shlex
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from time import perf_counter
from typing import Any, Sequence

from rich.markup import escape
from rich.table import Table

from .config import (
    Action,
    BuildContext,
    BuildToolError,
    CMAKE_ENV_VARS,
    CommandRequest,
    JOBS_ENV_VAR,
    OutputMode,
    preset_build_directory,
    preset_cache_string,
)
from .core import (
    create_context,
    derive_context,
    execute_context,
    interruption_marker_path,
    open_runtime_directory,
    prepare_command_context,
    prepare_toolchain_environment,
    stop_active_operation,
)
from .output import BuildOutput


@dataclass(frozen=True)
class ArgumentSpec:
    flags: tuple[str, ...]
    dest: str
    kwargs: dict[str, Any]


@dataclass(frozen=True)
class CommandSpec:
    action: Action
    summary: str
    shell_usage: str
    arguments: tuple[ArgumentSpec, ...] = ()
    compact_operands: tuple[str, ...] = ()

    @property
    def name(self) -> str:
        return self.action.value

    @property
    def aliases(self) -> tuple[str, ...]:
        return (f"/{self.name}",)


PROFILE = ArgumentSpec(("--profile",), "profile", {"help": "host build profile"})
PRESET = ArgumentSpec(("--preset",), "preset", {"help": "registered CMake configure preset"})
CMAKE = ArgumentSpec(("--cmake",), "cmake", {"help": "CMake executable override"})
ENVIRONMENT_SETUP = ArgumentSpec(
    ("--environment-setup",),
    "environment_setup",
    {"help": "toolchain environment script override"},
)
JOBS = ArgumentSpec(
    ("--jobs",),
    "jobs",
    {
        "type": int,
        "choices": range(1, 257),
        "metavar": "1..256",
        "help": "parallel build job limit",
    },
)
PLAIN = ArgumentSpec(
    ("--plain",),
    "plain",
    {"action": "store_true", "help": "disable colors and styled terminal output"},
)
OUTPUT_MODE = ArgumentSpec(
    ("--output",),
    "output_mode",
    {
        "choices": tuple(mode.value for mode in OutputMode),
        "default": OutputMode.AUTO.value,
        "help": "child output mode: auto, compact, or full (default: auto)",
    },
)
FRESH = ArgumentSpec(
    ("--fresh",),
    "fresh",
    {"action": "store_true", "help": "discard the existing CMake cache first"},
)
TARGET_ALL = ArgumentSpec(
    ("--target",),
    "target",
    {"default": "all", "help": "CMake target to build (default: all)"},
)
TARGET_TEST = ArgumentSpec(
    ("--target",),
    "target",
    {"required": True, "help": "native test target"},
)
TEST_FILTER = ArgumentSpec(("--filter",), "filter", {"default": "", "help": "GoogleTest filter"})
TEST_TIMEOUT = ArgumentSpec(
    ("--timeout",),
    "timeout",
    {
        "type": int,
        "choices": range(0, 86401),
        "default": 300,
        "metavar": "0..86400",
        "help": "test executable timeout in seconds; 0 disables it (default: 300)",
    },
)
ALL_PRESETS = ArgumentSpec(
    ("--all-presets",),
    "all_presets",
    {"action": "store_true", "help": "purge every registered preset"},
)
YES = ArgumentSpec(("--yes",), "yes", {"action": "store_true", "help": "skip purge confirmation"})
RUN_ARGUMENTS = ArgumentSpec(
    ("--args",),
    "run_arguments",
    {
        "nargs": argparse.REMAINDER,
        "default": (),
        "help": "arguments passed to the application; must be the final BuildTool option",
    },
)

CONTEXT_ARGUMENTS = (PROFILE, PRESET, PLAIN, OUTPUT_MODE)
TOOL_ARGUMENTS = (PROFILE, PRESET, CMAKE, ENVIRONMENT_SETUP, JOBS, PLAIN, OUTPUT_MODE)
COMMAND_SPECS = (
    CommandSpec(
        Action.SHELL,
        "open the interactive shell",
        "shell",
        TOOL_ARGUMENTS,
    ),
    CommandSpec(Action.STOP, "stop the active BuildTool operation", "stop", (PLAIN,)),
    CommandSpec(
        Action.PRESETS,
        "list registered presets",
        "presets",
        CONTEXT_ARGUMENTS,
    ),
    CommandSpec(
        Action.STATUS,
        "show build context and toolchain state",
        "status",
        TOOL_ARGUMENTS,
    ),
    CommandSpec(
        Action.OPEN_RUNTIME,
        "open the selected preset's runtime directory",
        "open-runtime",
        CONTEXT_ARGUMENTS,
    ),
    CommandSpec(
        Action.CONFIGURE,
        "configure the selected preset",
        "configure [--fresh]",
        TOOL_ARGUMENTS + (FRESH,),
    ),
    CommandSpec(
        Action.BUILD,
        "build a CMake target",
        "build [--target TARGET]",
        TOOL_ARGUMENTS + (TARGET_ALL,),
        ("target",),
    ),
    CommandSpec(
        Action.CLEAN,
        "clean the selected preset",
        "clean",
        TOOL_ARGUMENTS,
    ),
    CommandSpec(
        Action.PURGE,
        "delete generated build artifacts",
        "purge [--all-presets] [--yes]",
        CONTEXT_ARGUMENTS + (ALL_PRESETS, YES),
    ),
    CommandSpec(
        Action.REBUILD,
        "clean, configure, and build",
        "rebuild [--target TARGET]",
        TOOL_ARGUMENTS + (TARGET_ALL,),
        ("target",),
    ),
    CommandSpec(
        Action.TEST,
        "build and run a native test target",
        "test --target TARGET [--filter FILTER] [--timeout SECONDS]",
        TOOL_ARGUMENTS + (TARGET_TEST, TEST_FILTER, TEST_TIMEOUT),
        ("target", "filter"),
    ),
    CommandSpec(
        Action.RUN,
        "run the selected preset's existing application",
        "run [--args ...]",
        CONTEXT_ARGUMENTS + (RUN_ARGUMENTS,),
        ("run_arguments",),
    ),
)

TOOLCHAIN_ACTIONS = {
    Action.CONFIGURE,
    Action.BUILD,
    Action.CLEAN,
    Action.REBUILD,
    Action.TEST,
}
COMMAND_BY_NAME = {spec.name: spec for spec in COMMAND_SPECS}
SHELL_COMMANDS = {
    name: spec
    for spec in COMMAND_SPECS
    for name in (spec.name, *spec.aliases)
}
SHELL_CONTROL_ALIASES = {
    "/exit": "exit",
    "/quit": "quit",
    "/help": "help",
    "/?": "?",
    "/preset": "preset",
}
ROOT_ARGUMENTS = TOOL_ARGUMENTS
COMMON_OPTIONS_WITH_VALUES = {
    flag
    for argument in ROOT_ARGUMENTS
    if argument.kwargs.get("action") != "store_true"
    for flag in argument.flags
}


class BuildArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise BuildToolError(f"{message}\nRun '{self.prog} --help' for command usage.")


def add_argument_spec(
    parser: argparse.ArgumentParser,
    argument: ArgumentSpec,
    *,
    suppress_default: bool = False,
) -> None:
    kwargs = dict(argument.kwargs)
    kwargs["dest"] = argument.dest
    if suppress_default:
        kwargs["default"] = argparse.SUPPRESS
    parser.add_argument(*argument.flags, **kwargs)


def shell_command_help() -> str:
    command_width = max(len(spec.shell_usage) for spec in COMMAND_SPECS if spec.action is not Action.SHELL)
    lines = ["Interactive commands:"]
    for spec in COMMAND_SPECS:
        if spec.action is Action.SHELL:
            continue
        lines.append(f"  {spec.shell_usage:<{command_width}}  {spec.summary}")
    lines.extend(
        [
            f"  {'preset [full-name]':<{command_width}}  show or select the current preset",
            f"  {'help':<{command_width}}  show interactive command help",
            f"  {'exit':<{command_width}}  leave the shell",
        ]
    )
    return "\n".join(lines)


def make_parser() -> BuildArgumentParser:
    parser = BuildArgumentParser(
        prog="BuildTool",
        description="Configure, build, test, clean, and run Durin.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.set_defaults(action=Action.SHELL.value)
    for argument in ROOT_ARGUMENTS:
        add_argument_spec(parser, argument, suppress_default=True)
    subparsers = parser.add_subparsers(dest="action", metavar="COMMAND")
    for spec in COMMAND_SPECS:
        command_parser = subparsers.add_parser(
            spec.name,
            help=spec.summary,
            description=spec.summary,
            formatter_class=argparse.RawDescriptionHelpFormatter,
            epilog=shell_command_help() if spec.action is Action.SHELL else None,
        )
        command_parser.set_defaults(action=spec.name)
        for argument in spec.arguments:
            add_argument_spec(
                command_parser,
                argument,
                suppress_default=argument in ROOT_ARGUMENTS,
            )
    return parser


def normalize_action(argv: Sequence[str]) -> list[str]:
    values = list(argv)
    commands = {action.value for action in Action}
    index = 0
    while index < len(values):
        value = values[index]
        option = value.partition("=")[0]
        if value.startswith("-"):
            index += 1 if "=" in value or option not in COMMON_OPTIONS_WITH_VALUES else 2
            continue
        if value.lower() in commands:
            values[index] = value.lower()
        break
    return values


def split_windows_command_line(line: str) -> list[str]:
    arguments: list[str] = []
    current: list[str] = []
    in_quotes = False
    token_started = False
    index = 0
    while index < len(line):
        character = line[index]
        if character in " \t" and not in_quotes:
            if token_started:
                arguments.append("".join(current))
                current.clear()
                token_started = False
            index += 1
            continue
        if character == "\\":
            slash_start = index
            while index < len(line) and line[index] == "\\":
                index += 1
            slash_count = index - slash_start
            # Windows only treats backslashes as escapes when they immediately precede a quote.
            if index < len(line) and line[index] == '"':
                current.extend("\\" * (slash_count // 2))
                token_started = True
                if slash_count % 2:
                    current.append('"')
                else:
                    in_quotes = not in_quotes
                index += 1
            else:
                current.extend("\\" * slash_count)
                token_started = True
            continue
        if character == '"':
            token_started = True
            in_quotes = not in_quotes
            index += 1
            continue
        current.append(character)
        token_started = True
        index += 1
    if in_quotes:
        raise BuildToolError("Invalid shell command: unmatched double quote.")
    if token_started:
        arguments.append("".join(current))
    return arguments


def split_shell_command(line: str, *, current_host: str) -> list[str]:
    if current_host == "windows":
        return split_windows_command_line(line)
    try:
        return shlex.split(line, posix=True)
    except ValueError as exc:
        raise BuildToolError(f"Invalid shell command: {exc}.") from exc


NAMESPACE_FIELDS = {
    "target": "target",
    "jobs": "jobs",
    "filter": "test_filter",
    "timeout": "test_timeout_seconds",
    "run_arguments": "run_arguments",
    "profile": "profile",
    "preset": "preset",
    "cmake": "cmake",
    "environment_setup": "environment_setup",
    "all_presets": "all_presets",
    "yes": "yes",
    "fresh": "fresh",
    "plain": "plain",
    "output_mode": "output_mode",
}


def namespace_request(
    args: argparse.Namespace,
    *,
    defaults: CommandRequest | None = None,
) -> CommandRequest:
    request = defaults or CommandRequest(Action.SHELL)
    changes: dict[str, Any] = {"action": Action(args.action or Action.SHELL.value)}
    for namespace_name, request_name in NAMESPACE_FIELDS.items():
        if hasattr(args, namespace_name):
            value = getattr(args, namespace_name)
            if request_name == "output_mode":
                value = OutputMode(value)
            changes[request_name] = tuple(value) if request_name == "run_arguments" else value
    return replace(request, **changes)


def argument_flag(spec: CommandSpec, dest: str) -> str:
    return next(argument.flags[0] for argument in spec.arguments if argument.dest == dest)


def parse_request(
    argv: Sequence[str],
    *,
    defaults: CommandRequest | None = None,
) -> CommandRequest:
    args = make_parser().parse_args(normalize_action(argv))
    action = Action(args.action or Action.SHELL.value)
    spec = COMMAND_BY_NAME[action.value]
    supplied = set(vars(args)) - {"action"}
    allowed = {argument.dest for argument in spec.arguments}
    unsupported = sorted(supplied - allowed)
    if unsupported:
        flags = ", ".join(argument_flag(COMMAND_BY_NAME[Action.SHELL.value], name) for name in unsupported)
        raise BuildToolError(f"{spec.name} does not accept {flags}.")
    request = namespace_request(args, defaults=defaults)
    clean_defaults = CommandRequest(action)
    cleared = {
        request_name: getattr(clean_defaults, request_name)
        for namespace_name, request_name in NAMESPACE_FIELDS.items()
        if namespace_name not in allowed
    }
    return replace(request, **cleared)


def parse_args(argv: Sequence[str] | None = None) -> CommandRequest:
    values = normalize_action(sys.argv[1:] if argv is None else argv)
    return parse_request(values)


def normalize_shell_command(parts: Sequence[str]) -> list[str]:
    spec = SHELL_COMMANDS.get(parts[0].lower())
    if spec is None or spec.action is Action.SHELL:
        raise BuildToolError(f'Unknown shell command "{parts[0]}". Type help for available commands.')
    command = spec.name
    values = list(parts[1:])
    option_by_flag = {
        flag: argument
        for argument in spec.arguments
        for flag in argument.flags
    }
    normalized: list[str] = []
    positionals: list[str] = []
    supplied_destinations: set[str] = set()
    index = 0
    while index < len(values):
        value = values[index]
        flag = value.partition("=")[0]
        argument = option_by_flag.get(flag)
        if argument is None:
            if spec.action is Action.RUN:
                normalized.extend(["--args", *values[index:]])
                supplied_destinations.add("run_arguments")
                index = len(values)
                break
            if value.startswith("-"):
                normalized.append(value)
                index += 1
                continue
            positionals.append(value)
            index += 1
            continue
        normalized.append(value)
        supplied_destinations.add(argument.dest)
        if argument.kwargs.get("nargs") is argparse.REMAINDER:
            normalized.extend(values[index + 1 :])
            index = len(values)
            break
        if "=" not in value and argument.kwargs.get("action") != "store_true":
            if index + 1 < len(values):
                normalized.append(values[index + 1])
            index += 2
        else:
            index += 1

    available_operands = [
        operand for operand in spec.compact_operands if operand not in supplied_destinations
    ]
    if len(positionals) > len(available_operands):
        if spec.action in {Action.BUILD, Action.REBUILD}:
            raise BuildToolError(f"{command} accepts at most one target.")
        if spec.action is Action.TEST:
            raise BuildToolError("test requires a target and accepts an optional GoogleTest filter.")
        raise BuildToolError(f"{command} does not accept positional arguments.")
    for operand, value in zip(available_operands, positionals):
        normalized.extend([argument_flag(spec, operand), value])
    return [command, *normalized]


def parse_shell_request(
    parts: Sequence[str],
    session: CommandRequest,
    *,
    current_preset: str,
) -> CommandRequest:
    defaults = replace(session, action=Action.SHELL, preset=current_preset)
    return parse_request(normalize_shell_command(parts), defaults=defaults)


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
    output.info(escape(shell_command_help()))


def show_presets(output: BuildOutput, context: BuildContext, current_preset: str) -> None:
    if output.plain:
        for index, preset in enumerate(context.profile.presets, start=1):
            markers = []
            if preset == context.profile.default_preset:
                markers.append("default")
            if preset == current_preset:
                markers.append("current")
            suffix = f' [{", ".join(markers)}]' if markers else ""
            output.info(escape(f"  {index:>2}  {preset}{suffix}"))
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
    raise BuildToolError(f'Unknown preset "{value}". Use its full name or run presets.')


def resolve_shell_preset_number(value: str, context: BuildContext) -> str:
    if value.isdigit() and 1 <= int(value) <= len(context.profile.presets):
        return context.profile.presets[int(value) - 1]
    raise BuildToolError(f'Invalid preset number "{value}". Enter a number shown by presets.')


def show_status(output: BuildOutput, context: BuildContext) -> None:
    marker = interruption_marker_path(context.preset.name)
    toolchain_resolved = context.environment is not None
    cmake_default = context.request.cmake or next(
        (os.environ[name].strip() for name in CMAKE_ENV_VARS if os.environ.get(name, "").strip()),
        "",
    )
    cmake_default = cmake_default or context.config.cmake_command or "cmake"
    if context.request.jobs is not None:
        jobs_default: object = context.request.jobs
    elif os.environ.get(JOBS_ENV_VAR, "").strip():
        jobs_default = os.environ[JOBS_ENV_VAR].strip()
    elif context.config.jobs:
        jobs_default = context.config.jobs
    else:
        jobs_default = "automatic"
    values = {
        "Profile": context.profile.name,
        "Preset": context.preset.name,
        "Build directory": preset_build_directory(context.preset),
        "Configuration": preset_cache_string(context.preset, "CMAKE_BUILD_TYPE"),
        "Toolchain context": "resolved" if toolchain_resolved else "unresolved",
        "Parallel jobs": context.jobs or f"unresolved (default: {jobs_default})",
        "CMake": context.cmake or f'unresolved (default: {cmake_default})',
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
    base = create_context(request, prepare_tools=False)
    current_preset = base.preset.name
    output.info("Durin BuildTool shell")
    show_status(output, base)
    output.info("Type help for available commands.")
    selecting_preset = False
    while True:
        try:
            line = input("Preset> " if selecting_preset else "BuildTool> ").strip()
        except EOFError:
            output.info("")
            return
        except KeyboardInterrupt:
            if selecting_preset:
                selecting_preset = False
                output.cancelled("Preset selection cancelled; current preset unchanged.")
                continue
            output.warning("Use exit to leave the shell.")
            continue
        operation_started = perf_counter()
        if selecting_preset:
            selecting_preset = False
            if not line:
                output.cancelled("Preset selection cancelled; current preset unchanged.")
                continue
            try:
                current_preset = resolve_shell_preset_number(line, base)
                output.success(f'CMake preset selected: "{current_preset}"')
            except BuildToolError as exc:
                output.failure(exc, None, perf_counter() - operation_started)
            continue
        if not line:
            continue
        child_request: CommandRequest | None = None
        child_context: BuildContext | None = None
        try:
            parts = split_shell_command(line, current_host=base.current_host)
            shell_name = parts[0].lower()
            spec = SHELL_COMMANDS.get(shell_name)
            command = spec.name if spec is not None else SHELL_CONTROL_ALIASES.get(shell_name, shell_name)
            values = parts[1:]
            if spec is not None and spec.action is not Action.SHELL:
                child_request = replace(request, action=spec.action, preset=current_preset)
            if command in {"exit", "quit"}:
                return
            if command in {"help", "?"}:
                print_shell_help(output)
                continue
            if command == "preset":
                if not values:
                    output.info(f'CMake preset: "{current_preset}"')
                    continue
                if len(values) != 1:
                    raise BuildToolError("preset accepts one full preset name.")
                current_preset = resolve_shell_preset(values[0], base)
                output.success(f'CMake preset selected: "{current_preset}"')
                continue
            child_request = parse_shell_request(parts, request, current_preset=current_preset)
            child_output = (
                output
                if (
                    child_request.plain == request.plain
                    and child_request.output_mode == request.output_mode
                )
                else BuildOutput(
                    plain=child_request.plain,
                    output_mode=child_request.output_mode,
                    stdout=output.console.file,
                    stderr=output.error_console.file,
                )
            )
            if child_request.action is Action.STOP:
                if stop_active_operation():
                    child_output.success("Stopped the active BuildTool operation.")
                else:
                    child_output.info("No active BuildTool operation was found.")
                continue
            session_profile = request.profile or base.profile.name
            needs_independent_context = (
                child_request.profile not in {"", session_profile}
                or child_request.environment_setup != request.environment_setup
                or (
                    child_request.action not in TOOLCHAIN_ACTIONS
                    and child_request.cmake != request.cmake
                )
            )
            if needs_independent_context:
                prepare_tools = child_request.action in TOOLCHAIN_ACTIONS
                child_context = create_context(child_request, prepare_tools=prepare_tools)
            else:
                child_context = derive_context(base, child_request)
                if child_request.action in TOOLCHAIN_ACTIONS and base.environment is None:
                    prepare_toolchain_environment(base)
                    child_context = derive_context(base, child_request)
                needs_command_preparation = (
                    not child_context.cmake
                    or not child_context.jobs
                    or child_request.cmake != request.cmake
                    or child_request.jobs != request.jobs
                )
                if child_request.action in TOOLCHAIN_ACTIONS and needs_command_preparation:
                    prepare_command_context(child_context)
                    if child_request.cmake == request.cmake:
                        base.cmake = child_context.cmake
                    if child_request.jobs == request.jobs:
                        base.jobs = child_context.jobs
            if child_request.action is Action.PRESETS:
                show_presets(child_output, child_context, child_context.preset.name)
                child_output.info("Enter a preset number, or press Enter to keep the current preset.")
                selecting_preset = True
                continue
            if child_request.action is Action.STATUS:
                show_status(child_output, child_context)
                continue
            if child_request.action is Action.OPEN_RUNTIME:
                open_runtime_directory(child_context, child_output)
                continue
            execute_context(
                child_context,
                child_output,
                confirm_purge=lambda paths, all_presets: confirm_purge(
                    child_output, paths, all_presets
                ),
            )
        except (BuildToolError, ValueError) as exc:
            error = exc if isinstance(exc, BuildToolError) else BuildToolError(f"Invalid command: {exc}")
            output.failure(
                error,
                child_context,
                perf_counter() - operation_started,
                request=child_request,
                preset=current_preset,
            )


def main(argv: Sequence[str] | None = None) -> int:
    started = perf_counter()
    output: BuildOutput | None = None
    request: CommandRequest | None = None
    context: BuildContext | None = None
    try:
        request = parse_args(argv)
        output = BuildOutput(plain=request.plain, output_mode=request.output_mode)
        if request.action is Action.SHELL:
            run_shell(request, output)
            return 0
        if request.action is Action.STOP:
            if stop_active_operation():
                output.success("Stopped the active BuildTool operation.")
            else:
                output.info("No active BuildTool operation was found.")
            return 0
        prepare_tools = request.action in TOOLCHAIN_ACTIONS
        context = create_context(request, prepare_tools=prepare_tools)
        if request.action is Action.PRESETS:
            show_presets(output, context, context.preset.name)
            return 0
        if request.action is Action.STATUS:
            show_status(output, context)
            return 0
        if request.action is Action.OPEN_RUNTIME:
            open_runtime_directory(context, output)
            return 0
        execute_context(
            context,
            output,
            confirm_purge=lambda paths, all_presets: confirm_purge(output, paths, all_presets),
        )
        return 0
    except BuildToolError as exc:
        output = output or BuildOutput(plain="--plain" in (argv or sys.argv[1:]))
        output.failure(exc, context, perf_counter() - started, request=request)
        return 1
    except OSError as exc:
        output = output or BuildOutput(plain="--plain" in (argv or sys.argv[1:]))
        output.failure(
            BuildToolError(f"Operating system error: {exc}"),
            context,
            perf_counter() - started,
            request=request,
        )
        return 1
