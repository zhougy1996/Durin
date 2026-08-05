"""Validate the audited repository-owned direct GLM boundary."""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys


ALLOWED_CATEGORIES = {
    "alias-declaration",
    "backend-implementation",
    "third-party-interop",
    "shader-layout-interop",
    "reference-test",
    "deferred-plan",
    "migration-debt",
}
SOURCE_SUFFIXES = {".cpp", ".h", ".hpp"}
SCAN_ROOTS = ("Engine/Source", "Engine/Tests")
INCLUDE_PATTERN = re.compile(r"#\s*include\s*[<\"]([^\">]*glm[^\">]*)[\">]")
SYMBOL_PATTERN = re.compile(r"glm::([A-Za-z_][A-Za-z0-9_]*)")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="Repository root. Defaults to the root containing this tool.",
    )
    parser.add_argument(
        "--allowlist",
        type=Path,
        default=Path(__file__).with_name("direct_glm_allowlist.json"),
        help="Versioned allowlist to validate.",
    )
    return parser.parse_args()


def count_direct_uses(root: Path) -> dict[str, dict[str, Counter[str]]]:
    uses: dict[str, dict[str, Counter[str]]] = {}
    for relative_root in SCAN_ROOTS:
        scan_root = root / relative_root
        for source_path in sorted(path for path in scan_root.rglob("*") if path.suffix in SOURCE_SUFFIXES):
            content = source_path.read_text(encoding="utf-8")
            includes = Counter(INCLUDE_PATTERN.findall(content))
            symbols = Counter(SYMBOL_PATTERN.findall(content))
            if not includes and not symbols:
                continue
            relative_path = source_path.relative_to(root).as_posix()
            uses[relative_path] = {"includes": includes, "symbols": symbols}
    return uses


def load_allowlist(path: Path) -> tuple[dict[str, dict[str, object]], list[str]]:
    errors: list[str] = []
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("version") != 1:
        errors.append(f"{path}: expected version 1")

    entries: dict[str, dict[str, object]] = {}
    for entry in document.get("entries", []):
        entry_path = entry.get("path")
        if not isinstance(entry_path, str) or not entry_path:
            errors.append(f"{path}: every entry requires a non-empty path")
            continue
        if entry_path in entries:
            errors.append(f"{path}: duplicate entry for {entry_path}")
            continue
        if "\\" in entry_path or Path(entry_path).is_absolute():
            errors.append(f"{entry_path}: path must be normalized and repository-relative")
        category = entry.get("category")
        if category not in ALLOWED_CATEGORIES:
            errors.append(f"{entry_path}: unsupported category {category!r}")
        if not isinstance(entry.get("owner"), str) or not entry["owner"].strip():
            errors.append(f"{entry_path}: owner is required")
        if not isinstance(entry.get("rationale"), str) or not entry["rationale"].strip():
            errors.append(f"{entry_path}: rationale is required")
        entries[entry_path] = entry
    return entries, errors


def normalized_counts(entry: dict[str, object], key: str) -> Counter[str]:
    raw_counts = entry.get(key, {})
    if not isinstance(raw_counts, dict):
        raise ValueError(f"{key} must be an object")
    return Counter({str(name): int(count) for name, count in raw_counts.items()})


def main() -> int:
    arguments = parse_arguments()
    root = arguments.root.resolve()
    allowlist_path = arguments.allowlist.resolve()
    actual = count_direct_uses(root)
    expected, errors = load_allowlist(allowlist_path)

    for path in sorted(actual.keys() - expected.keys()):
        errors.append(f"{path}: unclassified direct GLM use")
    for path in sorted(expected.keys() - actual.keys()):
        errors.append(f"{path}: stale allowlist entry has no direct GLM use")

    for path in sorted(actual.keys() & expected.keys()):
        for key in ("includes", "symbols"):
            try:
                expected_counts = normalized_counts(expected[path], key)
            except (TypeError, ValueError) as error:
                errors.append(f"{path}: invalid {key}: {error}")
                continue
            actual_counts = actual[path][key]
            if actual_counts != expected_counts:
                errors.append(
                    f"{path}: {key} changed; expected {dict(sorted(expected_counts.items()))}, "
                    f"found {dict(sorted(actual_counts.items()))}"
                )

    if errors:
        print("Direct GLM boundary validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    include_count = sum(sum(use["includes"].values()) for use in actual.values())
    symbol_count = sum(sum(use["symbols"].values()) for use in actual.values())
    print(
        f"Direct GLM boundary verified: {len(actual)} files, "
        f"{symbol_count} symbol uses, {include_count} direct includes."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
