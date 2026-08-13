from __future__ import annotations

from datetime import datetime
from pathlib import Path

from ..errors import DevToolError


class BuildToolError(DevToolError):
    def __init__(
        self,
        message: str,
        *,
        command: list[str] | None = None,
        exit_code: int | None = None,
        recovery: str = "",
        output_excerpt: str = "",
        log_path: Path | None = None,
        process_id: int | None = None,
        started_at_utc: datetime | None = None,
        ended_at_utc: datetime | None = None,
    ):
        super().__init__(message)
        self.command = command
        self.exit_code = exit_code
        self.recovery = recovery
        self.output_excerpt = output_excerpt
        self.log_path = log_path
        self.process_id = process_id
        self.started_at_utc = started_at_utc
        self.ended_at_utc = ended_at_utc


class BuildToolInterruptedError(BuildToolError):
    pass
