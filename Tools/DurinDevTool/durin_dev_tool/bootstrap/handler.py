"""Unified setup and dependency command handlers."""

from __future__ import annotations

import argparse
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
    try:
        with redirect_stdout(stdout), redirect_stderr(stderr):
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
