"""Bootstrap domain models and errors."""

from __future__ import annotations

from dataclasses import dataclass

from ..errors import DevToolError


class BootstrapError(DevToolError):
    pass


@dataclass(frozen=True)
class DependencyRequest:
    use_all: bool = False
    libraries: str | None = None
    config: str = "All"
    with_tests: bool = False
    with_development: bool = False
    cmake_command: str | None = None


@dataclass(frozen=True)
class PlannedDependency:
    name: str
    kind: str
    configurations: tuple[str, ...]
