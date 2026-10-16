from __future__ import annotations

from .specification import argument


PROFILE = argument("--profile", group="advanced options", help="host build profile")
PRESET = argument("--preset", help="registered CMake configure preset")
PLAIN = argument(
    "--plain",
    group="advanced options",
    action="store_true",
    help="disable colors and styled terminal output",
)
CONTEXT_ARGUMENTS = (PROFILE, PRESET)
DISPLAY_ARGUMENTS = (PLAIN,)
