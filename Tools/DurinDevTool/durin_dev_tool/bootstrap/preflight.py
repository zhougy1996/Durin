#!/usr/bin/env python3
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

from ..build.config import BuildToolError, load_local_config
from ..configuration import load_repository_config
from ..repository import discover_repository_root
from ..toolchain import (
    ToolchainError,
    capture_windows_environment,
    environment_path,
    find_vsdevcmd as find_shared_vsdevcmd,
)

MINIMUM_PYTHON = (3, 10)
MINIMUM_CMAKE = (3, 24)
MINIMUM_MSVC_TOOLS = (14, 44)
LONG_PATHS_REGISTRY_KEY = r"SYSTEM\CurrentControlSet\Control\FileSystem"
LONG_PATHS_REGISTRY_VALUE = "LongPathsEnabled"
REPO_ROOT = discover_repository_root()
REPOSITORY_CONFIG = load_repository_config(REPO_ROOT)


class PreflightError(RuntimeError):
    pass


DEFAULT_ENVIRONMENT_ARGUMENTS = ("-arch=x64", "-host_arch=x64")


@dataclass(frozen=True)
class ToolchainSelection:
    cmake_command: str
    environment_script: Path
    environment_arguments: tuple[str, ...]
    environment: dict[str, str]


def command_path(command: str, environment: Mapping[str, str] | None = None) -> str | None:
    search_path = None if environment is None else environment_path(environment)
    return shutil.which(command, path=search_path)


def find_vsdevcmd(environment: Mapping[str, str]) -> Path:
    try:
        return find_shared_vsdevcmd(environment)
    except ToolchainError as exc:
        raise PreflightError(str(exc)) from exc


def capture_visual_studio_environment(script: Path, arguments: Sequence[str]) -> dict[str, str]:
    try:
        environment = capture_windows_environment(script, arguments)
        environment["VSLANG"] = "1033"
        return environment
    except ToolchainError as exc:
        raise PreflightError(str(exc)) from exc


def configured_cmake_command(repository_root: Path | None = None) -> str:
    repository_root = repository_root or REPO_ROOT
    if environment_command := os.environ.get("CMAKE_COMMAND"):
        return environment_command
    config_path = repository_root / REPOSITORY_CONFIG.paths.local_build_config
    try:
        config = load_local_config(config_path)
    except BuildToolError as exc:
        raise PreflightError(str(exc)) from exc
    return config.cmake_command or "cmake"


def configured_visual_studio_environment(
    repository_root: Path | None = None,
) -> tuple[Path | None, list[str]]:
    repository_root = repository_root or REPO_ROOT
    config_path = repository_root / REPOSITORY_CONFIG.paths.local_build_config
    try:
        config = load_local_config(config_path)
    except BuildToolError as exc:
        raise PreflightError(str(exc)) from exc
    script = config.environment_setup.script
    configured_script = (
        Path(script).expanduser().resolve()
        if script
        else None
    )
    return configured_script, list(config.environment_setup.arguments)


def resolve_cmake_executable(
    command: str,
    environment: Mapping[str, str],
) -> str:
    executable = command_path(command, environment)
    if not executable:
        raise PreflightError(f'CMake was not found (requested command: "{command}").')
    return str(Path(executable).resolve())


def resolve_toolchain(
    repository_root: Path | None = None,
    *,
    cmake_command: str | None = None,
    environment_script: Path | None = None,
    environment_arguments: Sequence[str] | None = None,
) -> ToolchainSelection:
    repository_root = repository_root or REPO_ROOT
    configured_script, configured_arguments = configured_visual_studio_environment(
        repository_root
    )
    script = environment_script or configured_script
    if script is None:
        script = find_vsdevcmd(os.environ)
    arguments = tuple(
        environment_arguments
        if environment_arguments is not None
        else configured_arguments or DEFAULT_ENVIRONMENT_ARGUMENTS
    )
    environment = capture_visual_studio_environment(script, arguments)
    configured_cmake = cmake_command or configured_cmake_command(repository_root)
    executable = resolve_cmake_executable(configured_cmake, environment)
    if cmake_error := check_cmake(
        repository_root,
        environment=environment,
        command=executable,
    ):
        raise PreflightError(cmake_error)
    if not find_ninja(environment):
        raise PreflightError(
            "Ninja was not found in PATH or in the selected Visual Studio installation."
        )
    if msvc_error := check_msvc_version(environment):
        raise PreflightError(msvc_error)
    return ToolchainSelection(
        cmake_command=executable,
        environment_script=script,
        environment_arguments=arguments,
        environment=environment,
    )


def check_cmake(
    repository_root: Path | None = None,
    *,
    environment: Mapping[str, str] | None = None,
    command: str | None = None,
) -> str | None:
    repository_root = repository_root or REPO_ROOT
    configured = command or configured_cmake_command(repository_root)
    executable = command_path(configured, environment)
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
    ninja = command_path("ninja", environment)
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
    if not command_path("cl.exe", environment):
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
) -> list[str]:
    repository_root = repository_root or REPO_ROOT
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
    if not command_path("git"):
        errors.append("Git was not found in PATH.")
    if long_paths_error := check_windows_long_paths():
        errors.append(long_paths_error)
    vs_environment: Mapping[str, str] = os.environ
    environment_setup_failed = False
    if selection is None:
        try:
            configured_script, configured_arguments = configured_visual_studio_environment(
                repository_root
            )
            script = configured_script or find_vsdevcmd(os.environ)
            arguments = configured_arguments or DEFAULT_ENVIRONMENT_ARGUMENTS
            vs_environment = capture_visual_studio_environment(script, arguments)
        except PreflightError as exc:
            errors.append(str(exc))
            environment_setup_failed = True
    else:
        vs_environment = selection.environment
    if cmake_error := check_cmake(
        repository_root,
        environment=vs_environment,
        command=selection.cmake_command if selection is not None else None,
    ):
        errors.append(cmake_error)
    if environment_setup_failed:
        return errors
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
) -> None:
    """Validate every prerequisite before setup mutates repository state."""
    print("Checking Durin setup prerequisites...")
    errors = collect_errors(repository_root, selection=selection)
    if errors:
        print(f"Setup prerequisite check found {len(errors)} problem(s):", file=sys.stderr)
        for index, error in enumerate(errors, 1):
            print(f"  {index}. {error}", file=sys.stderr)
        print("Resolve every item above, then rerun the setup or prepare command.", file=sys.stderr)
        raise PreflightError("setup prerequisite validation failed")
    print("Setup prerequisites are ready.")
