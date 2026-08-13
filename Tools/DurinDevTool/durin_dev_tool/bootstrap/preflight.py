#!/usr/bin/env python3
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Mapping, Protocol

from ..context import CommandIO, RepositoryContext
from ..errors import DevToolError
from ..toolchain import find_command

MINIMUM_PYTHON = (3, 10)
MINIMUM_CMAKE = (3, 24)
MINIMUM_MSVC_TOOLS = (14, 44)
LONG_PATHS_REGISTRY_KEY = r"SYSTEM\CurrentControlSet\Control\FileSystem"
LONG_PATHS_REGISTRY_VALUE = "LongPathsEnabled"


class PreflightError(DevToolError):
    pass


def _repository(
    repository_root: Path | None,
    repository_context: RepositoryContext | None,
) -> RepositoryContext:
    if repository_context is not None:
        return repository_context
    current = RepositoryContext.load()
    return current.at_root(repository_root) if repository_root is not None else current


class ToolchainSelection(Protocol):
    cmake_command: str
    environment: dict[str, str]


def check_cmake(
    repository_root: Path | None = None,
    *,
    environment: Mapping[str, str] | None = None,
    command: str | None = None,
    repository_context: RepositoryContext | None = None,
) -> str | None:
    repository = _repository(repository_root, repository_context)
    if command is None:
        from .toolchain_selection import configured_cmake_command

        command = configured_cmake_command(
            repository.root,
            repository_context=repository,
        )
    configured = command
    executable = find_command(configured, environment)
    if not executable:
        return f'CMake was not found (requested command: "{configured}").'
    try:
        result = subprocess.run(
            [executable, "--version"],
            capture_output=True,
            text=True,
            check=False,
            env=dict(environment) if environment is not None else None,
        )
    except OSError as exc:
        return f'CMake could not be launched from "{executable}": {exc}'
    match = re.search(r"cmake version (\d+)\.(\d+)", result.stdout)
    if result.returncode != 0 or not match:
        return f'CMake version could not be determined from "{executable}".'
    version = tuple(int(part) for part in match.groups())
    if version < MINIMUM_CMAKE:
        return f"CMake {version[0]}.{version[1]} is installed; Durin requires 3.24 or newer."
    return None


def find_ninja(environment: Mapping[str, str]) -> str | None:
    ninja = find_command("ninja", environment)
    if ninja:
        return ninja
    visual_studio_root = environment.get("VSINSTALLDIR", "")
    bundled = (
        Path(visual_studio_root)
        / "Common7"
        / "IDE"
        / "CommonExtensions"
        / "Microsoft"
        / "CMake"
        / "Ninja"
        / "ninja.exe"
    )
    return str(bundled) if visual_studio_root and bundled.is_file() else None


def check_msvc_version(environment: Mapping[str, str]) -> str | None:
    if not find_command("cl.exe", environment):
        return "MSVC cl.exe was not found after Visual Studio environment initialization."
    version_text = environment.get("VCTOOLSVERSION", "")
    match = re.match(r"(\d+)\.(\d+)", version_text)
    if not match:
        return "The selected MSVC toolset version could not be determined."
    version = tuple(int(part) for part in match.groups())
    if version < MINIMUM_MSVC_TOOLS:
        return (
            f"MSVC Build Tools {version_text} is installed; Durin requires 14.44 or newer "
            "(Visual Studio 2022 17.14) for its C++20 standard library, including std::format_string."
        )
    return None


def check_vulkan_sdk(environment: Mapping[str, str]) -> str | None:
    sdk_value = environment.get("VULKAN_SDK", os.environ.get("VULKAN_SDK", ""))
    if not sdk_value:
        return "VULKAN_SDK is not set. Install the LunarG Vulkan SDK and reopen the terminal."
    sdk = Path(sdk_value)
    required = [
        sdk / "Include" / "vulkan" / "vulkan.h",
        sdk / "Include" / "vma" / "vk_mem_alloc.h",
        sdk / "Lib" / "vulkan-1.lib",
    ]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        return (
            "The Vulkan SDK is incomplete; missing: "
            + ", ".join(missing)
            + ". Update the Vulkan SDK, or install VulkanMemoryAllocator's vk_mem_alloc.h "
            "under the SDK Include/vma directory."
        )
    return None


def read_windows_long_paths_enabled() -> bool:
    import winreg

    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, LONG_PATHS_REGISTRY_KEY) as key:
        value, value_type = winreg.QueryValueEx(key, LONG_PATHS_REGISTRY_VALUE)
    return value_type == winreg.REG_DWORD and value == 1


def check_windows_long_paths() -> str | None:
    try:
        enabled = read_windows_long_paths_enabled()
    except OSError:
        enabled = False
    if enabled:
        return None
    return (
        r"Windows long-path support is required. Set "
        r"HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled (REG_DWORD) to 1 "
        r"through Computer Configuration > Administrative Templates > System > Filesystem > "
        r"Enable Win32 long paths, then restart Windows. Durin never changes machine policy automatically."
    )


def collect_errors(
    repository_root: Path | None = None,
    *,
    selection: ToolchainSelection | None = None,
    repository_context: RepositoryContext | None = None,
) -> list[str]:
    repository = _repository(repository_root, repository_context)
    errors: list[str] = []
    if sys.version_info < MINIMUM_PYTHON:
        errors.append(
            f"Python {sys.version_info.major}.{sys.version_info.minor} is installed; "
            "Durin requires Python 3.10 or newer."
        )
    if sys.platform != "win32":
        errors.append(
            f"DevTool setup supports Windows hosts only (detected {sys.platform})."
        )
        return errors
    if not find_command("git"):
        errors.append("Git was not found in PATH.")
    if long_paths_error := check_windows_long_paths():
        errors.append(long_paths_error)
    selected = selection
    if selection is None:
        try:
            from .toolchain_selection import select_toolchain

            selected = select_toolchain(
                repository.root,
                repository_context=repository,
            )
        except DevToolError as exc:
            errors.append(str(exc))
            return errors
    assert selected is not None
    vs_environment: Mapping[str, str] = selected.environment
    if cmake_error := check_cmake(
        repository.root,
        environment=vs_environment,
        command=selected.cmake_command,
        repository_context=repository,
    ):
        errors.append(cmake_error)
    if not find_ninja(vs_environment):
        errors.append("Ninja was not found in PATH or in the selected Visual Studio installation.")
    if msvc_error := check_msvc_version(vs_environment):
        errors.append(msvc_error)
    if vulkan_error := check_vulkan_sdk(vs_environment):
        errors.append(vulkan_error)
    return errors


def validate_prerequisites(
    repository_root: Path,
    *,
    selection: ToolchainSelection | None = None,
    repository_context: RepositoryContext | None = None,
    command_io: CommandIO | None = None,
) -> None:
    """Validate every prerequisite before setup mutates repository state."""
    repository = _repository(repository_root, repository_context)
    io = command_io or CommandIO.system()
    io.out("Checking Durin setup prerequisites...")
    errors = collect_errors(
        repository.root,
        selection=selection,
        repository_context=repository,
    )
    if errors:
        io.error(f"Setup prerequisite check found {len(errors)} problem(s):")
        for index, error in enumerate(errors, 1):
            io.error(f"  {index}. {error}")
        io.error("Resolve every item above, then rerun the setup or prepare command.")
        raise PreflightError("setup prerequisite validation failed")
    io.out("Setup prerequisites are ready.")
