#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from agent_config import AgentConfigError, ensure_agent_config


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Create the optional local Agent build config from its template.")
    parser.add_argument("--dry-run", action="store_true", help="Print the operation without changing the filesystem.")
    return parser.parse_args()


def main() -> int:
    try:
        args = parse_args()
        ensure_agent_config(REPO_ROOT, dry_run=args.dry_run)
    except (AgentConfigError, OSError) as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
