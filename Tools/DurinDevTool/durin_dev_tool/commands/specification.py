from __future__ import annotations

import importlib
from dataclasses import dataclass
from typing import Callable

from ..errors import DevToolError


@dataclass(frozen=True)
class ArgumentSpec:
    flags: tuple[str, ...]
    kwargs: dict[str, object]


@dataclass(frozen=True)
class CommandSpec:
    name: str
    summary: str
    handler: str = ""
    arguments: tuple[ArgumentSpec, ...] = ()
    required_modules: tuple[str, ...] = ()
    subcommands: tuple["CommandSpec", ...] = ()
    default_subcommand: str = ""
    defaults: tuple[tuple[str, object], ...] = ()
    epilog: str = ""

    def load_handler(self) -> Callable[..., int]:
        module_name, separator, attribute = self.handler.partition(":")
        if not separator:
            raise DevToolError(f'Invalid handler registration for "{self.name}".')
        module = importlib.import_module(module_name)
        return getattr(module, attribute)


def argument(*flags: str, **kwargs: object) -> ArgumentSpec:
    return ArgumentSpec(flags, kwargs)
