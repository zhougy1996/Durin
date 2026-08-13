"""Dependency preparation orchestration across manifest, source, and installer owners."""

from __future__ import annotations

import os
import sys
from typing import Mapping

from ..build.config import load_local_config
from ..context import CommandIO, RepositoryContext
from . import installer, manifests
from .models import BootstrapError, DependencyRequest
from .toolchain_selection import resolve_cmake_executable, resolve_toolchain

PLATFORM_NAMES = {"win32": "Win64", "cygwin": "Win64", "darwin": "MacOS", "linux": "Linux"}


def configured_cmake_command(repository: RepositoryContext) -> str:
    if command := os.environ.get("CMAKE_COMMAND"):
        return command
    config = load_local_config(repository.resolve(repository.config.paths.local_build_config))
    return config.cmake_command or "cmake"


def detect_platform_name() -> str:
    for prefix, name in PLATFORM_NAMES.items():
        if sys.platform.startswith(prefix):
            return name
    raise BootstrapError(f"Unsupported host platform: {sys.platform}")


def prepare_dependencies(
    repository: RepositoryContext,
    request: DependencyRequest,
    *,
    command_io: CommandIO,
    environment: Mapping[str, str] | None = None,
) -> tuple[str, ...]:
    values = manifests.load_manifests(repository)
    manifests.validate_manifests(values)
    selected = manifests.select_manifests(values, request)
    platform = detect_platform_name()
    effective_environment = environment
    cmake_command = request.cmake_command
    if any(manifest["kind"] == "shared_install" for manifest in selected):
        if effective_environment is None and sys.platform == "win32":
            selection = resolve_toolchain(
                repository.root,
                cmake_command=cmake_command,
                repository_context=repository,
            )
            effective_environment = selection.environment
            cmake_command = selection.cmake_command
        elif cmake_command is None:
            cmake_command = resolve_cmake_executable(
                configured_cmake_command(repository),
                effective_environment or os.environ,
            )
    cmake_command = cmake_command or configured_cmake_command(repository)
    for manifest in selected:
        installer.prepare_manifest(
            manifest,
            platform_name=platform,
            configurations=manifests.configurations(request.config),
            cmake_command=cmake_command,
            repository=repository,
            command_io=command_io,
            environment=effective_environment,
        )
    return tuple(manifest["name"] for manifest in selected)
