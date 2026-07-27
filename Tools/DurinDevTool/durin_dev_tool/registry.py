from __future__ import annotations

import argparse
import importlib
import importlib.util
import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Callable, Sequence, TextIO

from .errors import DevToolError


class Capability(Enum):
    BOOTSTRAP = "bootstrap"
    PREPARED_ENVIRONMENT = "prepared-environment"


@dataclass(frozen=True)
class ArgumentSpec:
    flags: tuple[str, ...]
    kwargs: dict[str, object]


@dataclass(frozen=True)
class CommandSpec:
    name: str
    summary: str
    handler: str = ""
    capability: Capability = Capability.BOOTSTRAP
    arguments: tuple[ArgumentSpec, ...] = ()
    required_modules: tuple[str, ...] = ()
    subcommands: tuple["CommandSpec", ...] = ()
    defaults: tuple[tuple[str, object], ...] = ()

    def load_handler(self) -> Callable[..., int]:
        module_name, separator, attribute = self.handler.partition(":")
        if not separator:
            raise DevToolError(f'Invalid handler registration for "{self.name}".')
        module = importlib.import_module(module_name)
        return getattr(module, attribute)


def _argument(*flags: str, **kwargs: object) -> ArgumentSpec:
    return ArgumentSpec(flags, kwargs)


PROFILE = _argument("--profile", help="host build profile")
PRESET = _argument("--preset", help="registered CMake configure preset")
CMAKE = _argument("--cmake", help="CMake executable override")
ENVIRONMENT_SETUP = _argument(
    "--environment-setup",
    help="toolchain environment script override",
)
JOBS = _argument(
    "--jobs",
    type=int,
    choices=range(1, 257),
    metavar="1..256",
    help="parallel build job limit",
)
PLAIN = _argument(
    "--plain",
    action="store_true",
    help="disable colors and styled terminal output",
)
OUTPUT_MODE = _argument(
    "--output",
    dest="output_mode",
    choices=("auto", "compact", "progress", "full"),
    default="auto",
    help="child output mode (default: auto)",
)
TOOL_ARGUMENTS = (PROFILE, PRESET, CMAKE, ENVIRONMENT_SETUP, JOBS, PLAIN, OUTPUT_MODE)
CONTEXT_ARGUMENTS = (PROFILE, PRESET, PLAIN, OUTPUT_MODE)
BUILD_HANDLER = "durin_dev_tool.build.handler:run"
BUILD_CAPABILITY = Capability.PREPARED_ENVIRONMENT
BUILD_MODULES = ("rich",)
DOCUMENTATION_HANDLER = "durin_dev_tool.documentation.handler:run"
PLAN_SCOPES = ("active", "completed", "archive", "all")
BOOTSTRAP_HANDLER = "durin_dev_tool.bootstrap.handler:run"
WORKTREE_HANDLER = "durin_dev_tool.worktree.handler:run"


def _build_command(
    name: str,
    summary: str,
    arguments: tuple[ArgumentSpec, ...],
    *,
    action: str | None = None,
) -> CommandSpec:
    return CommandSpec(
        name,
        summary,
        BUILD_HANDLER,
        capability=BUILD_CAPABILITY,
        arguments=arguments,
        required_modules=BUILD_MODULES,
        defaults=(("build_action", action or name),),
    )


CREATE_MODULE = _build_command(
    "module",
    "create and register a module",
    (
        _argument("create_name", metavar="NAME"),
        _argument("--project", dest="project_path", type=Path, required=True),
        _argument("--path", dest="destination_path", type=Path, default=None),
        _argument("--kind", dest="module_kind", choices=("runtime", "editor"), default="runtime"),
        _argument("--link", dest="link_type", choices=("shared", "static"), default="shared"),
        _argument("--pch", default=""),
        _argument("--public-dependency", dest="public_dependencies", action="append", default=None),
        _argument("--private-dependency", dest="private_dependencies", action="append", default=None),
        _argument(
            "--optional-public-dependency",
            dest="optional_public_dependencies",
            action="append",
            default=None,
        ),
        _argument(
            "--optional-private-dependency",
            dest="optional_private_dependencies",
            action="append",
            default=None,
        ),
        _argument("--enable", dest="enablements", action="append", default=None),
        _argument("--dry-run", action="store_true"),
        PLAIN,
    ),
    action="create-module",
)
CREATE_PROJECT = _build_command(
    "project",
    "create and register a workspace project",
    (
        _argument("create_name", metavar="NAME"),
        _argument("--path", dest="destination_path", type=Path, required=True),
        _argument("--dry-run", action="store_true"),
        PLAIN,
    ),
    action="create-project",
)

