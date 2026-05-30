#!/usr/bin/env python3
from __future__ import annotations

import shutil
import sys
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]

LEGACY_PATHS = [
    REPO_ROOT / "Engine" / "Source" / "ThirdParty" / "assimp",
    REPO_ROOT / "Engine" / "Source" / "ThirdParty" / "glfw",
    REPO_ROOT / "Engine" / "Source" / "ThirdParty" / "glm",
    REPO_ROOT / "Engine" / "Source" / "ThirdParty" / "googletest",
    REPO_ROOT / "Engine" / "Source" / "ThirdParty" / "rapidyaml",
    REPO_ROOT / "Engine" / "Source" / "ThirdParty" / "slang",
    REPO_ROOT / "Engine" / "Source" / "ThirdParty" / "spdlog",
    REPO_ROOT / "Build" / "ThirdParty" / "Install",
]


def main() -> int:
    dry_run = "--dry-run" in sys.argv
    for path in LEGACY_PATHS:
        if not path.exists():
            print(f"MISSING {path}")
            continue
        if dry_run:
            print(f"WOULD REMOVE {path}")
            continue
        shutil.rmtree(path)
        print(f"REMOVED {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
