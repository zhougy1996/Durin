import sys
from pathlib import Path
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import io as utils


class TestAtomicFile:
    def test_generate_file_replaces_content_and_skips_unchanged_write(self, tmp_path):
        output_path = tmp_path / "generated.txt"
        utils.generate_file(output_path, "first")
        first_mtime = output_path.stat().st_mtime_ns

        utils.generate_file(output_path, "first")
        assert output_path.stat().st_mtime_ns == first_mtime

        utils.generate_file(output_path, "second")
        assert output_path.read_text(encoding="utf-8") == "second"
        assert list(output_path.parent.glob(f".{output_path.name}.*.tmp")) == []

    def test_replace_failure_preserves_old_file_and_removes_temporary_file(self, tmp_path):
        output_path = tmp_path / "generated.txt"
        output_path.write_text("old", encoding="utf-8")

        with mock.patch("durin_header_tool.io.file_helper.os.replace", side_effect=OSError("replace failed")):
            with pytest.raises(OSError, match="replace failed"):
                utils.generate_file(output_path, "new")

        assert output_path.read_text(encoding="utf-8") == "old"
        assert list(output_path.parent.glob(f".{output_path.name}.*.tmp")) == []

    def test_invalid_utf8_output_is_replaced(self, tmp_path):
        output_path = tmp_path / "generated.txt"
        output_path.write_bytes(b"\xff\xfe")

        utils.generate_file(output_path, "recovered")

        assert output_path.read_text(encoding="utf-8") == "recovered"
