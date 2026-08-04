"""Shared toolchain environment discovery primitives."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Mapping, Sequence


class ToolchainError(RuntimeError):
    """A toolchain environment could not be discovered or captured."""


def environment_value(environment: Mapping[str, str], name: str) -> tuple[str, str]:
    """Return an environment value while honoring Windows key casing."""
    for existing_name, value in environment.items():
        if existing_name.casefold() == name.casefold():
            return existing_name, value
    return name, ""


def environment_path(environment: Mapping[str, str]) -> str:
    return environment_value(environment, "PATH")[1]


def parse_environment_output(
    output: str,
    *,
    case_insensitive: bool = False,
) -> dict[str, str]:
    environment: dict[str, str] = {}
    entries = output.replace("\r\n", "\n").split("\0" if "\0" in output else "\n")
    for entry in entries:
        if "=" not in entry:
            continue
        name, value = entry.split("=", 1)
        if name:
            environment[name] = value
    if not case_insensitive:
        return environment
    normalized: dict[str, str] = {}
    for name, value in environment.items():
        normalized_name = name.upper()
        if normalized_name not in normalized or name == normalized_name:
            normalized[normalized_name] = value
    return normalized


def find_vsdevcmd(environment: Mapping[str, str]) -> Path:
    candidates = [
        Path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        for variable in ("ProgramFiles(x86)", "ProgramFiles")
        if (root := environment.get(variable))
    ]
    vswhere = next((path for path in candidates if path.is_file()), None)
    if vswhere is None:
        raise ToolchainError("Visual Studio Installer (vswhere.exe) was not found.")

    command = [
        str(vswhere),
        "-latest",
        "-products",
        "*",
        "-version",
        "[17.0,)",
        "-requires",
        "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property",
        "installationPath",
    ]
    try:
        result = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as exc:
        raise ToolchainError(
            f'Could not run Visual Studio Installer query "{vswhere}": {exc}'
        ) from exc
    installation = result.stdout.strip()
    if result.returncode != 0 or not installation:
        raise ToolchainError(
            "Visual Studio 2022 or newer with Desktop development with C++ was not found."
        )
    script = Path(installation) / "Common7" / "Tools" / "VsDevCmd.bat"
    if not script.is_file():
        raise ToolchainError(f'Visual Studio environment script is missing: "{script}"')
    return script


def capture_windows_environment(
    script: Path,
    arguments: Sequence[str],
    *,
    cwd: Path | None = None,
) -> dict[str, str]:
    if not script.is_file():
        raise ToolchainError(f'Visual Studio environment script is missing: "{script}"')
    comspec = os.environ.get("COMSPEC", "cmd.exe")
    command = [
        comspec,
        "/d",
        "/s",
        "/c",
        "call",
        str(script),
        *arguments,
        ">nul",
        "&&",
        "set",
    ]
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as exc:
        raise ToolchainError(
            f'Could not initialize the Visual Studio environment with "{script}": {exc}'
        ) from exc
    if result.returncode != 0:
        raise ToolchainError(
            f"Visual Studio x64 environment initialization failed ({result.returncode})."
        )
    return parse_environment_output(result.stdout, case_insensitive=True)
