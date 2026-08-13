"""Bootstrap application services over manifests, acquisition, installers, and setup."""

from __future__ import annotations

from pathlib import Path

from ..context import CommandIO, RepositoryContext
from . import manifests
from .dependency_service import prepare_dependencies
from .models import DependencyRequest
from .setup import setup_repository


def validate_dependencies(repository: RepositoryContext, command_io: CommandIO) -> int:
    values = manifests.load_manifests(repository)
    manifests.validate_manifests(values)
    command_io.out(f"Validated {len(values)} third-party manifests successfully.")
    return len(values)


def prepare_dependency_plan(
    repository: RepositoryContext,
    request: DependencyRequest,
    command_io: CommandIO,
) -> tuple[str, ...]:
    return prepare_dependencies(repository, request, command_io=command_io)


def setup_checkout(
    repository: RepositoryContext,
    command_io: CommandIO,
    *,
    interactive: bool,
) -> Path:
    return setup_repository(repository, command_io, interactive=interactive)
