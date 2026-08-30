from __future__ import annotations

import argparse
import importlib.util
import sys
from pathlib import Path
from typing import Sequence, TextIO

from .context import CommandIO, RepositoryContext
from .configuration import RepositoryConfigError
from .errors import DevToolError
from .python_environment import launcher_command, prepared_python_path
from .commands.asset_specs import COMMAND_SPEC as ASSET_COMMAND_SPEC
from .commands.bootstrap_specs import DEPENDENCY_COMMAND_SPEC, SETUP_COMMAND_SPEC
from .commands.build_specs import COMMAND_SPECS as BUILD_COMMAND_SPECS, SCAFFOLDING_COMMAND_SPEC
from .commands.core_specs import COMMAND_SPECS as CORE_COMMAND_SPECS
from .commands.cook_specs import COMMAND_SPEC as COOK_COMMAND_SPEC
from .commands.documentation_specs import COMMAND_SPEC as DOCUMENTATION_COMMAND_SPEC
from .commands.scene_specs import COMMAND_SPEC as SCENE_COMMAND_SPEC
from .commands.specification import CommandSpec
from .commands.worktree_specs import COMMAND_SPEC as WORKTREE_COMMAND_SPEC


COMMAND_SPECS = (
    *CORE_COMMAND_SPECS,
    SETUP_COMMAND_SPEC,
    *BUILD_COMMAND_SPECS,
    COOK_COMMAND_SPEC,
    ASSET_COMMAND_SPEC,
    SCENE_COMMAND_SPEC,
    SCAFFOLDING_COMMAND_SPEC,
    DOCUMENTATION_COMMAND_SPEC,
    DEPENDENCY_COMMAND_SPEC,
    WORKTREE_COMMAND_SPEC,
)


class DevToolArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        raise DevToolError(f"{message}\nRun 'DevTool help' for command usage.")


class CommandRegistry:
    def __init__(
        self,
        specifications: Sequence[CommandSpec] = COMMAND_SPECS,
    ) -> None:
        self.specifications = tuple(specifications)
        self._by_name = {spec.name: spec for spec in self.specifications}
        self._parser: DevToolArgumentParser | None = None

    def parser(self) -> DevToolArgumentParser:
        if self._parser is None:
            self._parser = self._create_parser()
        return self._parser

    def _create_parser(self) -> DevToolArgumentParser:
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
                epilog=spec.epilog or None,
                formatter_class=argparse.RawDescriptionHelpFormatter,
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
                    epilog=child.epilog or None,
                    formatter_class=argparse.RawDescriptionHelpFormatter,
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
                    if (
                        child is None
                        and current.default_subcommand
                        and normalized[index].startswith("-")
                    ):
                        normalized.insert(index, current.default_subcommand)
                        child_name = current.default_subcommand
                        child = next(
                            candidate
                            for candidate in current.subcommands
                            if candidate.name == child_name
                        )
                    if child is None:
                        break
                    normalized[index] = child_name
                    current = child
                    index += 1
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

    def resolve_command_path(
        self,
        command_path: Sequence[str],
    ) -> tuple[CommandSpec, tuple[str, ...]]:
        if not command_path:
            raise DevToolError("a command path is required")
        first = command_path[0].removeprefix("/").casefold()
        current = self._by_name.get(first)
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
            epilog=spec.epilog or None,
            formatter_class=argparse.RawDescriptionHelpFormatter,
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
        repository_context: RepositoryContext | None = None,
        command_io: CommandIO | None = None,
        repository_root: Path | None = None,
        stdout: TextIO | None = None,
        stderr: TextIO | None = None,
        session_state: dict[str, object] | None = None,
    ) -> int:
        if repository_context is not None:
            repository = repository_context
        else:
            try:
                repository = RepositoryContext.load(repository_root)
            except RepositoryConfigError:
                if repository_root is None:
                    raise
                repository = RepositoryContext.load().at_root(repository_root)
        io = command_io or CommandIO(stdout or sys.stdout, stderr or sys.stderr)
        if spec.required_modules:
            require_prepared_environment(
                repository,
                required_modules=spec.required_modules,
            )
        handler = spec.load_handler()
        keywords = {
            "registry": self,
            "repository_context": repository,
            "command_io": io,
            "repository_root": repository_root or repository.root,
            "stdout": io.stdout,
            "stderr": io.stderr,
        }
        if session_state is not None:
            keywords["session_state"] = session_state
        return handler(
            namespace,
            **keywords,
        )


def require_prepared_environment(
    repository_context: RepositoryContext | Path,
    *,
    required_modules: Sequence[str] = (),
) -> None:
    if isinstance(repository_context, RepositoryContext):
        root = repository_context.root
        python_environment = repository_context.config.worktrees.python_environment
    else:
        root = repository_context
        try:
            python_environment = RepositoryContext.load(root).config.worktrees.python_environment
        except RepositoryConfigError:
            python_environment = Path(".venv")
    interpreter = prepared_python_path(root, python_environment)
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
            f"'{launcher_command()}' so it selects the prepared environment."
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
