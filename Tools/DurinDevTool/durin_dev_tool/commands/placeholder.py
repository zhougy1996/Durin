from __future__ import annotations

from ..errors import DevToolError


def run(*_args: object, **_kwargs: object) -> int:
    raise DevToolError(
        "The build domain has not been migrated to DurinDevTool yet. "
        "Use the existing BuildTool entrypoint until the repository cutover."
    )
