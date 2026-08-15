from __future__ import annotations

from pathlib import Path

from .common_specs import CONTEXT_ARGUMENTS
from .specification import CommandSpec, argument


HANDLER = "durin_dev_tool.asset:run"

COMMAND_SPEC = CommandSpec(
    "asset",
    "inspect or enforce authored asset package compatibility",
    subcommands=(
        CommandSpec(
            "baseline",
            "require every authored package to match the current baseline",
            HANDLER,
            required_modules=("rich", "jsonschema"),
            arguments=CONTEXT_ARGUMENTS
            + (
                argument("--project", dest="project_path", type=Path, required=True),
                argument("--format", dest="format_name", choices=("human", "json"), default="human"),
            ),
        ),
        CommandSpec(
            "audit",
            "run a deterministic read-only compatibility audit",
            HANDLER,
            required_modules=("rich", "jsonschema"),
            arguments=CONTEXT_ARGUMENTS
            + (
                argument("--project", dest="project_path", type=Path, required=True),
                argument("--format", dest="format_name", choices=("human", "json"), default="human"),
                argument(
                    "--fail-on",
                    dest="fail_on",
                    choices=("incompatible", "unsupported", "error"),
                    action="append",
                    default=[],
                ),
            ),
        ),
    ),
)
