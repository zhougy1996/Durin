from __future__ import annotations

from pathlib import Path

from .common_specs import CONTEXT_ARGUMENTS, DRY_RUN, REPORT_FORMAT
from .specification import CommandSpec, argument


HANDLER = "durin_dev_tool.asset:run"

PROJECT_ARGUMENT = argument(
    "--project",
    dest="project_path",
    type=Path,
    default=None,
    help="project descriptor (defaults to the configured game project)",
)

COMMAND_SPEC = CommandSpec(
    "asset",
    "check, resave, or inspect authored assets",
    help_group="Assets",
    subcommands=(
        CommandSpec(
            "material-functions",
            "validate shipped standard functions and initialize missing DefaultMaterial",
            HANDLER,
            required_modules=("rich",),
            arguments=CONTEXT_ARGUMENTS + (
                PROJECT_ARGUMENT,
                DRY_RUN,
            ),
        ),
        CommandSpec(
            "material-template",
            "create a PBRSurfaceMaterial_MR template at an unused package path",
            HANDLER,
            required_modules=("rich",),
            arguments=CONTEXT_ARGUMENTS + (
                argument("destination", metavar="PACKAGE", help="destination package, such as /Game/Materials/PBRSurfaceMaterial_MR"),
                PROJECT_ARGUMENT,
                DRY_RUN,
            ),
        ),
        CommandSpec(
            "identity-audit",
            "write read-only package object and reference inventory as JSON",
            HANDLER,
            required_modules=("rich",),
            arguments=CONTEXT_ARGUMENTS + (PROJECT_ARGUMENT,),
        ),
        CommandSpec(
            "check",
            "inspect authored packages without changing them",
            HANDLER,
            required_modules=("rich", "jsonschema"),
            arguments=CONTEXT_ARGUMENTS
            + (
                PROJECT_ARGUMENT,
                REPORT_FORMAT,
            ),
        ),
        CommandSpec(
            "resave",
            "resave authored packages in the canonical format",
            HANDLER,
            usage="%(prog)s SCOPE... [options]\n       %(prog)s --all [options]",
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
                DRY_RUN,
                argument(
                    "--recompress-texture-sources",
                    action="store_true",
                    help="recompress texture sources with Zstd, including current packages",
                ),
                REPORT_FORMAT,
            ),
        ),
    ),
    default_subcommand="check",
)
