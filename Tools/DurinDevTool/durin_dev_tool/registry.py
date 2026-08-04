from __future__ import annotations

import argparse
import importlib
import importlib.util
import sys
from dataclasses import dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Callable, Mapping, Sequence, TextIO

from .configuration import load_repository_config
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
    default_subcommand: str = ""
    defaults: tuple[tuple[str, object], ...] = ()
    feature: str = ""

    def load_handler(self) -> Callable[..., int]:
        module_name, separator, attribute = self.handler.partition(":")
        if not separator:
            raise DevToolError(f'Invalid handler registration for "{self.name}".')
        module = importlib.import_module(module_name)
        return getattr(module, attribute)


@dataclass(frozen=True)
class CommandAlias:
    name: str
    expansion: tuple[str, ...]
    warning: str = ""


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
AGENT = _argument(
    "--agent",
    action="store_true",
    help="use stable compact output with liveness heartbeats for Agent execution",
)
NON_INTERACTIVE = _argument(
    "--non-interactive",
    action="store_true",
    help="use configured or automatically detected toolchain settings without prompting",
)
OUTPUT_MODE = _argument(
    "--output",
    dest="output_mode",
    choices=("auto", "compact", "progress", "full"),
    default=None,
    help="child output mode (default: auto)",
)
CONTEXT_ARGUMENTS = (PROFILE, PRESET)
DISPLAY_ARGUMENTS = (PLAIN,)
CHILD_OUTPUT_ARGUMENTS = (PLAIN, OUTPUT_MODE)
TOOL_ARGUMENTS = (
    PROFILE,
    PRESET,
    CMAKE,
    ENVIRONMENT_SETUP,
    JOBS,
    AGENT,
    PLAIN,
    OUTPUT_MODE,
)
STATUS_ARGUMENTS = (PROFILE, PRESET, CMAKE, ENVIRONMENT_SETUP, JOBS, PLAIN)
BUILD_HANDLER = "durin_dev_tool.build.handler:run"
BUILD_CAPABILITY = Capability.PREPARED_ENVIRONMENT
BUILD_MODULES = ("rich",)
DOCUMENTATION_HANDLER = "durin_dev_tool.documentation.handler:run"
PLAN_SCOPES = ("active", "completed", "archive", "all")
DOCUMENT_KINDS = (
    "router",
    "contract",
    "guide",
    "task",
    "plan",
    "investigation",
    "policy",
    "generic",
)
CREATABLE_DOCUMENT_KINDS = ("router", "contract", "guide", "generic")
BOOTSTRAP_HANDLER = "durin_dev_tool.bootstrap.handler:run"
WORKTREE_HANDLER = "durin_dev_tool.worktree.handler:run"
ASSET_HANDLER = "durin_dev_tool.asset:run"


