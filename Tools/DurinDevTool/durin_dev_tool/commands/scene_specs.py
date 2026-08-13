from __future__ import annotations

from pathlib import Path

from .common_specs import CONTEXT_ARGUMENTS, DISPLAY_ARGUMENTS
from .specification import CommandSpec, argument


COMMAND_SPEC = CommandSpec(
    "scene",
    "author Level scenes through a bounded non-interactive Editor process",
    subcommands=(
        CommandSpec(
            "graybox-build",
            "create a new Box-based open-arena Level",
            "durin_dev_tool.scene:run",
            required_modules=("rich",),
            arguments=CONTEXT_ARGUMENTS
            + DISPLAY_ARGUMENTS
            + (
                argument("--project", dest="project_path", type=Path, required=True),
                argument("--output", dest="mounted_output", required=True),
                argument("--width", type=float, default=20.0),
                argument("--depth", type=float, default=20.0),
                argument("--floor-thickness", dest="floor_thickness", type=float, default=0.5),
                argument("--wall-height", dest="wall_height", type=float, default=4.0),
                argument("--wall-thickness", dest="wall_thickness", type=float, default=0.5),
                argument("--ceiling", action="store_true"),
                argument("--timeout", type=int, choices=range(1, 3601), default=300, metavar="1..3600"),
            ),
        ),
    ),
)
