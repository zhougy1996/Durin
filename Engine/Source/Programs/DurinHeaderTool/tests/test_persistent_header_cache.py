import json
import logging
from pathlib import Path
import sys
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool.cache.persistent_header_cache import (
    CACHE_SCHEMA_VERSION,
    CacheDiagnostics,
    CacheEntryIdentity,
    CacheMissReason,
    CachePhase,
    ExportHeaderCachePayload,
    PersistentHeaderCache,
    ReflectionHeaderCachePayload,
    canonical_json_bytes,
    fingerprint_native_libclang,
    normalize_logical_header,
    sha256_bytes,
)
from durin_header_tool.model.export_info import ExportedSymbolInfo


def _digest(label: str) -> str:
    return sha256_bytes(label.encode("utf-8"))


def _identity(**overrides) -> CacheEntryIdentity:
    values = {
        "tool_fingerprint": "tool-fixture",
        "native_libclang_fingerprint": _digest("libclang"),
        "platform": "Win64",
        "runtime_variant": "DurinEditor",
        "context_digest": _digest("hermetic-v1"),
        "module": "Engine",
        "logical_header": "Public/Engine/Actor.h",
        "header_content_digest": _digest("header"),
        "dependency_digest": _digest("dependencies"),
    }
    values.update(overrides)
    return CacheEntryIdentity(**values)


def _symbol(qualified_name: str) -> ExportedSymbolInfo:
    short_name = qualified_name.rsplit("::", 1)[-1]
    return ExportedSymbolInfo(
        Kind="Class",
        ShortName=short_name,
        Namespace="Durin",
        QualifiedName=qualified_name,
        GeneratedHelperName=f"Z_Construct_DClass_{short_name}",
        Header="Public/Engine/Actor.h",
        API="ENGINE_API",
        BaseQualifiedName="Durin::DObject",
    )


class TestPersistentHeaderCacheRoundTrip:
    def test_export_entry_round_trips_with_canonical_ordering(self, tmp_path):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        identity = _identity()
        payload = ExportHeaderCachePayload(
            symbols={
                "Durin::ZActor": _symbol("Durin::ZActor"),
                "Durin::AActor": _symbol("Durin::AActor"),
            }
        )

        entry_path = cache.write(CachePhase.EXPORT, identity, payload)
        first_bytes = entry_path.read_bytes()
        entry_data = json.loads(first_bytes)
        result = cache.read(CachePhase.EXPORT, identity)

        assert result.is_hit
        assert result.payload == payload
        assert entry_data["SchemaVersion"] == CACHE_SCHEMA_VERSION
        assert "EntryDigest" not in entry_data
        assert first_bytes == canonical_json_bytes(json.loads(first_bytes))

        reversed_payload = ExportHeaderCachePayload(symbols=dict(reversed(list(payload.symbols.items()))))
        cache.write(CachePhase.EXPORT, identity, reversed_payload)
        assert entry_path.read_bytes() == first_bytes

    def test_reflection_entry_round_trips_counts_outputs_and_dependencies(self, tmp_path):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        identity = _identity(dependency_digest=_digest("available exports"))
        payload = ReflectionHeaderCachePayload(
            generated_header="// header\n",
            generated_source="// source\n",
            class_count=2,
            property_count=3,
            resolved_symbol_dependencies={
                "Durin::DObject": {"Kind": "Class", "Module": "CoreDObject"},
            },
        )

        cache.write(CachePhase.REFLECTION, identity, payload)
        result = cache.read(CachePhase.REFLECTION, identity)

        assert result.is_hit
        assert result.payload == payload


