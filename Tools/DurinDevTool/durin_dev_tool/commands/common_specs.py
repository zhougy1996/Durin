from __future__ import annotations

from .specification import argument


PROFILE = argument("--profile", group="advanced options", help="host build profile")
PRESET = argument("--preset", help="full preset name or number from presets")
COLOR = argument(
    "--color",
    group="advanced options",
    choices=("auto", "always", "never"),
    default="auto",
    help="ANSI color policy (default: auto; honors NO_COLOR)",
)
CONTEXT_ARGUMENTS = (PROFILE, PRESET)
DISPLAY_ARGUMENTS = (COLOR,)

DRY_RUN = argument(
    "--dry-run", action="store_true",
    help="preview the action instead of applying it (default: execute)",
)

REPORT_FORMAT = argument(
    "--format", dest="output_format", choices=("terminal", "json"),
    default="terminal", help="result format (default: terminal)",
)
