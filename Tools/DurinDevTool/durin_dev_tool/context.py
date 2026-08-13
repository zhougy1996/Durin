"""Explicit repository and command execution context."""

from __future__ import annotations

import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import TextIO

from .configuration import RepositoryConfig, load_repository_config
from .repository import discover_repository_root


@dataclass(frozen=True)
class RepositoryContext:
    """Immutable checkout identity and its tracked DurinDevTool configuration."""

    root: Path
    config: RepositoryConfig

    @classmethod
    def load(cls, repository_root: Path | None = None) -> "RepositoryContext":
        root = (repository_root or discover_repository_root()).resolve()
        return cls(root=root, config=load_repository_config(root))

    def resolve(self, path: Path) -> Path:
        return self.root / path

    def at_root(self, repository_root: Path) -> "RepositoryContext":
        root = repository_root.resolve()
        return replace(self, root=root, config=replace(self.config, repository_root=root))


@dataclass(frozen=True)
class CommandIO:
    """Streams and display policy owned by one command invocation."""

    stdout: TextIO
    stderr: TextIO
    plain: bool = False

    @classmethod
    def system(cls, *, plain: bool = False) -> "CommandIO":
        return cls(sys.stdout, sys.stderr, plain=plain)

    def out(self, message: object = "") -> None:
        print(message, file=self.stdout)

    def error(self, message: object = "") -> None:
        print(message, file=self.stderr)
