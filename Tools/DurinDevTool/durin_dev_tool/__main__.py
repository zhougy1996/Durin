#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path


def _configure_standard_streams() -> None:
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8")


if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from durin_dev_tool.cli import main


if __name__ == "__main__":
    _configure_standard_streams()
    raise SystemExit(main())
