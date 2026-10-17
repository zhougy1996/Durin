from __future__ import annotations

from .common_specs import COLOR
from .specification import CommandSpec, argument


HANDLER = "durin_dev_tool.bootstrap.handler:run"

SETUP_COMMAND_SPEC = CommandSpec(
    "setup",
    "prepare this main checkout",
    HANDLER,
    help_group="Getting started",
    arguments=(
        COLOR,
        argument(
            "--check",
            action="store_true",
            help="check local configuration and prerequisites without prompts, writing setup files, or installing tools",
        ),
        argument(
            "--non-interactive",
            action="store_true",
            help="use configured or automatically detected toolchain settings without prompting",
        ),
        argument(
            "--skip-development-tools",
            action="store_true",
            help="skip Tracy profiler and capture tools for CI or build-only environments",
        ),
    ),
    defaults=(("bootstrap_action", "setup"),),
)

DEPENDENCY_COMMAND_SPEC = CommandSpec(
    "dependency",
    "prepare and validate third-party dependencies",
    help_group="Workspace",
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
                    help="select the default dependency set; optional groups require --with-tests or --with-development",
                ),
                argument("libraries", nargs="*", metavar="NAME", help="dependency names; use --all for the default set"),
                argument(
                    "--config",
                    dest="dependency_config",
                    choices=("debug", "release", "all"),
                    default="all",
                ),
                argument("--with-tests", action="store_true"),
                argument("--with-development", action="store_true"),
                argument("--cmake", dest="dependency_cmake", default=None),
                COLOR,
            ),
            defaults=(("bootstrap_action", "dependency-prepare"),),
            epilog="Examples:\n  DevTool dependency prepare tracy tracy-tools\n  DevTool dependency prepare glm spdlog --config debug\n  DevTool dependency prepare --all",
        ),
        CommandSpec(
            "validate",
            "validate every third-party dependency manifest",
            HANDLER,
            arguments=(COLOR,),
            defaults=(("bootstrap_action", "dependency-validate"),),
        ),
    ),
)
