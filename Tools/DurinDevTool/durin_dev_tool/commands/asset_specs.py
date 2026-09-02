from __future__ import annotations

from pathlib import Path

from .common_specs import CONTEXT_ARGUMENTS
from .specification import CommandSpec, argument


HANDLER = "durin_dev_tool.asset:run"

PROJECT_ARGUMENT = argument(
    "--project",
    dest="project_path",
    type=Path,
    default=None,
    help="project descriptor (defaults to the configured game project)",
)
JSON_ARGUMENT = argument(
    "--json",
    dest="format_name",
    action="store_const",
    const="json",
    default="human",
    help="write stable machine-readable JSON",
)

COMMAND_SPEC = CommandSpec(
    "asset",
    "check, resave, or inspect authored assets",
    subcommands=(
        CommandSpec(
            "check",
            "inspect authored packages without changing them",
            HANDLER,
            required_modules=("rich", "jsonschema"),
            arguments=CONTEXT_ARGUMENTS
            + (
                PROJECT_ARGUMENT,
                JSON_ARGUMENT,
            ),
        ),
        CommandSpec(
            "resave",
            "preview or apply canonical resaves",
            HANDLER,
            required_modules=("rich",),
            arguments=CONTEXT_ARGUMENTS
            + (
                argument(
                    "scopes",
                    nargs="*",
                    metavar="SCOPE",
                    help="package or package-tree path such as /Game/Characters",
                ),
                PROJECT_ARGUMENT,
                argument(
                    "--all",
                    dest="whole_project",
                    action="store_true",
                    help="select the whole project",
                ),
                argument(
                    "--apply",
                    action="store_true",
                    help="write the previewed resaves",
                ),
                JSON_ARGUMENT,
            ),
        ),
    ),
    default_subcommand="check",
)