class TestPersistentHeaderCacheValidation:
    @pytest.mark.parametrize(
        ("field_name", "replacement", "reason"),
        [
            ("SchemaVersion", CACHE_SCHEMA_VERSION + 1, CacheMissReason.SCHEMA),
            ("ToolFingerprint", "another-tool", CacheMissReason.TOOL_FINGERPRINT),
            ("RuntimeVariant", "DurinGame", CacheMissReason.RUNTIME_VARIANT),
            ("Platform", "MacOS", CacheMissReason.PLATFORM),
            ("NativeLibClangFingerprint", _digest("another clang"), CacheMissReason.NATIVE_PARSER),
        ],
    )
    def test_incompatible_common_metadata_is_a_typed_miss(self, tmp_path, field_name, replacement, reason):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        identity = _identity()
        entry_path = cache.write(CachePhase.EXPORT, identity, ExportHeaderCachePayload())
        data = json.loads(entry_path.read_text(encoding="utf-8"))
        data[field_name] = replacement
        entry_path.write_bytes(canonical_json_bytes(data))

        result = cache.read(CachePhase.EXPORT, identity)

        assert not result.is_hit
        assert result.miss_reason is reason
        assert result.detail == field_name

    def test_truncated_entry_warns_and_becomes_a_miss(self, tmp_path, caplog):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        identity = _identity()
        entry_path = cache.entry_path(CachePhase.EXPORT, identity.module, identity.logical_header)
        entry_path.parent.mkdir(parents=True)
        entry_path.write_text('{"SchemaVersion":', encoding="utf-8")

        with caplog.at_level(logging.WARNING):
            result = cache.read(CachePhase.EXPORT, identity)

        assert result.miss_reason is CacheMissReason.MALFORMED
        assert "Ignoring invalid persistent cache entry" in caplog.text

    def test_payload_checksum_disagreement_warns_and_becomes_a_miss(self, tmp_path, caplog):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        identity = _identity()
        entry_path = cache.write(CachePhase.EXPORT, identity, ExportHeaderCachePayload())
        data = json.loads(entry_path.read_text(encoding="utf-8"))
        data["Payload"]["Symbols"]["Durin::Actor"] = {
            "Kind": "Class",
        }
        entry_path.write_bytes(canonical_json_bytes(data))

        with caplog.at_level(logging.WARNING):
            result = cache.read(CachePhase.EXPORT, identity)

        assert result.miss_reason is CacheMissReason.PAYLOAD_DIGEST
        assert "payload-digest" in caplog.text

    def test_previous_entry_shape_is_rejected_as_malformed(self, tmp_path, caplog):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        identity = _identity()
        entry_path = cache.write(CachePhase.EXPORT, identity, ExportHeaderCachePayload())
        data = json.loads(entry_path.read_text(encoding="utf-8"))
        data["SchemaVersion"] = CACHE_SCHEMA_VERSION - 1
        data["EntryDigest"] = _digest("legacy entry")
        entry_path.write_bytes(canonical_json_bytes(data))

        with caplog.at_level(logging.WARNING):
            result = cache.read(CachePhase.EXPORT, identity)

        assert result.miss_reason is CacheMissReason.MALFORMED
        assert "invalid JSON object shape" in result.detail
        assert "malformed" in caplog.text

    @pytest.mark.parametrize(
        "logical_header",
        [
            "",
            "/absolute/Actor.h",
            "C:/absolute/Actor.h",
            "../Actor.h",
            "Public/../Actor.h",
            "Public//Actor.h",
            "./Actor.h",
        ],
    )
    def test_invalid_logical_headers_cannot_escape_cache_root(self, tmp_path, logical_header):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        with pytest.raises(ValueError):
            cache.entry_path(CachePhase.EXPORT, "Engine", logical_header)

    def test_normalization_preserves_declared_case_and_normalizes_separators(self):
        assert normalize_logical_header(r"Public\Engine\Actor.h") == "Public/Engine/Actor.h"
        assert normalize_logical_header("public/engine/actor.h") != "Public/Engine/Actor.h"

    def test_module_name_is_not_an_unchecked_path_component(self, tmp_path):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        with pytest.raises(ValueError, match="repository identifier"):
            cache.entry_path(CachePhase.EXPORT, "../Engine", "Public/Actor.h")


class TestPersistentHeaderCachePublication:
    def test_interrupted_replacement_preserves_previous_complete_entry(self, tmp_path):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        identity = _identity()
        old_payload = ExportHeaderCachePayload(symbols={"Durin::Actor": _symbol("Durin::Actor")})
        new_payload = ExportHeaderCachePayload(symbols={"Durin::Pawn": _symbol("Durin::Pawn")})
        entry_path = cache.write(CachePhase.EXPORT, identity, old_payload)
        old_bytes = entry_path.read_bytes()

        with mock.patch("durin_header_tool.io.file_helper.os.replace", side_effect=OSError("replace interrupted")):
            with pytest.raises(OSError, match="replace interrupted"):
                cache.write(CachePhase.EXPORT, identity, new_payload)

        assert entry_path.read_bytes() == old_bytes
        assert cache.read(CachePhase.EXPORT, identity).payload == old_payload
        assert list(entry_path.parent.glob(f".{entry_path.name}.*.tmp")) == []

    def test_cleanup_bounds_entries_to_current_header_set(self, tmp_path):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        current = _identity(logical_header="Public/Engine/Actor.h")
        stale = _identity(logical_header="Public/Engine/Removed.h")
        cache.write(CachePhase.EXPORT, current, ExportHeaderCachePayload())
        stale_path = cache.write(CachePhase.EXPORT, stale, ExportHeaderCachePayload())
        abandoned_temp = stale_path.parent / f".{stale_path.name}.abandoned.tmp"
        abandoned_temp.write_text("partial", encoding="utf-8")

        removed = cache.cleanup_stale_headers(
            CachePhase.EXPORT,
            "Engine",
            ["Public/Engine/Actor.h"],
        )

        assert removed == 2
        assert not stale_path.exists()
        assert not abandoned_temp.exists()
        assert cache.read(CachePhase.EXPORT, current).is_hit

    def test_invalid_typed_payload_is_rejected_before_publication(self, tmp_path):
        cache = PersistentHeaderCache(tmp_path / "DHTCache")
        identity = _identity()
        invalid_payload = ReflectionHeaderCachePayload(
            generated_header="",
            generated_source="",
            class_count=-1,
            property_count=0,
        )

        with pytest.raises(ValueError, match="non-negative integer"):
            cache.write(CachePhase.REFLECTION, identity, invalid_payload)

        assert not cache.entry_path(CachePhase.REFLECTION, identity.module, identity.logical_header).exists()


def test_native_libclang_fingerprint_uses_binary_content(tmp_path):
    libclang_path = tmp_path / "libclang.dll"
    libclang_path.write_bytes(b"native parser bytes")

    with mock.patch(
        "durin_header_tool.cache.persistent_header_cache.clang.cindex.conf.get_filename",
        return_value=libclang_path,
    ):
        assert fingerprint_native_libclang() == _digest("native parser bytes")


def test_cache_diagnostics_emit_one_aggregate_info_record(caplog):
    diagnostics = CacheDiagnostics()
    diagnostics.record_lookup(mock.Mock(is_hit=True, miss_reason=None))
    diagnostics.record_lookup(mock.Mock(is_hit=False, miss_reason=CacheMissReason.HEADER_CONTENT))
    diagnostics.record_materialization(1)
    diagnostics.record_parse(2)

    with caplog.at_level(logging.INFO):
        diagnostics.log_summary("Engine", CachePhase.REFLECTION, 12.5)

    assert "hits=1 misses=1 materialized=1 parsed=2" in caplog.text
    assert "reasons=header-content:1" in caplog.text
