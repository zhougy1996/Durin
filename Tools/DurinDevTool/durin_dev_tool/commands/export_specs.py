from pathlib import Path
from .common_specs import CONTEXT_ARGUMENTS, REPORT_FORMAT
from .specification import CommandSpec, argument

COMMAND_SPEC = CommandSpec(
    "export", "export a Game through the engine worker", "durin_dev_tool.export:run",
    required_modules=("rich", "jsonschema"), help_group="Assets",
    arguments=CONTEXT_ARGUMENTS + (
        argument("--project", dest="project_path", type=Path, required=True, help="project descriptor"),
        argument("--output-dir", type=Path, required=True, help="new invocation directory; existing directories are rejected"),
        argument("--configuration", default="Release", choices=("Debug", "Release"), help="target Game configuration"),
        argument("--build", default="auto", choices=("auto", "reuse"), help="native build policy"),
        argument("--cook", default="auto", choices=("auto", "reuse"), help="Cook policy"),
        argument("--cook-dir", type=Path, help="optional committed Cook directory"),
        argument("--preview", action="store_true", help="read-only planning"),
        argument("--verify", action="store_true", help="verify bounded Game startup"),
        argument("--archive", action="store_true", help="create a delivery archive"),
        REPORT_FORMAT,
    ),
)
