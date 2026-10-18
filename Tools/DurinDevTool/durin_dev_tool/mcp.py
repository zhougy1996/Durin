"""Explicit external-client acceptance, separate from ordinary native tests."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import TextIO


def run_sdk_test(
    namespace: argparse.Namespace,
    *,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    **_kwargs: object,
) -> int:
    driver = repository_root / "Engine/Tests/Native/EngineTests/Scripts/MCPClientSmoke.py"
    command = [sys.executable, "-I", str(driver)]
    for option in ("profile", "preset", "color"):
        value = getattr(namespace, option, None)
        if value:
            command.extend((f"--{option}", str(value)))
    return subprocess.run(
        command, cwd=repository_root, stdout=stdout, stderr=stderr, check=False,
    ).returncode