PLAN_LIST = CommandSpec(
    "list",
    "list implementation plans",
    DOCUMENTATION_HANDLER,
    arguments=(
        _argument("--scope", choices=PLAN_SCOPES, default="active"),
        _argument("--query", help="filter by title or filename"),
        _argument(
            "--all-results",
            action="store_true",
            help="allow an unfiltered archive or all-scope listing",
        ),
        _argument(
            "--format",
            choices=("markdown", "terminal"),
            default=None,
            dest="output_format",
        ),
        _argument(
            "--color",
            choices=("auto", "always", "never"),
            default="auto",
        ),
    ),
    defaults=(("plan_action", "list"),),
)
PLAN_VALIDATE = CommandSpec(
    "validate",
    "validate plan metadata and layout",
    DOCUMENTATION_HANDLER,
    arguments=(_argument("--scope", choices=PLAN_SCOPES, default="all"),),
    defaults=(("plan_action", "validate"),),
)
PLAN_ARCHIVE = CommandSpec(
    "archive",
    "archive one completion month",
    DOCUMENTATION_HANDLER,
    arguments=(
        _argument("month", help="completion month in YYYY-MM form"),
        _argument(
            "--apply",
            action="store_true",
            help="apply the transaction; the default is a dry-run",
        ),
    ),
    defaults=(("plan_action", "archive"),),
)

DEPENDENCY_PREPARE = CommandSpec(
    "prepare",
    "prepare selected third-party dependencies",
    BOOTSTRAP_HANDLER,
    arguments=(
        _argument(
            "--all",
            dest="all_dependencies",
            action="store_true",
            help="prepare all non-test dependencies",
        ),
        _argument(
            "--libs",
            dest="libraries",
            help="comma-separated dependencies to prepare",
        ),
        _argument(
            "--config",
            dest="dependency_config",
            choices=("Debug", "Release", "All"),
            default="All",
        ),
        _argument("--with-tests", action="store_true"),
        _argument("--with-development", action="store_true"),
        _argument("--cmake", dest="dependency_cmake", default=None),
    ),
    defaults=(("bootstrap_action", "dependency-prepare"),),
)
DEPENDENCY_VALIDATE = CommandSpec(
    "validate",
    "validate every third-party dependency manifest",
    BOOTSTRAP_HANDLER,
    defaults=(("bootstrap_action", "dependency-validate"),),
)

WORKTREE_OPEN = CommandSpec(
    "open",
    "open terminals for every worktree",
    WORKTREE_HANDLER,
    arguments=(_argument("--dry-run", action="store_true"),),
    defaults=(("worktree_action", "open"),),
)
WORKTREE_LIST = CommandSpec(
    "list",
    "list registered worktrees",
    WORKTREE_HANDLER,
    defaults=(("worktree_action", "list"),),
)
WORKTREE_ADD = CommandSpec(
    "add",
    "create and prepare a worktree",
    WORKTREE_HANDLER,
    arguments=(
        _argument("path", help="new worktree path"),
        _argument("commit_ish", nargs="?", help="commit or branch to check out"),
        _argument("-b", "--branch", help="create and check out a new branch"),
        _argument("--detach", action="store_true"),
        _argument("--source", help="prepared source worktree or Engine/External path"),
        _argument(
            "--link-type",
            choices=("auto", "junction", "symlink"),
            default="auto",
        ),
    ),
    defaults=(("worktree_action", "add"),),
)
WORKTREE_PREPARE = CommandSpec(
    "prepare",
    "prepare or repair a linked worktree",
    WORKTREE_HANDLER,
    arguments=(
        _argument("path", nargs="?", help="linked worktree path"),
        _argument("--source", help="prepared source worktree or Engine/External path"),
        _argument(
            "--link-type",
            choices=("auto", "junction", "symlink"),
            default="auto",
        ),
        _argument("--dry-run", action="store_true"),
    ),
    defaults=(("worktree_action", "prepare"),),
)
WORKTREE_REMOVE = CommandSpec(
    "remove",
    "safely remove a linked worktree",
    WORKTREE_HANDLER,
    arguments=(
        _argument("path", help="linked worktree path"),
        _argument("--force", action="store_true"),
        _argument("--dry-run", action="store_true"),
    ),
    defaults=(("worktree_action", "remove"),),
)


