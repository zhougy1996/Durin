from __future__ import annotations

from .common_specs import COLOR, PRESET, PROFILE
from .specification import CommandSpec


COMMAND_SPEC = CommandSpec(
    "mcp",
    "verify the local MCP adapter with external clients",
    subcommands=(
        CommandSpec(
            "sdk-test",
            "run official Python SDK acceptance against the native CPU editor host",
            "durin_dev_tool.mcp:run_sdk_test",
            required_modules=("rich",),
            arguments=(PROFILE, PRESET, COLOR),
            epilog="Requires mcp==2.3.0 in Build/MCP/site; see Documentation/Editor/Guides/LocalMCP.md.",
        ),
    ),
    help_group="Build and run",
)
