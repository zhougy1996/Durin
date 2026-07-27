#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path


# Support both ``python -m durin_build_tool`` and direct execution of this
# package-owned entry file without changing the caller's working directory.
if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from durin_build_tool.cli import main


if __name__ == "__main__":
    raise SystemExit(main())
