"""Unified setup and dependency command handlers."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import TextIO

from ..errors import DevToolError
from .dependencies import (
    BootstrapError,
    DependencyRequest,
    prepare_dependencies,
    validate_repository_manifests,
)
from .setup import setup_repository


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

    def isatty(self) -> bool:
        return bool(getattr(self.stream, "isatty", lambda: False)())


def _restart_prepared_shell(
    repository_root: Path,
    python: Path,
    session_state: dict[str, object],
) -> None:
    try:
        same_interpreter = python.samefile(Path(sys.executable))
    except OSError:
        same_interpreter = python.resolve() == Path(sys.executable).resolve()
    if same_interpreter:
        return
    entrypoint = (
        repository_root
        / "Tools"
        / "DurinDevTool"
        / "durin_dev_tool"
        / "__main__.py"
    )
    print("Restarting the interactive shell in Durin's prepared environment...")
    result = subprocess.run(
        [str(python), str(entrypoint), "shell"],
        cwd=repository_root,
        check=False,
    )
    if result.returncode != 0:
        raise BootstrapError(
            f"Prepared shell exited with code {result.returncode}."
        )
    session_state["exit_requested"] = True


def run(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    session_state: dict[str, object] | None = None,
    **_: object,
) -> int:
    styled_stdout = _BootstrapOutput(stdout, plain=getattr(namespace, "plain", False))
    styled_stderr = _BootstrapOutput(stderr, plain=getattr(namespace, "plain", False))
    try:
        with redirect_stdout(styled_stdout), redirect_stderr(styled_stderr):
            if namespace.bootstrap_action == "setup":
                python = setup_repository(repository_root)
                if session_state is not None:
                    _restart_prepared_shell(
                        repository_root,
                        python,
                        session_state,
                    )
                return 0
            if namespace.bootstrap_action == "dependency-validate":
                validate_repository_manifests(repository_root)
                return 0
            if namespace.bootstrap_action == "dependency-prepare":
                prepare_dependencies(
                    repository_root,
                    DependencyRequest(
                        use_all=namespace.all_dependencies,
                        libraries=namespace.libraries,
                        config=namespace.dependency_config,
                        with_tests=namespace.with_tests,
                        with_development=namespace.with_development,
                        cmake_command=namespace.dependency_cmake,
                    ),
                )
                return 0
    except (BootstrapError, RuntimeError, OSError, ValueError) as exc:
        raise DevToolError(str(exc)) from exc
    raise DevToolError("a bootstrap command is required")
