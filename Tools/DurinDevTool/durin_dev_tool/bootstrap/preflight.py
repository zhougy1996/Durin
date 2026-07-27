#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Mapping, Sequence

from ..repository import discover_repository_root

MINIMUM_PYTHON = (3, 10)
MINIMUM_CMAKE = (3, 24)
MINIMUM_MSVC_TOOLS = (14, 44)
LONG_PATHS_REGISTRY_KEY = r"SYSTEM\CurrentControlSet\Control\FileSystem"
LONG_PATHS_REGISTRY_VALUE = "LongPathsEnabled"
REPO_ROOT = discover_repository_root()


def command_path(command: str, environment: Mapping[str, str] | None = None) -> str | None:
    search_path = None if environment is None else environment.get("PATH", "")
    return shutil.which(command, path=search_path)


def find_vsdevcmd(environment: Mapping[str, str]) -> Path:
    candidates = [
        Path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
        for variable in ("ProgramFiles(x86)", "ProgramFiles")
        if (root := environment.get(variable))
    ]
    vswhere = next((path for path in candidates if path.is_file()), None)
    if vswhere is None:
        raise RuntimeError("Visual Studio Installer (vswhere.exe) was not found.")

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
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    installation = result.stdout.strip()
    if result.returncode != 0 or not installation:
        raise RuntimeError(
            "Visual Studio 2022 or newer with Desktop development with C++ was not found."
        )
    script = Path(installation) / "Common7" / "Tools" / "VsDevCmd.bat"
    if not script.is_file():
        raise RuntimeError(f'Visual Studio environment script is missing: "{script}"')
    return script


def capture_visual_studio_environment(script: Path, arguments: Sequence[str]) -> dict[str, str]:
    if not script.is_file():
        raise RuntimeError(f'Visual Studio environment script is missing: "{script}"')
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
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"Visual Studio x64 environment initialization failed ({result.returncode}).")
    environment: dict[str, str] = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            name, value = line.split("=", 1)
            normalized_name = name.upper()
            if normalized_name not in environment or name == normalized_name:
                environment[normalized_name] = value
    environment["VSLANG"] = "1033"
    return environment


def configured_cmake_command(repository_root: Path | None = None) -> str:
    repository_root = repository_root or REPO_ROOT
    if environment_command := os.environ.get("CMAKE_COMMAND"):
        return environment_command
    config_path = repository_root / ".agents" / "build-config.json"
    try:
        value = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return "cmake"
    configured = value.get("cmakeCommand") if isinstance(value, dict) else None
    return configured if isinstance(configured, str) and configured else "cmake"


def configured_visual_studio_environment(
    repository_root: Path | None = None,
) -> tuple[Path | None, list[str]]:
    repository_root = repository_root or REPO_ROOT
    config_path = repository_root / ".agents" / "build-config.json"
    try:
        value = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None, []
    setup = value.get("environmentSetup") if isinstance(value, dict) else None
    if not isinstance(setup, dict):
        return None, []
    script = setup.get("script")
    arguments = setup.get("arguments")
    configured_script = (
        Path(script).expanduser().resolve() if isinstance(script, str) and script.strip() else None
    )
    configured_arguments = (
        arguments
        if isinstance(arguments, list) and all(isinstance(argument, str) for argument in arguments)
        else []
    )
    return configured_script, configured_arguments


def check_cmake(repository_root: Path | None = None) -> str | None:
    repository_root = repository_root or REPO_ROOT
    configured = configured_cmake_command(repository_root)
    executable = command_path(configured)
    if not executable:
        return f'CMake was not found (requested command: "{configured}").'
    result = subprocess.run([executable, "--version"], capture_output=True, text=True, check=False)
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


def collect_errors(repository_root: Path | None = None) -> list[str]:
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
    if cmake_error := check_cmake(repository_root):
        errors.append(cmake_error)

    try:
        configured_script, configured_arguments = configured_visual_studio_environment(
            repository_root
        )
        script = configured_script or find_vsdevcmd(os.environ)
        arguments = configured_arguments or ["-arch=x64", "-host_arch=x64"]
        vs_environment = capture_visual_studio_environment(script, arguments)
    except RuntimeError as exc:
        errors.append(str(exc))
        return errors
    if not find_ninja(vs_environment):
        errors.append("Ninja was not found in PATH or in the selected Visual Studio installation.")
    if msvc_error := check_msvc_version(vs_environment):
        errors.append(msvc_error)
    if vulkan_error := check_vulkan_sdk(vs_environment):
        errors.append(vulkan_error)
    return errors


def validate_prerequisites(repository_root: Path) -> None:
    """Validate every prerequisite before setup mutates repository state."""
    print("Checking Durin setup prerequisites...")
    errors = collect_errors(repository_root)
    if errors:
        print(f"Setup prerequisite check found {len(errors)} problem(s):", file=sys.stderr)
        for index, error in enumerate(errors, 1):
            print(f"  {index}. {error}", file=sys.stderr)
        print("Resolve every item above, then rerun the setup or prepare command.", file=sys.stderr)
        raise RuntimeError("setup prerequisite validation failed")
    print("Setup prerequisites are ready.")
