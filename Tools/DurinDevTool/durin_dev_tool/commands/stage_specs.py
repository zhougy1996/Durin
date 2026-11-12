from pathlib import Path
from .common_specs import CONTEXT_ARGUMENTS, REPORT_FORMAT
from .specification import CommandSpec, argument

COMMAND_SPEC = CommandSpec(
    "stage", "publish a source-independent cooked Game", "durin_dev_tool.stage:run",
    required_modules=("rich", "jsonschema"), help_group="Assets",
    arguments=CONTEXT_ARGUMENTS + (
        argument("--project", dest="project_path", type=Path, required=True, help="source project descriptor used by Cook"),
        argument("--cook-dir", type=Path, required=True, help="committed Cook generation"),
        argument("--output-dir", type=Path, required=True, help="complete staged Game directory"),
        argument("--game-preset", default="", help="matching Game preset (inferred when unique)"),
        REPORT_FORMAT,
    ),
)
