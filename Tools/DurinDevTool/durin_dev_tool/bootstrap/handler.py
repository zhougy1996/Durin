"""Unified setup and dependency command handlers."""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, TextIO

from ..context import CommandIO, RepositoryContext
from ..errors import DevToolError
from ..python_environment import restart_prepared_shell
from .models import BootstrapError, DependencyRequest
from . import application
from .preflight import PreflightError


def _enable_virtual_terminal(stream: TextIO) -> bool:
    if os.name != "nt":
        return True
    try:
        import ctypes
        import msvcrt

        handle = msvcrt.get_osfhandle(stream.fileno())
        mode = ctypes.c_ulong()
        kernel32 = ctypes.windll.kernel32
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))
    except (AttributeError, OSError, ValueError):
        return False


class _BootstrapOutput:
    """Add lightweight styling before the prepared environment can provide Rich."""

    def __init__(self, stream: TextIO, *, plain: bool) -> None:
        self.stream = stream
        self.styled = (
            not plain
            and "NO_COLOR" not in os.environ
            and bool(getattr(stream, "isatty", lambda: False)())
            and _enable_virtual_terminal(stream)
        )

    def write(self, text: str) -> int:
        if not self.styled or not text.strip():
            return self.stream.write(text)
        lowered = text.casefold()
        if text.startswith("[run]"):
            style = "2;36"
        elif text.startswith("==>"):
            style = "1;36"
        elif any(word in lowered for word in ("successfully", " is ready", " are ready", "validated")):
            style = "1;32"
        elif any(word in lowered for word in ("warning", "skipping", "repairing")):
            style = "1;33"
        else:
            style = "36"
        return self.stream.write(f"\x1b[{style}m{text}\x1b[0m")

    def flush(self) -> None:
        self.stream.flush()

    def fileno(self) -> int:
        return self.stream.fileno()

    def isatty(self) -> bool:
        return bool(getattr(self.stream, "isatty", lambda: False)())


@dataclass(frozen=True)
class _BootstrapCommand:
    namespace: argparse.Namespace
    repository: RepositoryContext
    command_io: CommandIO
    session_state: dict[str, object] | None
    stdout: TextIO


def _run_setup(command: _BootstrapCommand) -> None:
    interactive = (
        not getattr(command.namespace, "non_interactive", False)
        and bool(getattr(sys.stdin, "isatty", lambda: False)())
        and bool(getattr(command.stdout, "isatty", lambda: False)())
    )
    python = application.setup_checkout(command.repository, command.command_io, interactive=interactive)
    if command.session_state is not None:
        restart_prepared_shell(
            command.repository.root,
            python,
            command.session_state,
            command.command_io,
        )


def _run_dependency_validate(command: _BootstrapCommand) -> None:
    application.validate_dependencies(command.repository, command.command_io)


def _run_dependency_prepare(command: _BootstrapCommand) -> None:
    namespace = command.namespace
    application.prepare_dependency_plan(
        command.repository,
        DependencyRequest(
            use_all=namespace.all_dependencies,
            libraries=namespace.libraries,
            config=namespace.dependency_config,
            with_tests=namespace.with_tests,
            with_development=namespace.with_development,
            cmake_command=namespace.dependency_cmake,
        ),
        command_io=command.command_io,
    )


_ACTIONS: dict[str, Callable[[_BootstrapCommand], None]] = {
    "setup": _run_setup,
    "dependency-validate": _run_dependency_validate,
    "dependency-prepare": _run_dependency_prepare,
}


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    session_state: dict[str, object] | None = None,
    repository_context: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
    **_: object,
) -> int:
    repository = repository_context or RepositoryContext.load(repository_root)
    styled_stdout = _BootstrapOutput(stdout, plain=getattr(namespace, "plain", False))
    styled_stderr = _BootstrapOutput(stderr, plain=getattr(namespace, "plain", False))
    io = command_io or CommandIO(styled_stdout, styled_stderr, plain=getattr(namespace, "plain", False))
    if command_io is not None:
        io = CommandIO(
            _BootstrapOutput(command_io.stdout, plain=command_io.plain),
            _BootstrapOutput(command_io.stderr, plain=command_io.plain),
            plain=command_io.plain,
        )
    try:
        action = _ACTIONS.get(namespace.bootstrap_action)
        if action is None:
            raise DevToolError("a bootstrap command is required")
        action(_BootstrapCommand(namespace, repository, io, session_state, stdout))
        return 0
    except (BootstrapError, PreflightError):
        raise
