from __future__ import annotations

from .specification import argument


PROFILE = argument("--profile", help="host build profile")
PRESET = argument("--preset", help="registered CMake configure preset")
PLAIN = argument(
    "--plain",
    action="store_true",
    help="disable colors and styled terminal output",
)
CONTEXT_ARGUMENTS = (PROFILE, PRESET)
DISPLAY_ARGUMENTS = (PLAIN,)
