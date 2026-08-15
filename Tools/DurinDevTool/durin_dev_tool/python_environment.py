from __future__ import annotations

import sys
from pathlib import Path


def prepared_python_path(
    repository_root: Path,
    environment_path: Path,
    *,
    current_platform: str | None = None,
) -> Path:
    platform_name = current_platform or sys.platform
    relative = Path("Scripts/python.exe") if platform_name == "win32" else Path("bin/python")
    return repository_root / environment_path / relative


def launcher_command(*, current_platform: str | None = None) -> str:
    return "DevTool.bat" if (current_platform or sys.platform) == "win32" else "./DevTool"
