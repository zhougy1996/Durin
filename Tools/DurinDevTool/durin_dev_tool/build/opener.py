"""Platform file-manager integration for resolved repository locations."""

from __future__ import annotations

import os
import subprocess

from .errors import BuildToolError
from .settings import default_build_paths
from .locations import ResolvedLocation


def open_location(
    location: ResolvedLocation,
    *,
    current_host: str,
    root: Path | None = None,
) -> None:
    if not location.is_directory:
        raise BuildToolError(
            f'{location.spec.name.capitalize()} directory was not found: '
            f'"{location.path}".',
            recovery=location.spec.missing_recovery,
        )
    try:
        if current_host == "windows":
            os.startfile(location.path)  # type: ignore[attr-defined]
        else:
            opener = "open" if current_host == "macos" else "xdg-open"
            subprocess.Popen(
                [opener, str(location.path)],
                cwd=root or default_build_paths().root,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
    except (AttributeError, OSError) as exc:
        raise BuildToolError(
            f'Could not open {location.spec.name} directory "{location.path}": {exc}'
        ) from exc
