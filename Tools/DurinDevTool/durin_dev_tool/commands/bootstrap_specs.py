from __future__ import annotations

from .common_specs import PLAIN
from .specification import CommandSpec, argument


HANDLER = "durin_dev_tool.bootstrap.handler:run"

SETUP_COMMAND_SPEC = CommandSpec(
    "setup",
    "prepare this main checkout",
    HANDLER,
    arguments=(
        PLAIN,
        argument(
            "--non-interactive",
            action="store_true",
            help="use configured or automatically detected toolchain settings without prompting",
        ),
    ),
    defaults=(("bootstrap_action", "setup"),),
)

DEPENDENCY_COMMAND_SPEC = CommandSpec(
    "dependency",
    "prepare and validate third-party dependencies",
    subcommands=(
        CommandSpec(
            "prepare",
            "prepare selected third-party dependencies",
            HANDLER,
            arguments=(
                argument(
                    "--all",
                    dest="all_dependencies",
                    action="store_true",
                    help="prepare all non-test dependencies",
                ),
                argument("--libs", dest="libraries", help="comma-separated dependencies to prepare"),
                argument(
                    "--config",
                    dest="dependency_config",
                    choices=("Debug", "Release", "All"),
                    default="All",
                ),
                argument("--with-tests", action="store_true"),
                argument("--with-development", action="store_true"),
                argument("--cmake", dest="dependency_cmake", default=None),
            ),
            defaults=(("bootstrap_action", "dependency-prepare"),),
        ),
        CommandSpec(
            "validate",
            "validate every third-party dependency manifest",
            HANDLER,
            defaults=(("bootstrap_action", "dependency-validate"),),
        ),
    ),
)
