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
from durin_header_tool.generators import module_reflection_files_generator as reflection_generator
from durin_header_tool.model.export_info import ExportedSymbolInfo


_NATIVE_FINGERPRINT = "2" * 64


class _ReflectionHarness:
    module_name = "Engine"

    def __init__(self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
        self.module_dir = tmp_path / "Module"
        self.output_dir = tmp_path / "Generated"
        self.cache_root = tmp_path / "Intermediate" / "DHTCache"
        self.dependency_export_path = tmp_path / "Dependency.export"
        self.headers = ["Public/Engine/Actor.h", "Public/Engine/World.h"]
        self.export_parse_calls: list[str] = []
        self.parse_calls: list[str] = []
        for header in self.headers:
            header_path = self.module_dir / header
            header_path.parent.mkdir(parents=True, exist_ok=True)
            header_path.write_text(f"// reflected {header}\n", encoding="utf-8")

        self.dependency_symbol = self._dependency_symbol("DEPENDENCY_API")
        self._write_dependency_export()
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
            lambda _module: ["Dependency"],
        )
        monkeypatch.setattr(
            reflection_generator.utils,
            "get_module_export_file_path",
            lambda _module: self.dependency_export_path,
        )
        monkeypatch.setattr(
            reflection_generator.utils,
            "get_module_manifest_file_path",
            lambda _module: self.manifest_path,
        )
        monkeypatch.setattr(
            reflection_generator.utils,
            "get_module_dht_output_dir",
            lambda _module: self.output_dir,
        )
        monkeypatch.setattr(
            reflection_generator.utils,
            "get_module_dht_cache_root",
            lambda _module: self.cache_root,
        )
        monkeypatch.setattr(
            reflection_generator,
            "fingerprint_native_libclang",
            lambda: _NATIVE_FINGERPRINT,
        )
        monkeypatch.setattr(
            reflection_generator,
            "load_available_symbols",
            lambda _module: {self.dependency_symbol.QualifiedName: self.dependency_symbol},
        )
        monkeypatch.setattr(
            reflection_generator,
            "_generate_reflection_output_worker",
            self.generate_worker,
        )
        monkeypatch.setattr(
            export_generator.utils,
            "get_module_export_file_path",
            lambda module: self.dependency_export_path if module == "Dependency" else self.export_path,
        )
        monkeypatch.setattr(
            export_generator.utils,
            "get_module_export_manifest_file_path",
            lambda _module: self.export_manifest_path,
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
        monkeypatch.setattr(export_generator, "_parse_header_export_worker", self.export_worker)

    @property
    def manifest_path(self) -> Path:
        return self.output_dir / f"{self.module_name}.manifest"

    @property
    def export_path(self) -> Path:
        return self.output_dir / f"{self.module_name}.export"

    @property
    def export_manifest_path(self) -> Path:
        return self.output_dir / f"{self.module_name}.export.manifest"

    def _dependency_symbol(self, api: str) -> ExportedSymbolInfo:
        return ExportedSymbolInfo(
            Kind="class",
            ShortName="DObject",
            Namespace="Durin",
            QualifiedName="Durin::DObject",
            GeneratedHelperName="Z_Construct_DClass_Durin_DObject",
            Header="DObject/Object.h",
            API=api,
        )

    def _write_dependency_export(self) -> None:
        self.dependency_export_path.write_text(
            json.dumps(
                {
                    "SchemaVersion": 5,
                    "Module": "Dependency",
                    "Symbols": {
                        self.dependency_symbol.QualifiedName: vars(self.dependency_symbol),
                    },
                },
                indent=4,
            ),
            encoding="utf-8",
        )

    def change_dependency(self) -> None:
        self.dependency_symbol = self._dependency_symbol("CHANGED_API")
        self._write_dependency_export()

    def generate_worker(self, args):
        header = args[1]
        self.parse_calls.append(header)
        ordinal = self.headers.index(header) + 1
        return {
            "header": header,
            "header_content": f"// generated header {header}\n",
            "cpp_content": f"// generated source {header} {self.dependency_symbol.API}\n",
            "class_count": ordinal,
            "property_count": ordinal * 2,
            "resolved_symbol_dependencies": {
                self.dependency_symbol.QualifiedName: {
                    "GeneratedHelperName": self.dependency_symbol.GeneratedHelperName,
                    "API": self.dependency_symbol.API,
                },
            },
            "elapsed_ms": 1.0,
        }

    def export_worker(self, args):
        header = args[1]
        self.export_parse_calls.append(header)
        name = Path(header).stem
        symbol = ExportedSymbolInfo(
            Kind="class",
            ShortName=name,
            Namespace="Durin",
            QualifiedName=f"Durin::{name}",
            GeneratedHelperName=f"Z_Construct_DClass_Durin_{name}",
            Header=header,
            API="ENGINE_API",
        )
        return header, {symbol.QualifiedName: symbol}, 1.0

    def generate_all(self) -> None:
        export_generator.generate_module_export_file(self.module_name, max_workers=1)
        self.generate()

    def cmake_owned_outputs(self) -> list[Path]:
        return [
            self.export_path,
            self.export_manifest_path,
            *self.generated_paths(),
            self.manifest_path,
        ]

    def generate(self) -> None:
        reflection_generator.generate_reflection_files(self.module_name, max_workers=1)

    def generated_paths(self) -> list[Path]:
        return sorted(self.output_dir.glob("*.gen.*"))

    def delete_generated_outputs(self, *, manifest: bool = False) -> None:
        for path in self.generated_paths():
            path.unlink()
        if manifest:
            self.manifest_path.unlink(missing_ok=True)

    def cache_entries(self) -> list[Path]:
        return sorted((self.cache_root / self.module_name / CachePhase.REFLECTION.value).glob("*.json"))


@pytest.fixture
def reflection_harness(tmp_path, monkeypatch):
    return _ReflectionHarness(tmp_path, monkeypatch)


def test_deleted_reflection_outputs_and_manifest_reconstruct_byte_identically(reflection_harness, caplog):
    harness = reflection_harness
    harness.generate()
    cold_outputs = {path.name: path.read_bytes() for path in harness.generated_paths()}
    cold_manifest = harness.manifest_path.read_bytes()
    assert harness.parse_calls == harness.headers

    harness.delete_generated_outputs(manifest=True)
    harness.parse_calls.clear()
    with caplog.at_level(logging.INFO):
        harness.generate()

    assert harness.parse_calls == []
    assert {path.name: path.read_bytes() for path in harness.generated_paths()} == cold_outputs
    assert harness.manifest_path.read_bytes() == cold_manifest
    assert "hits=2 misses=0 materialized=2 parsed=0" in caplog.text
    assert "(3 classes, 6 properties)" in caplog.text
    manifest = json.loads(cold_manifest)
    assert manifest["ResolvedSymbolDependencies"][harness.headers[0]]["Durin::DObject"]["API"] == "DEPENDENCY_API"


def test_partial_hit_parses_only_changed_header(reflection_harness, caplog):
    harness = reflection_harness
    harness.generate()
    changed_header = harness.headers[1]
    (harness.module_dir / changed_header).write_text("// changed reflected header with new content\n", encoding="utf-8")
    harness.delete_generated_outputs()
    harness.parse_calls.clear()

    with caplog.at_level(logging.INFO):
        harness.generate()

    assert harness.parse_calls == [changed_header]
    assert "hits=1 misses=1 materialized=2 parsed=1" in caplog.text


def test_dependency_export_semantics_invalidate_all_entries(reflection_harness):
    harness = reflection_harness
    harness.generate()
    harness.change_dependency()
    harness.parse_calls.clear()

    harness.generate()

    assert harness.parse_calls == harness.headers
    manifest = json.loads(harness.manifest_path.read_text(encoding="utf-8"))
    assert set(manifest["DependencyExports"]) == {"Dependency"}
    assert len(manifest["DependencyExports"]["Dependency"]) == 64


def test_generator_option_change_invalidates_all_entries(reflection_harness, monkeypatch):
    harness = reflection_harness
    harness.generate()
    harness.parse_calls.clear()
    get_fingerprint = reflection_generator.utils.get_file_fingerprint_with_old_cache
    reused_fingerprints = []

    def record_old_fingerprint(path, old_fingerprint):
        reused_fingerprints.append(old_fingerprint)
        return get_fingerprint(path, old_fingerprint)

    monkeypatch.setattr(
        reflection_generator.utils,
        "get_file_fingerprint_with_old_cache",
        record_old_fingerprint,
    )
    monkeypatch.setattr(reflection_generator, "_GENERATOR_OPTIONS_HASH", "cpp-packages-v2")

    harness.generate()

    assert harness.parse_calls == harness.headers
    assert reused_fingerprints == [None, None]


def test_damaged_cache_entry_warns_reparses_and_is_replaced(reflection_harness, caplog):
    harness = reflection_harness
    harness.generate()
    damaged_entry = harness.cache_entries()[0]
    damaged_entry.write_text("{", encoding="utf-8")
    harness.delete_generated_outputs(manifest=True)
    harness.parse_calls.clear()

    with caplog.at_level(logging.WARNING):
        harness.generate()

    assert len(harness.parse_calls) == 1
    assert "Ignoring invalid persistent cache entry" in caplog.text
    assert json.loads(damaged_entry.read_text(encoding="utf-8"))["EntryKind"] == "reflection"


def test_damaged_generated_output_is_rematerialized_without_parsing(reflection_harness):
    harness = reflection_harness
    harness.generate()
    damaged_output = harness.output_dir / "Actor.gen.cpp"
    expected = damaged_output.read_bytes()
    damaged_output.write_text("damaged", encoding="utf-8")
    harness.parse_calls.clear()

    harness.generate()

    assert harness.parse_calls == []
    assert damaged_output.read_bytes() == expected


def test_interrupted_materialization_recovers_from_published_entries(reflection_harness, monkeypatch):
    harness = reflection_harness
    write_output = reflection_generator._write_reflection_output
    materialized = 0

    def interrupt_second_output(module_name, result, manifest):
        nonlocal materialized
        if materialized == 1:
            raise OSError("materialization interrupted")
        write_output(module_name, result, manifest)
        materialized += 1

    monkeypatch.setattr(reflection_generator, "_write_reflection_output", interrupt_second_output)
    with pytest.raises(OSError, match="materialization interrupted"):
        harness.generate()

    assert len(harness.cache_entries()) == 2
    assert not harness.manifest_path.exists()
    monkeypatch.setattr(reflection_generator, "_write_reflection_output", write_output)
    harness.parse_calls.clear()

    harness.generate()

    assert harness.parse_calls == []
    assert harness.manifest_path.exists()
    assert len(harness.generated_paths()) == 5


def test_cleaned_dht_outputs_rebuild_from_shared_warm_cache_without_parsing(reflection_harness):
    harness = reflection_harness
    harness.generate_all()
    cold_outputs = {path.name: path.read_bytes() for path in harness.cmake_owned_outputs()}
    cache_entries = sorted((harness.cache_root / harness.module_name).rglob("*.json"))
    cold_cache = {path.relative_to(harness.cache_root): path.read_bytes() for path in cache_entries}
    assert harness.export_parse_calls == harness.headers
    assert harness.parse_calls == harness.headers
    assert len(cache_entries) == len(harness.headers) * 2

    for path in harness.cmake_owned_outputs():
        path.unlink()
    harness.export_parse_calls.clear()
    harness.parse_calls.clear()

    harness.generate_all()

    assert harness.export_parse_calls == []
    assert harness.parse_calls == []
    assert {path.name: path.read_bytes() for path in harness.cmake_owned_outputs()} == cold_outputs
    assert {
        path.relative_to(harness.cache_root): path.read_bytes()
        for path in sorted((harness.cache_root / harness.module_name).rglob("*.json"))
    } == cold_cache
