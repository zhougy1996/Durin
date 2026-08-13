from __future__ import annotations

from .specification import CommandSpec, argument


COMMAND_SPECS = (
    CommandSpec(
        "help",
        "show command help",
        "durin_dev_tool.commands.core:show_help",
        arguments=(argument("command_path", nargs="*"),),
    ),
    CommandSpec(
        "shell",
        "open the interactive shell",
        "durin_dev_tool.commands.core:open_shell",
    ),
)