def _build_command(
    name: str,
    summary: str,
    arguments: tuple[ArgumentSpec, ...],
    *,
    action: str | None = None,
    feature: str = "build",
) -> CommandSpec:
    return CommandSpec(
        name,
        summary,
        BUILD_HANDLER,
        capability=BUILD_CAPABILITY,
        arguments=arguments,
        required_modules=BUILD_MODULES,
        defaults=(("build_action", action or name),),
        feature=feature,
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
    feature="scaffolding",
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
    feature="scaffolding",
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

DOCUMENT_OUTPUT = _argument(
    "--format",
    choices=("markdown", "terminal", "json"),
    default=None,
    dest="output_format",
)
DOCUMENT_ARCHIVE = _argument(
    "--include-archive",
    action="store_true",
    help="include archived implementation plans",
)
DOCUMENT_FILTERS = (
    _argument("--under", help="repository-relative Documentation path"),
    _argument(
        "--kind",
        dest="kinds",
        choices=DOCUMENT_KINDS,
        action="append",
        default=None,
    ),
    DOCUMENT_ARCHIVE,
    DOCUMENT_OUTPUT,
)
DOCUMENT_LIST = CommandSpec(
    "list",
    "list repository documentation",
    DOCUMENTATION_HANDLER,
    arguments=DOCUMENT_FILTERS,
    defaults=(("document_action", "list"),),
)
DOCUMENT_FIND = CommandSpec(
    "find",
    "find documentation by title or path",
    DOCUMENTATION_HANDLER,
    arguments=(
        _argument("document_query", metavar="QUERY"),
        *DOCUMENT_FILTERS,
    ),
    defaults=(("document_action", "find"),),
)
DOCUMENT_REFS = CommandSpec(
    "refs",
    "show inbound and outbound document references",
    DOCUMENTATION_HANDLER,
    arguments=(
        _argument("document_path", metavar="PATH"),
        DOCUMENT_ARCHIVE,
        DOCUMENT_OUTPUT,
    ),
    defaults=(("document_action", "refs"),),
)
DOCUMENT_VALIDATE = CommandSpec(
    "validate",
    "validate repository documentation",
    DOCUMENTATION_HANDLER,
    arguments=(
        _argument(
            "--scope",
            choices=("all", "changed"),
            default="all",
        ),
        DOCUMENT_ARCHIVE,
        DOCUMENT_OUTPUT,
    ),
    defaults=(("document_action", "validate"),),
)
DOCUMENT_CREATE = CommandSpec(
    "create",
    "preview or create a documentation file",
    DOCUMENTATION_HANDLER,
    arguments=(
        _argument(
            "document_kind",
            choices=CREATABLE_DOCUMENT_KINDS,
            metavar="KIND",
        ),
        _argument("document_path", metavar="PATH"),
        _argument("--title", required=True),
        _argument("--summary", default=""),
        _argument("--apply", action="store_true"),
        DOCUMENT_OUTPUT,
    ),
    defaults=(("document_action", "create"),),
)
DOCUMENT_MOVE = CommandSpec(
    "move",
    "preview or move a document and repair references",
    DOCUMENTATION_HANDLER,
    arguments=(
        _argument("source_path", metavar="SOURCE"),
        _argument("destination_path", metavar="DESTINATION"),
        _argument("--apply", action="store_true"),
        DOCUMENT_OUTPUT,
    ),
    defaults=(("document_action", "move"),),
)
TASK_LIST = CommandSpec(
    "list",
    "list open engineering tasks",
    DOCUMENTATION_HANDLER,
    arguments=(
        _argument(
            "--query",
            dest="task_query",
            help="filter by title, outcome, or filename",
        ),
        DOCUMENT_OUTPUT,
    ),
    defaults=(("task_action", "list"),),
)
TASK_VALIDATE = CommandSpec(
    "validate",
    "validate open engineering tasks",
    DOCUMENTATION_HANDLER,
    arguments=(DOCUMENT_OUTPUT,),
    defaults=(("task_action", "validate"),),
)
TASK_REMOVE = CommandSpec(
    "remove",
    "preview or remove an open engineering task",
    DOCUMENTATION_HANDLER,
    arguments=(
        _argument("task_path", metavar="PATH"),
        _argument("--apply", action="store_true"),
        DOCUMENT_OUTPUT,
    ),
    defaults=(("task_action", "remove"),),
)
DOCUMENT_TASK = CommandSpec(
    "task",
    "discover, validate, and remove open engineering tasks",
    subcommands=(TASK_LIST, TASK_VALIDATE, TASK_REMOVE),
    default_subcommand="list",
)
DOCUMENT_PLAN = CommandSpec(
    "plan",
    "manage implementation-plan lifecycle",
    subcommands=(PLAN_LIST, PLAN_VALIDATE, PLAN_ARCHIVE),
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
        arguments=(_argument("command_path", nargs="*"),),
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
        arguments=(PLAIN, NON_INTERACTIVE),
        defaults=(("bootstrap_action", "setup"),),
        feature="setup",
    ),
    _build_command("stop", "stop the active build operation", (PLAIN,)),
    _build_command(
        "presets",
        "list registered presets",
        CONTEXT_ARGUMENTS + DISPLAY_ARGUMENTS,
    ),
    _build_command(
        "preset",
        "inspect a selected preset",
        (
            _argument("selected_preset", nargs="?", default=""),
            PROFILE,
            PLAIN,
        ),
    ),
    _build_command("status", "show build context and toolchain state", STATUS_ARGUMENTS),
    _build_command(
        "path",
        "print a registered repository location",
        CONTEXT_ARGUMENTS
        + DISPLAY_ARGUMENTS
        + (
            _argument("location", nargs="?"),
            _argument("--all", dest="all_locations", action="store_true"),
        ),
    ),
    _build_command(
        "open",
        "open a registered repository location",
        CONTEXT_ARGUMENTS + DISPLAY_ARGUMENTS + (_argument("location"),),
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
        + DISPLAY_ARGUMENTS
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
        "build and run one or all native tests",
        TOOL_ARGUMENTS
        + (
            _argument(
                "--target",
                required=True,
                help="native test target, or 'all' for every CTest-registered test",
            ),
            _argument(
                "--filter",
                default="",
                help="GoogleTest filter for a single native test target",
            ),
            _argument(
                "--timeout",
                type=int,
                choices=range(0, 86401),
                default=300,
                metavar="0..86400",
                help="test timeout in seconds; 0 disables it (default: 300)",
            ),
            _argument(
                "--schedule-random",
                action="store_true",
                help="randomize CTest scheduling for --target all",
            ),
            _argument(
                "--output-junit",
                type=Path,
                default=None,
                help="write CTest JUnit XML for --target all",
            ),
            _argument(
                "--ctest-regex",
                default="",
                help="run CTest names matching a regex for --target all",
            ),
            _argument(
                "--include-direct",
                action="store_true",
                help="also run whole-target direct lifecycle tests for --target all",
            ),
        ),
    ),
    _build_command(
        "run",
        "run the selected preset's existing application",
        CONTEXT_ARGUMENTS
        + CHILD_OUTPUT_ARGUMENTS
        + (
            _argument("--project", dest="project_path", type=Path, default=None),
            _argument("--args", dest="run_arguments", nargs=argparse.REMAINDER, default=()),
        ),
    ),
    CommandSpec(
        "asset",
        "inspect authored asset packages without modifying them",
        subcommands=(
            CommandSpec(
                "audit",
                "run a deterministic read-only compatibility audit",
                ASSET_HANDLER,
                capability=Capability.PREPARED_ENVIRONMENT,
                arguments=CONTEXT_ARGUMENTS
                + (
                    _argument("--project", dest="project_path", type=Path, required=True),
                    _argument("--format", dest="format_name", choices=("human", "json"), default="human"),
                    _argument(
                        "--fail-on",
                        dest="fail_on",
                        choices=("incompatible", "unsupported", "error"),
                        action="append",
                        default=(),
                    ),
                ),
            ),
        ),
    ),
    CommandSpec(
        "create",
        "create a module or workspace project",
        subcommands=(CREATE_MODULE, CREATE_PROJECT),
        feature="scaffolding",
    ),
    CommandSpec(
        "doc",
        "discover, validate, and safely change documentation",
        subcommands=(
            DOCUMENT_LIST,
            DOCUMENT_FIND,
            DOCUMENT_REFS,
            DOCUMENT_VALIDATE,
            DOCUMENT_CREATE,
            DOCUMENT_MOVE,
            DOCUMENT_TASK,
            DOCUMENT_PLAN,
        ),
        feature="documentation",
    ),
    CommandSpec(
        "dependency",
        "prepare and validate third-party dependencies",
        subcommands=(DEPENDENCY_PREPARE, DEPENDENCY_VALIDATE),
        feature="dependencies",
    ),
    CommandSpec(
        "worktree",
        "create, prepare, list, open, and remove worktrees",
        subcommands=(
            WORKTREE_OPEN,
            WORKTREE_LIST,
            WORKTREE_ADD,
            WORKTREE_PREPARE,
            WORKTREE_REMOVE,
        ),
        default_subcommand="list",
        feature="worktrees",
    ),
)


