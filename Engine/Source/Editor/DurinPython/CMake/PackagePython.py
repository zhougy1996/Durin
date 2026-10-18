"""Package source without requiring a zlib extension during interpreter startup."""

from pathlib import Path
import sys
from zipfile import ZipFile, ZIP_STORED

root, output = map(Path, sys.argv[1:])
with ZipFile(output, "w", compression=ZIP_STORED) as archive:
    for source in sorted(root.rglob("*.py")):
        archive.write(source, source.relative_to(root).as_posix())
