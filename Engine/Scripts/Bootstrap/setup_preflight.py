#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Mapping


MINIMUM_PYTHON = (3, 10)
MINIMUM_CMAKE = (3, 24)
MINIMUM_MSVC_TOOLS = (14, 44)
REPO_ROOT = Path(__file__).resolve().parents[3]


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


def capture_visual_studio_environment(script: Path) -> dict[str, str]:
    comspec = os.environ.get("COMSPEC", "cmd.exe")
    command = f'"{comspec}" /d /s /c ""{script}" -arch=x64 -host_arch=x64 >nul && set"'
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


def configured_cmake_command() -> str:
    if environment_command := os.environ.get("CMAKE_COMMAND"):
        return environment_command
    config_path = REPO_ROOT / ".agents" / "build-config.json"
    try:
        value = json.loads(config_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return "cmake"
    configured = value.get("cmakeCommand") if isinstance(value, dict) else None
    return configured if isinstance(configured, str) and configured else "cmake"


def check_cmake() -> str | None:
    configured = configured_cmake_command()
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


def collect_errors() -> list[str]:
    errors: list[str] = []
    if sys.version_info < MINIMUM_PYTHON:
        errors.append(
            f"Python {sys.version_info.major}.{sys.version_info.minor} is installed; "
            "Durin requires Python 3.10 or newer."
        )
    if sys.platform != "win32":
        errors.append(f"Setup.bat supports Windows hosts only (detected {sys.platform}).")
        return errors
    if not command_path("git"):
        errors.append("Git was not found in PATH.")
    if cmake_error := check_cmake():
        errors.append(cmake_error)

    try:
        vs_environment = capture_visual_studio_environment(find_vsdevcmd(os.environ))
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


def main() -> int:
    print("Checking Durin setup prerequisites...")
    errors = collect_errors()
    if errors:
        print(f"Setup prerequisite check found {len(errors)} problem(s):", file=sys.stderr)
        for index, error in enumerate(errors, 1):
            print(f"  {index}. {error}", file=sys.stderr)
        print("Resolve every item above, then run Setup.bat again.", file=sys.stderr)
        return 1
    print("Setup prerequisites are ready.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
