"""Shared standard-library color policy for command renderers."""

from __future__ import annotations

import os
from typing import TextIO


def color_enabled(mode: str, stream: TextIO) -> bool:
    if mode == "always":
        return True
    if mode == "never":
        return False
    return (
        bool(getattr(stream, "isatty", lambda: False)())
        and "NO_COLOR" not in os.environ
        and os.environ.get("TERM") != "dumb"
    )
