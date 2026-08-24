"""Preset-aware third-party preparation for CMake configuration."""

from __future__ import annotations

from typing import Iterable

from ..bootstrap.dependency_service import prepare_dependencies
from ..bootstrap.models import BootstrapError, DependencyRequest
from ..context import CommandIO, RepositoryContext
from .config import BuildContext, BuildToolError, preset_cache_bool, preset_cache_string
from .output import BuildOutput


def _definition_values(definitions: Iterable[str]) -> dict[str, str]:
    values: dict[str, str] = {}
    for definition in definitions:
        key, separator, value = definition.partition("=")
        if separator:
            values[key.partition(":")[0]] = value
    return values


def _effective_string(context: BuildContext, name: str) -> str:
    overrides = _definition_values(getattr(context.request, "defines", ()))
    if name in overrides:
        return overrides[name]
    return preset_cache_string(context.preset, name, required=False)


def _effective_bool(context: BuildContext, name: str) -> bool:
    overrides = _definition_values(getattr(context.request, "defines", ()))
    if name not in overrides:
        return preset_cache_bool(context.preset, name)
    return overrides[name].strip().casefold() in {"1", "on", "true", "yes", "y"}


def dependency_configuration(context: BuildContext) -> str:
    configuration = _effective_string(context, "CMAKE_BUILD_TYPE")
    if configuration == "Shipping":
        return "Release"
    if configuration not in {"Debug", "Release"}:
        raise BuildToolError(
            f'Preset "{context.preset.name}" uses unsupported third-party configuration '
            f'"{configuration or "<empty>"}".'
        )
    return configuration


def prepare_configure_dependencies(context: BuildContext, output: BuildOutput) -> None:
    """Prepare only the dependencies required by the effective configure options."""
    repository = context.repository or RepositoryContext.load()
    command_io = CommandIO(
        stdout=output.console.file,
        stderr=output.error_console.file,
        plain=output.plain,
    )
    configuration = dependency_configuration(context)
    try:
        prepare_dependencies(
            repository,
            DependencyRequest(
                use_all=True,
                config=configuration,
                with_tests=_effective_bool(context, "BUILD_TESTING"),
                cmake_command=context.cmake,
            ),
            command_io=command_io,
            environment=context.environment,
        )
        if _effective_bool(context, "DURIN_ENABLE_TRACY"):
            prepare_dependencies(
                repository,
                DependencyRequest(
                    libraries="tracy",
                    config=configuration,
                    cmake_command=context.cmake,
                ),
                command_io=command_io,
                environment=context.environment,
            )
    except BootstrapError as exc:
        raise BuildToolError(f"Could not prepare configure dependencies: {exc}") from exc
