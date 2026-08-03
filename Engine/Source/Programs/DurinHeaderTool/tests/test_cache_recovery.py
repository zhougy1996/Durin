import json
import sys
from pathlib import Path
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import io as utils
from durin_header_tool.cache.reflection_cache import reflection_manifest_contract_changed
from durin_header_tool.generators import module_export_file_generator as export_generator
from durin_header_tool.generators import module_reflection_files_generator as reflection_generator
from durin_header_tool.io import FileFingerprint
from durin_header_tool.model.export_info import (
    ExportedSymbolInfo,
    ModuleExportInfo,
    ModuleExportManifest,
    load_module_export_manifest_file,
    save_module_export_manifest_file,
)
from durin_header_tool.model.reflection_manifest import ModuleManifest


class TestCacheRecovery:
    def test_file_fingerprint_content_identity_ignores_timestamp_and_size(self):
        old_fingerprint = FileFingerprint(timestamp=1.0, file_size=10, md5="same-content")
        touched_fingerprint = FileFingerprint(timestamp=2.0, file_size=10, md5="same-content")
        changed_fingerprint = FileFingerprint(timestamp=2.0, file_size=11, md5="different-content")

        assert old_fingerprint == touched_fingerprint
        assert old_fingerprint != changed_fingerprint

    def test_touched_header_keeps_export_and_reflection_cache_current(self):
        header = "Public/Engine/Actor.h"
        old_fingerprint = FileFingerprint(timestamp=1.0, file_size=10, md5="same-content")
        touched_fingerprint = FileFingerprint(timestamp=2.0, file_size=10, md5="same-content")
        old_export_manifest = ModuleExportManifest(Module="Engine")
        new_export_manifest = ModuleExportManifest(Module="Engine")
        old_export_manifest.ReflectHeaders[header] = old_fingerprint
        old_export_manifest.RawSymbolsByHeader[header] = {}
        new_export_manifest.ReflectHeaders[header] = touched_fingerprint

        assert export_generator._is_export_current(old_export_manifest, new_export_manifest, True)

        old_reflection_manifest = ModuleManifest(module_name="Engine")
        new_reflection_manifest = ModuleManifest(module_name="Engine")
        old_reflection_manifest.reflect_headers[header] = old_fingerprint
        new_reflection_manifest.reflect_headers[header] = touched_fingerprint
        old_reflection_manifest.resolved_symbol_dependencies[header] = {}
        with (
            mock.patch.object(reflection_generator, "_generated_outputs_missing", return_value=False),
            mock.patch.object(reflection_generator, "_generated_outputs_damaged", return_value=False),
        ):
            headers = reflection_generator.get_reflection_headers_requiring_regeneration(
                "Engine",
                old_reflection_manifest,
                new_reflection_manifest,
            )

        assert headers == []

    def test_unchanged_zero_symbol_header_reuses_empty_export_result(self):
        header = "Public/Engine/Empty.h"
        fingerprint = FileFingerprint(timestamp=1.0, file_size=10, md5="content")
        old_manifest = ModuleExportManifest(Module="Engine", ReflectHeaders={header: fingerprint})
        new_manifest = ModuleExportManifest(Module="Engine", ReflectHeaders={header: fingerprint})
        old_manifest.RawSymbolsByHeader[header] = {}

        symbols = export_generator._load_or_parse_header_export(
            "Engine",
            header,
            old_manifest,
            new_manifest,
        )

        assert symbols == {}

    def test_tool_fingerprint_invalidates_reflection_manifest(self):
        old_manifest = ModuleManifest(module_name="Engine", tool_fingerprint="old")
        new_manifest = ModuleManifest(module_name="Engine", tool_fingerprint="new")

        assert reflection_manifest_contract_changed(old_manifest, new_manifest)

    def test_runtime_variant_invalidates_reflection_manifest(self):
        old_manifest = ModuleManifest(module_name="Engine", runtime_variant="DurinEditor")
        new_manifest = ModuleManifest(module_name="Engine", runtime_variant="DurinGame")

        assert reflection_manifest_contract_changed(old_manifest, new_manifest)

    def test_tool_fingerprint_invalidates_export_manifest(self):
        old_manifest = ModuleExportManifest(Module="Engine", ToolFingerprint="old")
        new_manifest = ModuleExportManifest(Module="Engine", ToolFingerprint="new")

        assert not export_generator._is_export_current(old_manifest, new_manifest, True)

    def test_dependency_export_change_reresolves_reused_raw_headers(self):
        header = "Public/Engine/Actor.h"
        fingerprint = FileFingerprint(timestamp=1.0, file_size=10, md5="same-content")
        old_manifest = ModuleExportManifest(
            Module="Engine",
            DependencyExports={"CoreDObject": "old"},
            ReflectHeaders={header: fingerprint},
            RawSymbolsByHeader={header: {}},
        )
        new_manifest = ModuleExportManifest(
            Module="Engine",
            DependencyExports={"CoreDObject": "new"},
            ReflectHeaders={header: fingerprint},
        )

        assert not export_generator._is_export_current(old_manifest, new_manifest, True)
        assert export_generator._is_header_current(old_manifest, new_manifest, header)
        assert export_generator._load_or_parse_header_export(
            "Engine", header, old_manifest, new_manifest
        ) == {}

    def test_raw_header_export_projection_round_trips(self, tmp_path, monkeypatch):
        manifest_path = tmp_path / "Engine.export.manifest"
        symbol = ExportedSymbolInfo(
            Kind="class",
            ShortName="AActor",
            Namespace="Durin",
            QualifiedName="Durin::AActor",
            GeneratedHelperName="Z_Construct_DClass_Durin_AActor",
            Header="Public/Engine/Actor.h",
            API="ENGINE_API",
            BaseQualifiedName="DObject",
        )
        manifest = ModuleExportManifest(
            Module="Engine",
            DependencyExports={"CoreDObject": "digest"},
            RawSymbolsByHeader={symbol.Header: {symbol.QualifiedName: symbol}},
        )
        monkeypatch.setattr(
            utils,
            "get_module_export_manifest_file_path",
            lambda _module_name: manifest_path,
        )

        save_module_export_manifest_file(manifest)
        loaded = load_module_export_manifest_file(manifest_path)

        assert loaded.DependencyExports == manifest.DependencyExports
        assert loaded.RawSymbolsByHeader == manifest.RawSymbolsByHeader

    def test_invalid_reflection_manifest_is_a_cache_miss(self, tmp_path, monkeypatch):
        manifest_path = tmp_path / "Engine.manifest"
        manifest_path.write_text("{", encoding="utf-8")
        monkeypatch.setattr(
            reflection_generator.utils,
            "get_module_manifest_file_path",
            lambda _module_name: manifest_path,
        )
        assert reflection_generator._load_previous_manifest("Engine") is None

    def test_v3_reflection_manifest_derives_generated_output_ownership(self, tmp_path, monkeypatch):
        manifest_path = tmp_path / "Engine.manifest"
        manifest_path.write_text(
            json.dumps(
                {
                    "SchemaVersion": 3,
                    "ModuleName": "Engine",
                    "ReflectHeaders": {
                        "Public/Engine/Actor.h": {
                            "Timestamp": 0.0,
                            "FileSize": 0,
                            "MD5": "",
                        }
                    },
                    "DependencyExports": {},
                    "ResolvedSymbolDependencies": {},
                }
            ),
            encoding="utf-8",
        )
        monkeypatch.setattr(
            reflection_generator.utils,
            "get_module_manifest_file_path",
            lambda _module_name: manifest_path,
        )
        manifest = reflection_generator.load_module_manifest_file("Engine")

        assert manifest.generated_outputs == [
            "Actor.gen.cpp",
            "Actor.gen.h",
            "Engine.module.gen.cpp",
        ]
        assert manifest.pending_cleanup_outputs == []

    def test_invalid_generated_output_ownership_is_a_cache_miss(self, tmp_path, monkeypatch):
        manifest_path = tmp_path / "Engine.manifest"
        manifest_path.write_text(
            json.dumps(
                {
                    "SchemaVersion": 4,
                    "ModuleName": "Engine",
                    "ReflectHeaders": {},
                    "DependencyExports": {},
                    "ResolvedSymbolDependencies": {},
                    "GeneratedOutputs": ["../Actor.gen.h"],
                    "PendingCleanupOutputs": [],
                }
            ),
            encoding="utf-8",
        )
        monkeypatch.setattr(
            reflection_generator.utils,
            "get_module_manifest_file_path",
            lambda _module_name: manifest_path,
        )
        assert reflection_generator._load_previous_manifest("Engine") is None

    def test_invalid_export_manifest_discards_valid_export_cache(self, tmp_path, monkeypatch):
        export_path = tmp_path / "Engine.export"
        manifest_path = tmp_path / "Engine.export.manifest"
        export_path.write_text(
            json.dumps({"SchemaVersion": 4, "Module": "Engine", "Symbols": {}}),
            encoding="utf-8",
        )
        manifest_path.write_text("{", encoding="utf-8")
        monkeypatch.setattr(
            export_generator.utils,
            "get_module_export_file_path",
            lambda _module_name: export_path,
        )
        monkeypatch.setattr(
            export_generator.utils,
            "get_module_export_manifest_file_path",
            lambda _module_name: manifest_path,
        )
        assert export_generator._load_previous_export("Engine") == (None, None)

    def test_missing_generated_header_forces_regeneration(self):
        old_manifest = ModuleManifest(module_name="Engine")
        new_manifest = ModuleManifest(module_name="Engine")
        old_manifest.reflect_headers["Public/Engine/Actor.h"] = mock.sentinel.fingerprint
        new_manifest.reflect_headers["Public/Engine/Actor.h"] = mock.sentinel.fingerprint
        old_manifest.resolved_symbol_dependencies["Public/Engine/Actor.h"] = {}

        with mock.patch.object(reflection_generator, "_generated_outputs_missing", return_value=True):
            result = reflection_generator.get_reflection_headers_requiring_regeneration(
                "Engine",
                old_manifest,
                new_manifest,
            )

        assert result == ["Public/Engine/Actor.h"]

    def test_reflection_failure_does_not_commit_manifest(self):
        save_manifest = mock.Mock()
        with (
            mock.patch.object(reflection_generator, "_load_previous_manifest", return_value=None),
            mock.patch.object(
                reflection_generator,
                "make_new_module_manifest",
                return_value=ModuleManifest(module_name="Engine"),
            ),
            mock.patch.object(reflection_generator, "_write_reflection_files", side_effect=RuntimeError("generation failed")),
            mock.patch.object(reflection_generator, "make_persistent_header_cache", return_value=mock.Mock()),
            mock.patch.object(reflection_generator, "save_module_manifest_file", new=save_manifest),
        ):
            with pytest.raises(RuntimeError, match="generation failed"):
                reflection_generator.generate_reflection_files("Engine")

        save_manifest.assert_not_called()

    def test_removed_reflect_header_outputs_are_deleted_after_manifest_commit(self, tmp_path):
        old_manifest = ModuleManifest(
            module_name="Engine",
            generated_outputs=[
                "Actor.gen.cpp",
                "Actor.gen.h",
                "Engine.module.gen.cpp",
                "World.gen.cpp",
                "World.gen.h",
            ],
            pending_cleanup_outputs=["PreviouslyPending.gen.cpp"],
        )
        new_manifest = ModuleManifest(
            module_name="Engine",
            generated_outputs=[
                "Engine.module.gen.cpp",
                "World.gen.cpp",
                "World.gen.h",
            ],
        )

        output_dir = tmp_path
        stale_names = ["Actor.gen.cpp", "Actor.gen.h", "PreviouslyPending.gen.cpp"]
        for output_name in stale_names + ["World.gen.h", "Unowned.gen.h"]:
            (output_dir / output_name).write_text("generated", encoding="utf-8")

        manifest_commits = []

        def record_manifest_commit(manifest):
            manifest_commits.append(
                (
                    list(manifest.pending_cleanup_outputs),
                    [(output_dir / output_name).exists() for output_name in stale_names],
                )
            )

        with (
            mock.patch.object(reflection_generator, "_load_previous_manifest", return_value=old_manifest),
            mock.patch.object(reflection_generator, "make_new_module_manifest", return_value=new_manifest),
            mock.patch.object(
                reflection_generator,
                "get_reflection_headers_requiring_regeneration",
                return_value=[],
            ),
            mock.patch.object(reflection_generator, "_write_reflection_files", return_value=(0, 0)),
            mock.patch.object(reflection_generator, "make_persistent_header_cache", return_value=mock.Mock()),
            mock.patch.object(reflection_generator, "save_module_manifest_file", side_effect=record_manifest_commit),
            mock.patch.object(reflection_generator.utils, "get_module_dht_output_dir", return_value=output_dir),
        ):
            reflection_generator.generate_reflection_files("Engine")

        assert manifest_commits == [
            (stale_names, [True, True, True]),
            ([], [False, False, False]),
        ]
        assert (output_dir / "World.gen.h").exists()
        assert (output_dir / "Unowned.gen.h").exists()

    def test_cleanup_failure_keeps_pending_outputs_in_committed_manifest(self):
        old_manifest = ModuleManifest(
            module_name="Engine",
            generated_outputs=["Actor.gen.h", "Engine.module.gen.cpp"],
        )
        new_manifest = ModuleManifest(
            module_name="Engine",
            generated_outputs=["Engine.module.gen.cpp"],
        )
        manifest_commits = []

        with (
            mock.patch.object(reflection_generator, "_load_previous_manifest", return_value=old_manifest),
            mock.patch.object(reflection_generator, "make_new_module_manifest", return_value=new_manifest),
            mock.patch.object(
                reflection_generator,
                "get_reflection_headers_requiring_regeneration",
                return_value=[],
            ),
            mock.patch.object(reflection_generator, "_write_reflection_files", return_value=(0, 0)),
            mock.patch.object(reflection_generator, "make_persistent_header_cache", return_value=mock.Mock()),
            mock.patch.object(
                reflection_generator,
                "save_module_manifest_file",
                side_effect=lambda manifest: manifest_commits.append(list(manifest.pending_cleanup_outputs)),
            ),
            mock.patch.object(
                reflection_generator,
                "_cleanup_stale_generated_outputs",
                side_effect=OSError("cleanup failed"),
            ),
        ):
            with pytest.raises(OSError, match="cleanup failed"):
                reflection_generator.generate_reflection_files("Engine")

        assert manifest_commits == [["Actor.gen.h"]]
