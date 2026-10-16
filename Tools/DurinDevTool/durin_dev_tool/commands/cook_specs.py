from __future__ import annotations

from pathlib import Path

from .common_specs import CONTEXT_ARGUMENTS, REPORT_FORMAT
from .specification import CommandSpec, argument


COMMAND_SPEC = CommandSpec(
    "cook",
    "publish a deterministic project Cook generation",
    "durin_dev_tool.cook:run",
    required_modules=("rich", "jsonschema"),
    help_group="Assets",
    epilog="Examples:\n  DevTool cook --output-dir Saved/Cooked\n  DevTool cook --output-dir Saved/MacCook --target macos --dry-run",
    arguments=CONTEXT_ARGUMENTS
    + (
        argument(
            "--project",
            dest="project_path",
            type=Path,
            default=None,
            help="project descriptor (defaults to the configured game project)",
        ),
        argument(
            "--output-dir",
            dest="output_path",
            type=Path,
            required=True,
            help="absolute or repository-relative Cook output root",
        ),
        argument("--target", choices=("win64", "macos"), default=None, help="Cook platform (default: selected build profile's platform)"),
        argument(
            "--root",
            dest="roots",
            action="append",
            default=[],
            metavar="PACKAGE",
            help="explicit root package; repeat to add roots",
        ),
        argument(
            "--no-incremental",
            action="store_true",
            help="capture every selected package instead of accepting Cook hits",
        ),
        argument(
            "--dry-run",
            action="store_true",
            help="capture and report plans without opening an output transaction",
        ),
        REPORT_FORMAT,
    ),
)