COMMAND_SPECS = (
    CommandSpec(
        "help",
        "show command help",
        "durin_dev_tool.commands.core:show_help",
    ),
    CommandSpec(
        "shell",
        "open the interactive shell",
        "durin_dev_tool.commands.core:open_shell",
    ),
    CommandSpec(
        "setup",
        "prepare this main checkout",
        BOOTSTRAP_HANDLER,
        defaults=(("bootstrap_action", "setup"),),
    ),
    _build_command("stop", "stop the active build operation", (PLAIN,)),
    _build_command("presets", "list registered presets", CONTEXT_ARGUMENTS),
    _build_command(
        "preset",
        "inspect a selected preset",
        (
            _argument("selected_preset", nargs="?", default=""),
            PROFILE,
            PLAIN,
            OUTPUT_MODE,
        ),
    ),
    _build_command("status", "show build context and toolchain state", TOOL_ARGUMENTS),
    _build_command(
        "open-runtime",
        "open the selected preset's runtime directory",
        CONTEXT_ARGUMENTS,
    ),
    _build_command(
        "configure",
        "configure the selected preset",
        TOOL_ARGUMENTS + (_argument("--fresh", action="store_true"),),
    ),
    _build_command(
        "build",
        "build a CMake target",
        TOOL_ARGUMENTS + (_argument("--target", default="all"),),
    ),
    _build_command("clean", "clean the selected preset", TOOL_ARGUMENTS),
    _build_command(
        "recover",
        "resume an interrupted build incrementally",
        TOOL_ARGUMENTS,
    ),
    _build_command(
        "purge",
        "delete generated build artifacts",
        CONTEXT_ARGUMENTS
        + (
            _argument("--all-presets", action="store_true"),
            _argument("--yes", action="store_true"),
        ),
    ),
    _build_command(
        "rebuild",
        "clean, configure, and build",
        TOOL_ARGUMENTS + (_argument("--target", default="all"),),
    ),
    _build_command(
        "test",
        "build and run a native test target",
        TOOL_ARGUMENTS
        + (
            _argument("--target", required=True),
            _argument("--filter", default=""),
            _argument(
                "--timeout",
                type=int,
                choices=range(0, 86401),
                default=300,
                metavar="0..86400",
            ),
        ),
    ),
    _build_command(
        "run",
        "run the selected preset's existing application",
        CONTEXT_ARGUMENTS
        + (
            _argument("--project", dest="project_path", type=Path, default=None),
            _argument("--args", dest="run_arguments", nargs=argparse.REMAINDER, default=()),
        ),
    ),
    CommandSpec(
        "create",
        "create a module or workspace project",
        subcommands=(CREATE_MODULE, CREATE_PROJECT),
    ),
    CommandSpec(
        "plan",
        "list, validate, and archive implementation plans",
        subcommands=(PLAN_LIST, PLAN_VALIDATE, PLAN_ARCHIVE),
    ),
    CommandSpec(
        "dependency",
        "prepare and validate third-party dependencies",
        subcommands=(DEPENDENCY_PREPARE, DEPENDENCY_VALIDATE),
    ),
    CommandSpec(
        "worktree",
        "create, prepare, inspect, open, and remove worktrees",
        subcommands=(
            WORKTREE_OPEN,
            WORKTREE_LIST,
            WORKTREE_ADD,
            WORKTREE_PREPARE,
            WORKTREE_REMOVE,
        ),
    ),
)


class DevToolArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise DevToolError(f"{message}\nRun 'DevTool help' for command usage.")


