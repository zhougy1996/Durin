from __future__ import annotations

import sys
from pathlib import Path


DEV_TOOL_ROOT = Path(__file__).resolve().parents[1]

if str(DEV_TOOL_ROOT) not in sys.path:
    sys.path.insert(0, str(DEV_TOOL_ROOT))
