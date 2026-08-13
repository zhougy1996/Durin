"""Shared toolchain environment discovery primitives."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
from pathlib import Path
from typing import Mapping, Sequence

from .errors import DevToolError


class ToolchainError(DevToolError):
    """A toolchain environment could not be discovered or captured."""


def environment_value(environment: Mapping[str, str], name: str) -> tuple[str, str]:
    """Return an environment value while honoring Windows key casing."""
    for existing_name, value in environment.items():
        if existing_name.casefold() == name.casefold():
            return existing_name, value
    return name, ""


def environment_path(environment: Mapping[str, str]) -> str:
    return environment_value(environment, "PATH")[1]


def find_command(command: str, environment: Mapping[str, str] | None = None) -> str | None:
    search_path = None if environment is None else environment_path(environment)
    return shutil.which(command, path=search_path)


def _program_files_roots(environment: Mapping[str, str]) -> tuple[Path, ...]:
    roots: list[Path] = []
    for variable in ("ProgramFiles(x86)", "ProgramW6432", "ProgramFiles"):
        _, value = environment_value(environment, variable)
        if value.strip():
            roots.append(Path(value))

    if os.name == "nt":
        try:
            import winreg
        except ImportError:
            winreg = None
        if winreg is not None:
            registry_path = r"SOFTWARE\Microsoft\Windows\CurrentVersion"
            access = getattr(winreg, "KEY_READ", 0)
            views = {
                0,
                getattr(winreg, "KEY_WOW64_64KEY", 0),
                getattr(winreg, "KEY_WOW64_32KEY", 0),
            }
            for view in views:
                try:
                    with winreg.OpenKey(
                        winreg.HKEY_LOCAL_MACHINE,
                        registry_path,
                        0,
                        access | view,
                    ) as key:
                        for value_name in ("ProgramFilesDir (x86)", "ProgramFilesDir"):
                            try:
                                value, _ = winreg.QueryValueEx(key, value_name)
                            except OSError:
                                continue
                            if isinstance(value, str) and value.strip():
                                roots.append(Path(value))
                except OSError:
                    continue

    unique: list[Path] = []
    seen: set[str] = set()
    for root in roots:
        key = str(root).casefold()
        if key not in seen:
            seen.add(key)
            unique.append(root)
    return tuple(unique)


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
        root / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        for root in _program_files_roots(environment)
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


def capture_setup_environment(
    script: Path,
    arguments: Sequence[str],
    *,
    current_host: str,
    cwd: Path,
) -> dict[str, str]:
    if not script.is_file():
        raise ToolchainError(f'Environment setup script does not exist: "{script}"')
    if current_host == "windows":
        if script.suffix.lower() not in {".bat", ".cmd"}:
            raise ToolchainError("Windows environment setup scripts must use the .bat or .cmd extension.")
        return capture_windows_environment(script, arguments, cwd=cwd)
    argument_text = " ".join(shlex.quote(item) for item in [str(script), *arguments])
    command = ["/bin/sh", "-c", f". {argument_text} >/dev/null && env -0"]
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)
    except OSError as exc:
        raise ToolchainError(f'Could not initialize the environment with "{script}": {exc}') from exc
    if result.returncode != 0:
        details = result.stderr.strip()
        raise ToolchainError(
            f'Environment setup script failed with exit code {result.returncode}: "{script}"'
            + (f"\n{details}" if details else "")
        )
    return parse_environment_output(result.stdout, case_insensitive=current_host == "windows")