class CommandRegistry:
    def __init__(self, specifications: Sequence[CommandSpec] = COMMAND_SPECS) -> None:
        self.specifications = tuple(specifications)
        self._by_name = {spec.name: spec for spec in self.specifications}

    def parser(self) -> DevToolArgumentParser:
        parser = DevToolArgumentParser(
            prog="DevTool",
            description="Set up, inspect, build, test, and run Durin.",
        )
        parser.set_defaults(command="shell")
        subparsers = parser.add_subparsers(dest="command", metavar="COMMAND")
        for spec in self.specifications:
            command_parser = subparsers.add_parser(
                spec.name,
                help=spec.summary,
                description=spec.summary,
            )
            self._configure_parser(command_parser, spec)
        return parser

    def _configure_parser(
        self,
        parser: argparse.ArgumentParser,
        spec: CommandSpec,
    ) -> None:
        parser.set_defaults(_command_spec=spec, **dict(spec.defaults))
        for argument in spec.arguments:
            parser.add_argument(*argument.flags, **argument.kwargs)
        if spec.subcommands:
            subparsers = parser.add_subparsers(
                dest=f"{spec.name}_command",
                metavar="COMMAND",
                required=True,
            )
            for child in spec.subcommands:
                child_parser = subparsers.add_parser(
                    child.name,
                    help=child.summary,
                    description=child.summary,
                )
                self._configure_parser(child_parser, child)

    def parse(self, arguments: Sequence[str]) -> tuple[CommandSpec, argparse.Namespace]:
        if not arguments:
            normalized = ["shell"]
        elif arguments[0] in {"-h", "--help", "/?"}:
            normalized = ["help"]
        else:
            normalized = list(arguments)
            command = normalized[0].removeprefix("/").lower()
            if command in self._by_name:
                normalized[0] = command
                parent = self._by_name[command]
                if parent.subcommands and len(normalized) > 1:
                    child = normalized[1].removeprefix("/").lower()
                    if child in {spec.name for spec in parent.subcommands}:
                        normalized[1] = child
        namespace = self.parser().parse_args(normalized)
        return namespace._command_spec, namespace

    def format_help(self) -> str:
        width = max(len(spec.name) for spec in self.specifications)
        lines = ["DurinDevTool commands:"]
        lines.extend(
            f"  {spec.name:<{width}}  {spec.summary}"
            for spec in self.specifications
        )
        lines.extend(
            [
                "",
                "Run 'DevTool COMMAND --help' for command-specific options.",
                "Run DevTool without arguments to open the interactive shell.",
            ]
        )
        return "\n".join(lines)

    def execute(
        self,
        spec: CommandSpec,
        namespace: argparse.Namespace,
        *,
        repository_root: Path,
        stdout: TextIO,
        stderr: TextIO,
        session_state: dict[str, object] | None = None,
    ) -> int:
        if spec.capability is Capability.PREPARED_ENVIRONMENT:
            require_prepared_environment(
                repository_root,
                required_modules=spec.required_modules,
            )
        handler = spec.load_handler()
        keywords = {
            "registry": self,
            "repository_root": repository_root,
            "stdout": stdout,
            "stderr": stderr,
        }
        if session_state is not None:
            keywords["session_state"] = session_state
        return handler(
            namespace,
            **keywords,
        )


def require_prepared_environment(
    repository_root: Path,
    *,
    required_modules: Sequence[str] = (),
) -> None:
    interpreter = repository_root / ".venv" / "Scripts" / "python.exe"
    if not interpreter.is_file():
        raise DevToolError(
            "Durin's prepared Python environment is missing. "
            "Run 'DevTool setup' in the main checkout, or "
            "'DevTool worktree prepare' in a linked worktree."
        )
    active_interpreter = Path(sys.executable)
    try:
        interpreter_matches = interpreter.samefile(active_interpreter)
    except OSError:
        interpreter_matches = interpreter.resolve() == active_interpreter.resolve()
    if not interpreter_matches:
        raise DevToolError(
            "Durin's prepared Python environment exists, but DevTool is "
            f'running with "{active_interpreter}". Restart through '
            "'DevTool.bat' so it selects the prepared environment."
        )
    missing_modules = tuple(
        module
        for module in required_modules
        if importlib.util.find_spec(module) is None
    )
    if missing_modules:
        missing = ", ".join(missing_modules)
        raise DevToolError(
            "Durin's prepared Python environment is incomplete "
            f"(missing Python packages: {missing}). Run 'DevTool setup' "
            "from the main checkout to repair it, then restart DevTool."
        )
