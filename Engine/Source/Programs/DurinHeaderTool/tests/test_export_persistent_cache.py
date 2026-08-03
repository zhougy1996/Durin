import json
import logging
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import config as configs
from durin_header_tool.cache.persistent_header_cache import CachePhase
from durin_header_tool.generators import module_export_file_generator as export_generator
from durin_header_tool.model.export_info import ExportedSymbolInfo


_NATIVE_FINGERPRINT = "1" * 64


class _ExportHarness:
    module_name = "Engine"

    def __init__(self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
        self.module_dir = tmp_path / "Module"
        self.output_dir = tmp_path / "Generated"
        self.cache_root = tmp_path / "Intermediate" / "DHTCache"
        self.headers = ["Public/Engine/Actor.h", "Public/Engine/World.h"]
        self.parse_calls: list[str] = []
        self.symbols_by_header: dict[str, dict[str, ExportedSymbolInfo]] = {}
        for header in self.headers:
            header_path = self.module_dir / header
            header_path.parent.mkdir(parents=True, exist_ok=True)
            header_path.write_text(f"// {header}\n", encoding="utf-8")
            self.symbols_by_header[header] = {
                self.symbol(header).QualifiedName: self.symbol(header)
            }

        self.module_config = SimpleNamespace(
            module_dir=self.module_dir,
            reflect_headers=self.headers,
        )
        monkeypatch.setattr(configs, "ARCH", "Win64")
        monkeypatch.setattr(configs, "RUNTIME_VARIANT", "DurinEditor")
        monkeypatch.setattr(configs, "TOOL_FINGERPRINT", "fixture-tool")
        monkeypatch.setattr(configs, "get_module_config", lambda _module: self.module_config)
        monkeypatch.setattr(
            configs,
            "collect_all_dependent_module_with_export_file",
            lambda _module: [],
        )
        monkeypatch.setattr(
            export_generator.utils,
            "get_module_export_file_path",
            lambda _module: self.export_path,
        )
        monkeypatch.setattr(
            export_generator.utils,
            "get_module_export_manifest_file_path",
            lambda _module: self.manifest_path,
        )
        monkeypatch.setattr(
            export_generator.utils,
            "get_module_dht_cache_root",
            lambda _module: self.cache_root,
        )
        monkeypatch.setattr(
            export_generator,
            "fingerprint_native_libclang",
            lambda: _NATIVE_FINGERPRINT,
        )
        monkeypatch.setattr(export_generator, "load_dependency_symbols", lambda _module: {})
        monkeypatch.setattr(export_generator, "_parse_header_export_worker", self.parse_worker)

    @property
    def export_path(self) -> Path:
        return self.output_dir / f"{self.module_name}.export"

    @property
    def manifest_path(self) -> Path:
        return self.output_dir / f"{self.module_name}.export.manifest"

    def symbol(self, header: str, *, short_name: str | None = None) -> ExportedSymbolInfo:
        name = short_name or Path(header).stem
        qualified_name = f"Durin::{name}"
        return ExportedSymbolInfo(
            Kind="class",
            ShortName=name,
            Namespace="Durin",
            QualifiedName=qualified_name,
            GeneratedHelperName=f"Z_Construct_DClass_Durin_{name}",
            Header=header,
            API="ENGINE_API",
        )

    def parse_worker(self, args):
        header = args[1]
        self.parse_calls.append(header)
        return header, self.symbols_by_header[header], 1.0

    def generate(self) -> None:
        export_generator.generate_module_export_file(self.module_name, max_workers=1)

    def delete_outputs(self) -> None:
        self.export_path.unlink(missing_ok=True)
        self.manifest_path.unlink(missing_ok=True)

    def cache_entries(self) -> list[Path]:
        return sorted((self.cache_root / self.module_name / CachePhase.EXPORT.value).glob("*.json"))


@pytest.fixture
def export_harness(tmp_path, monkeypatch):
    return _ExportHarness(tmp_path, monkeypatch)


def test_missing_generated_exports_reconstruct_byte_identically_without_parsing(export_harness, caplog):
    harness = export_harness
    harness.generate()
    cold_export = harness.export_path.read_bytes()
    cold_manifest = harness.manifest_path.read_bytes()
    assert harness.parse_calls == harness.headers

    harness.delete_outputs()
    harness.parse_calls.clear()
    with caplog.at_level(logging.INFO):
        harness.generate()

    assert harness.parse_calls == []
    assert harness.export_path.read_bytes() == cold_export
    assert harness.manifest_path.read_bytes() == cold_manifest
    assert "hits=2 misses=0 materialized=2 parsed=0" in caplog.text


def test_partial_hit_and_changed_header_preserve_declared_header_order(export_harness):
    harness = export_harness
    harness.generate()
    changed_header = harness.headers[1]
    changed_path = harness.module_dir / changed_header
    changed_path.write_text("// changed export projection\n", encoding="utf-8")
    changed_symbol = harness.symbol(changed_header, short_name="ChangedWorld")
    harness.symbols_by_header[changed_header] = {changed_symbol.QualifiedName: changed_symbol}
    harness.delete_outputs()
    harness.parse_calls.clear()

    harness.generate()

    manifest = json.loads(harness.manifest_path.read_text(encoding="utf-8"))
    export = json.loads(harness.export_path.read_text(encoding="utf-8"))
    assert harness.parse_calls == [changed_header]
    assert list(manifest["RawSymbolsByHeader"]) == sorted(harness.headers)
    assert set(export["Symbols"]) == {"Durin::Actor", "Durin::ChangedWorld"}

    # The in-memory reconstruction order, which decides duplicate-symbol
    # precedence during module resolution, follows the module declaration.
    old_export, old_manifest = export_generator._load_previous_export(harness.module_name)
    new_manifest = export_generator._make_current_export_manifest(harness.module_name, old_manifest)
    raw_symbols, parsed_count = export_generator._build_module_export_from_cache(
        harness.module_name,
        None,
        new_manifest,
        1,
        export_generator.make_persistent_header_cache(harness.module_name),
        _NATIVE_FINGERPRINT,
        export_generator.CacheDiagnostics(),
    )
    assert list(raw_symbols) == harness.headers
    assert parsed_count == 0
    assert old_export is not None


def test_removed_and_renamed_headers_cleanup_stale_entries(export_harness):
    harness = export_harness
    harness.generate()
    old_entries = set(harness.cache_entries())
    renamed_header = "Public/Engine/RenamedWorld.h"
    (harness.module_dir / renamed_header).write_text("// renamed\n", encoding="utf-8")
    renamed_symbol = harness.symbol(renamed_header)
    harness.symbols_by_header[renamed_header] = {renamed_symbol.QualifiedName: renamed_symbol}
    harness.headers[:] = [harness.headers[0], renamed_header]
    harness.delete_outputs()
    harness.parse_calls.clear()

    harness.generate()

    assert harness.parse_calls == [renamed_header]
    assert len(harness.cache_entries()) == 2
    assert len(old_entries & set(harness.cache_entries())) == 1


def test_tool_change_invalidates_every_export_entry(export_harness, monkeypatch):
    harness = export_harness
    harness.generate()
    harness.delete_outputs()
    harness.parse_calls.clear()
    monkeypatch.setattr(configs, "TOOL_FINGERPRINT", "changed-tool")

    harness.generate()

    assert harness.parse_calls == harness.headers


def test_damaged_entry_warns_reparses_and_is_replaced(export_harness, caplog):
    harness = export_harness
    harness.generate()
    damaged_entry = harness.cache_entries()[0]
    damaged_entry.write_text("{", encoding="utf-8")
    harness.delete_outputs()
    harness.parse_calls.clear()

    with caplog.at_level(logging.WARNING):
        harness.generate()

    assert len(harness.parse_calls) == 1
    assert "Ignoring invalid persistent cache entry" in caplog.text
    assert json.loads(damaged_entry.read_text(encoding="utf-8"))["EntryKind"] == "export"


def test_failed_header_extraction_does_not_publish_any_new_entries(export_harness, monkeypatch):
    harness = export_harness

    def fail_second_header(args):
        if args[1] == harness.headers[1]:
            raise ValueError("unsupported export")
        return harness.parse_worker(args)

    monkeypatch.setattr(export_generator, "_parse_header_export_worker", fail_second_header)

    with pytest.raises(ValueError, match="unsupported export"):
        harness.generate()

    assert harness.cache_entries() == []
    assert not harness.export_path.exists()
    assert not harness.manifest_path.exists()
