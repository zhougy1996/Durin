from __future__ import annotations

import subprocess
import sys
from pathlib import Path

from .context import CommandIO
from .errors import DevToolError


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


def prepared_environment_is_active(
    python: Path,
    *,
    active_prefix: Path | None = None,
    base_prefix: Path | None = None,
) -> bool:
    """Return whether this process is running inside the requested environment."""
    prefix = active_prefix or Path(sys.prefix)
    base = base_prefix or Path(sys.base_prefix)
    if prefix == base:
        return False
    environment = python.parent.parent
    try:
        return environment.samefile(prefix)
    except OSError:
        return environment.resolve() == prefix.resolve()


def restart_prepared_shell(
    repository_root: Path,
    python: Path,
    session_state: dict[str, object],
    command_io: CommandIO,
) -> None:
    if prepared_environment_is_active(python):
        return
    entrypoint = (
        repository_root
        / "Tools"
        / "DurinDevTool"
        / "durin_dev_tool"
        / "__main__.py"
    )
    command_io.out("Restarting the interactive shell in Durin's prepared environment...")
    result = subprocess.run(
        [str(python), str(entrypoint), "shell"],
        cwd=repository_root,
        check=False,
        stdout=command_io.stdout,
        stderr=command_io.stderr,
    )
    if result.returncode != 0:
        raise DevToolError(f"Prepared shell exited with code {result.returncode}.")
    session_state["exit_requested"] = True