COMMAND_ALIASES = (
    CommandAlias(
        "open-runtime",
        ("open", "runtime"),
        'Warning: "open-runtime" is deprecated; use "open runtime".',
    ),
)


class DevToolArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise DevToolError(f"{message}\nRun 'DevTool help' for command usage.")


class CommandRegistry:
    def __init__(
        self,
        specifications: Sequence[CommandSpec] = COMMAND_SPECS,
        *,
        aliases: Sequence[CommandAlias] = COMMAND_ALIASES,
        enabled_features: Mapping[str, bool] | None = None,
    ) -> None:
        features = (
            dict(enabled_features)
            if enabled_features is not None
            else dict(load_repository_config().features)
        )

        def enabled(specification: CommandSpec) -> CommandSpec | None:
            if specification.feature:
                if specification.feature not in features:
                    raise DevToolError(
                        f'Command "{specification.name}" references unknown '
                        f'DurinDevTool feature "{specification.feature}".'
                    )
                if not features[specification.feature]:
                    return None
            children = tuple(
                child
                for candidate in specification.subcommands
                if (child := enabled(candidate)) is not None
            )
            if specification.subcommands and not children:
                return None
            return replace(specification, subcommands=children)

        self.specifications = tuple(
            specification
            for candidate in specifications
            if (specification := enabled(candidate)) is not None
        )
        self._by_name = {spec.name: spec for spec in self.specifications}
        self._aliases_by_name = {
            alias.name.casefold(): alias
            for alias in aliases
            if alias.expansion and alias.expansion[0] in self._by_name
        }
        self._emitted_warnings: set[str] = set()

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
        warning = ""
        if not arguments:
            normalized = ["shell"]
        elif arguments[0] in {"-h", "--help", "/?"}:
            normalized = ["help"]
        else:
            normalized = list(arguments)
            command = normalized[0].removeprefix("/").lower()
            alias = self._aliases_by_name.get(command)
            if alias is not None:
                normalized = [*alias.expansion, *normalized[1:]]
                command = normalized[0]
                warning = alias.warning
            current = self._by_name.get(command)
            if current is not None:
                normalized[0] = command
                index = 1
                while current.subcommands:
                    if len(normalized) == index:
                        if not current.default_subcommand:
                            break
                        normalized.append(current.default_subcommand)
                    child_name = normalized[index].removeprefix("/").lower()
                    child = next(
                        (
                            candidate
                            for candidate in current.subcommands
                            if candidate.name == child_name
                        ),
                        None,
                    )
                    if child is None:
                        break
                    normalized[index] = child_name
                    current = child
                    index += 1
        namespace = self.parser().parse_args(normalized)
        namespace._deprecation_warning = warning
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

    def resolve_command_path(
        self,
        command_path: Sequence[str],
    ) -> tuple[CommandSpec, tuple[str, ...]]:
        if not command_path:
            raise DevToolError("a command path is required")
        first = command_path[0].removeprefix("/").casefold()
        alias = self._aliases_by_name.get(first)
        canonical_first = alias.expansion[0] if alias is not None else first
        current = self._by_name.get(canonical_first)
        if current is None:
            raise DevToolError(f'Unknown command "{command_path[0]}".')
        canonical_path = [current.name]
        for value in command_path[1:]:
            child_name = value.removeprefix("/").casefold()
            child = next(
                (
                    candidate
                    for candidate in current.subcommands
                    if candidate.name == child_name
                ),
                None,
            )
            if child is None:
                joined = " ".join(command_path)
                raise DevToolError(f'Unknown command path "{joined}".')
            current = child
            canonical_path.append(current.name)
        return current, tuple(canonical_path)

    def format_command_help(self, command_path: Sequence[str]) -> str:
        if not command_path:
            return self.format_help()
        spec, canonical_path = self.resolve_command_path(command_path)
        parser = DevToolArgumentParser(
            prog=f'DevTool {" ".join(canonical_path)}',
            description=spec.summary,
        )
        self._configure_parser(parser, spec)
        return parser.format_help().rstrip()

    def group_without_default(self, command: str) -> bool:
        normalized = command.removeprefix("/").casefold()
        spec = self._by_name.get(normalized)
        return bool(spec and spec.subcommands and not spec.default_subcommand)

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
        warning = str(getattr(namespace, "_deprecation_warning", ""))
        if warning and warning not in self._emitted_warnings:
            print(warning, file=stderr)
            self._emitted_warnings.add(warning)
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
    python_environment = load_repository_config().worktrees.python_environment
    interpreter = repository_root / python_environment / "Scripts" / "python.exe"
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
