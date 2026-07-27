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
    handler: str
    capability: Capability = Capability.BOOTSTRAP
    arguments: tuple[ArgumentSpec, ...] = ()
    required_modules: tuple[str, ...] = ()

    def load_handler(self) -> Callable[..., int]:
        module_name, separator, attribute = self.handler.partition(":")
        if not separator:
            raise DevToolError(f'Invalid handler registration for "{self.name}".')
        module = importlib.import_module(module_name)
        return getattr(module, attribute)


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
        "build",
        "build a CMake target",
        "durin_dev_tool.commands.placeholder:run",
        capability=Capability.PREPARED_ENVIRONMENT,
        arguments=(
            ArgumentSpec(("--target",), {"default": "all", "help": "CMake target (default: all)"}),
            ArgumentSpec(("--plain",), {"action": "store_true", "help": "disable styled output"}),
        ),
        required_modules=("rich",),
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
            for argument in spec.arguments:
                command_parser.add_argument(*argument.flags, **argument.kwargs)
        return parser

    def parse(self, arguments: Sequence[str]) -> tuple[CommandSpec, argparse.Namespace]:
        if not arguments:
            normalized = ["shell"]
        elif arguments[0] in {"-h", "--help", "/?"}:
            normalized = ["help"]
        else:
            normalized = list(arguments)
        namespace = self.parser().parse_args(normalized)
        return self._by_name[namespace.command], namespace

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
    ) -> int:
        if spec.capability is Capability.PREPARED_ENVIRONMENT:
            require_prepared_environment(
                repository_root,
                required_modules=spec.required_modules,
            )
        handler = spec.load_handler()
        return handler(
            namespace,
            registry=self,
            repository_root=repository_root,
            stdout=stdout,
            stderr=stderr,
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
            "'Tools\\DurinDevTool\\DevTool.bat' so it selects the prepared environment."
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
