from __future__ import annotations

import sys
from pathlib import Path


# CMake invokes this file by absolute path from different working directories.
# Add the package parent when it is executed as a script; ``python -m`` already
# supplies the correct import path.
if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from durin_header_tool.cli.main import main

if __name__ == "__main__":
    main()
