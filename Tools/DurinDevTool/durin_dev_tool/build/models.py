from __future__ import annotations

from dataclasses import dataclass, replace
from enum import Enum
from typing import Any, Mapping


class Action(str, Enum):
    SHELL = "shell"
    STOP = "stop"
    PRESETS = "presets"
    PRESET = "preset"
    STATUS = "status"
    PATH = "path"
    OPEN = "open"
    CONFIGURE = "configure"
    BUILD = "build"
    CLEAN = "clean"
    RECOVER = "recover"
    REBUILD = "rebuild"
    TEST = "test"
    PURGE = "purge"
    RUN = "run"
    CREATE_MODULE = "create-module"
    CREATE_PROJECT = "create-project"


class CreateKind(str, Enum):
    MODULE = "module"
    PROJECT = "project"


class ModuleKind(str, Enum):
    RUNTIME = "runtime"
    EDITOR = "editor"
    DEVELOPER = "developer"


class LinkType(str, Enum):
    SHARED = "shared"
    STATIC = "static"


class EnvironmentProvider(str, Enum):
    INHERIT = "inherit"
    SCRIPT = "script"
    VISUAL_STUDIO = "visual-studio"


class OutputMode(str, Enum):
    AUTO = "auto"
    COMPACT = "compact"
    PROGRESS = "progress"
    FULL = "full"


class TestMode(str, Enum):
    ROUTINE = "routine"
    ISOLATION = "isolation"
    STRESS = "stress"
    REPORT = "report"
    CHARACTERIZATION = "characterization"
    QUALIFICATION = "qualification"


@dataclass(frozen=True)
class EnvironmentSetup:
    script: str = ""
    arguments: tuple[str, ...] = ()


@dataclass(frozen=True)
class LocalConfig:
    cmake_command: str = ""
    default_build_profile: str = ""
    jobs: int = 0
    environment_setup: EnvironmentSetup = EnvironmentSetup()

    def with_environment_script(self, script: str) -> "LocalConfig":
        return replace(
            self,
            environment_setup=replace(self.environment_setup, script=script),
        )


@dataclass(frozen=True)
class BuildProfile:
    name: str
    host: str
    default_preset: str
    presets: tuple[str, ...]
    environment_provider: EnvironmentProvider
    platform: str
    test_executable_suffix: str
    is_default: bool
    required_commands: tuple[str, ...]


@dataclass(frozen=True)
class ConfigurePreset:
    name: str
    values: Mapping[str, Any]
