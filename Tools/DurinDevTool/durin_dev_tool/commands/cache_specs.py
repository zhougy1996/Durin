from __future__ import annotations

from pathlib import Path
from .common_specs import CONTEXT_ARGUMENTS, REPORT_FORMAT
from .specification import CommandSpec, argument


def _operation(name: str, summary: str) -> CommandSpec:
    return CommandSpec(
        name, summary, "durin_dev_tool.cache:run", required_modules=("rich", "jsonschema"),
        arguments=CONTEXT_ARGUMENTS + (
            argument("--root", type=Path, required=True, help="explicit local DDC root (absolute or repository-relative)"),
            argument("--max-entries", type=int, default=100000, help="scan entry limit, at most 1000000"),
            argument("--max-file-bytes", type=int, default=256 * 1024 * 1024, help="maximum verification input allocation"),
            argument("--max-read-bytes", type=int, default=1024 * 1024 * 1024, help="total verification read limit"),
            argument("--max-working-bytes", type=int, default=512 * 1024 * 1024, help="retained verification payload/codec limit"),
            REPORT_FORMAT,
        ), defaults=(("operation", name),),
    )


COMMAND_SPEC = CommandSpec(
    "cache", "inspect, verify, or clear offline local DDC storage", help_group="Assets",
    subcommands=(
        _operation("inspect", "report current and legacy record/value disk usage; refuse active roots"),
        _operation("verify", "verify current manifests and blobs under exclusive ownership"),
        _operation("clear", "delete records/ and values/ only; require all cache users to stop"),
    ),
    epilog="Build DurinCacheTool first. Offline commands acquire exclusive ownership; clear preserves unknown root siblings.",
)
