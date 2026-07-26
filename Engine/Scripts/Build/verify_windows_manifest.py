#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
import tempfile
import xml.etree.ElementTree as ElementTree
from pathlib import Path


def verify_long_path_manifest(executable: Path, manifest_tool: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="durin-manifest-") as directory:
        extracted = Path(directory) / "embedded.manifest"
        result = subprocess.run(
            [
                str(manifest_tool),
                "-nologo",
                f"-inputresource:{executable};#1",
                f"-out:{extracted}",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise RuntimeError(f'Could not extract the embedded manifest from "{executable}": {detail}')

        root = ElementTree.parse(extracted).getroot()
        values = [
            (element.text or "").strip().casefold()
            for element in root.iter()
            if element.tag.endswith("}longPathAware") or element.tag == "longPathAware"
        ]
        if values != ["true"]:
            raise RuntimeError(
                f'Executable "{executable}" must contain exactly one longPathAware=true declaration.'
            )


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify a Windows executable's embedded long-path manifest.")
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--manifest-tool", type=Path, required=True)
    args = parser.parse_args()
    verify_long_path_manifest(args.executable, args.manifest_tool)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
