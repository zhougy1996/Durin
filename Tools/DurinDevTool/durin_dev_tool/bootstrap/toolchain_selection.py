"""Bootstrap toolchain selection built from shared discovery primitives."""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence

from ..build.config import load_local_config
from ..context import RepositoryContext
from ..toolchain import capture_setup_environment, capture_windows_environment, find_command, find_vsdevcmd
from .preflight import PreflightError, check_cmake, check_msvc_version, find_ninja

DEFAULT_ENVIRONMENT_ARGUMENTS = ("-arch=x64", "-host_arch=x64")


@dataclass(frozen=True)
class ToolchainSelection:
    cmake_command: str
    environment_script: Path | None
    environment_arguments: tuple[str, ...]
    environment: dict[str, str]


def _repository(
    repository_root: Path | None,
    repository_context: RepositoryContext | None,
) -> RepositoryContext:
    if repository_context is not None:
        return repository_context
    current = RepositoryContext.load()
    return current.at_root(repository_root) if repository_root is not None else current


def configured_cmake_command(
    repository_root: Path | None = None,
    *,
    repository_context: RepositoryContext | None = None,
) -> str:
    repository = _repository(repository_root, repository_context)
    if environment_command := os.environ.get("CMAKE_COMMAND"):
        return environment_command
    config_path = repository.root / repository.config.paths.local_build_config
    config = load_local_config(config_path)
    return config.cmake_command or "cmake"


def configured_visual_studio_environment(
    repository_root: Path | None = None,
    *,
    repository_context: RepositoryContext | None = None,
) -> tuple[Path | None, list[str]]:
    repository = _repository(repository_root, repository_context)
    config_path = repository.root / repository.config.paths.local_build_config
    config = load_local_config(config_path)
    script = config.environment_setup.script
    configured_script = Path(script).expanduser().resolve() if script else None
    return configured_script, list(config.environment_setup.arguments)


def resolve_cmake_executable(command: str, environment: Mapping[str, str]) -> str:
    executable = find_command(command, environment)
    if not executable:
        raise PreflightError(f'CMake was not found (requested command: "{command}").')
    return str(Path(executable).resolve())


def find_macos_vulkan_environment_script(
    environment: Mapping[str, str],
) -> Path | None:
    sdk_value = environment.get("VULKAN_SDK", "").strip()
    if not sdk_value:
        return None
    sdk = Path(sdk_value).expanduser().resolve()
    candidate = sdk.parent / "setup-env.sh" if sdk.name == "macOS" else sdk / "setup-env.sh"
    return candidate if candidate.is_file() else None


def select_toolchain(
    repository_root: Path | None = None,
    *,
    cmake_command: str | None = None,
    environment_script: Path | None = None,
    environment_arguments: Sequence[str] | None = None,
    repository_context: RepositoryContext | None = None,
) -> ToolchainSelection:
    repository = _repository(repository_root, repository_context)
    configured_script, configured_arguments = configured_visual_studio_environment(
        repository.root,
        repository_context=repository,
    )
    script = environment_script or configured_script
    if sys.platform == "win32":
        script = script or find_vsdevcmd(os.environ)
        arguments = tuple(
            environment_arguments
            if environment_arguments is not None
            else configured_arguments or DEFAULT_ENVIRONMENT_ARGUMENTS
        )
        environment = capture_windows_environment(script, arguments)
        environment["VSLANG"] = "1033"
    elif sys.platform == "darwin":
        script = script or find_macos_vulkan_environment_script(os.environ)
        arguments = tuple(
            environment_arguments
            if environment_arguments is not None
            else configured_arguments
        )
        environment = (
            capture_setup_environment(
                script,
                arguments,
                current_host="macos",
                cwd=repository.root,
            )
            if script is not None
            else dict(os.environ)
        )
    else:
        raise PreflightError(
            f"DevTool setup does not support host platform {sys.platform!r}."
        )
    configured_cmake = cmake_command or configured_cmake_command(
        repository.root,
        repository_context=repository,
    )
    executable = resolve_cmake_executable(configured_cmake, environment)
    return ToolchainSelection(
        cmake_command=executable,
        environment_script=script,
        environment_arguments=arguments,
        environment=environment,
    )


def resolve_toolchain(
    repository_root: Path | None = None,
    *,
    cmake_command: str | None = None,
    environment_script: Path | None = None,
    environment_arguments: Sequence[str] | None = None,
    repository_context: RepositoryContext | None = None,
) -> ToolchainSelection:
    repository = _repository(repository_root, repository_context)
    selection = select_toolchain(
        repository.root,
        cmake_command=cmake_command,
        environment_script=environment_script,
        environment_arguments=environment_arguments,
        repository_context=repository,
    )
    if cmake_error := check_cmake(
        repository.root,
        environment=selection.environment,
        command=selection.cmake_command,
        repository_context=repository,
    ):
        raise PreflightError(cmake_error)
    if not find_ninja(selection.environment):
        message = "Ninja was not found in PATH"
        if sys.platform == "win32":
            message += " or in the selected Visual Studio installation"
        raise PreflightError(message + ".")
    if sys.platform == "win32":
        if msvc_error := check_msvc_version(selection.environment):
            raise PreflightError(msvc_error)
    return selection
