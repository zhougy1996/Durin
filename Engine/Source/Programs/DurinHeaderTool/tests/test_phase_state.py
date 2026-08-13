import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool import io as utils
from durin_header_tool.cache.phase_state import (
    ExportPhaseState,
    ReflectionPhaseState,
    canonical_json_bytes,
    export_state_path,
    load_export_phase_state,
    load_reflection_phase_state,
    reflection_state_path,
    save_export_phase_state,
    save_reflection_phase_state,
    sha256_bytes,
)
from durin_header_tool.io import FileFingerprint
from durin_header_tool.generators.module_reflection_files_generator import (
    get_reflection_headers_requiring_regeneration,
)
from durin_header_tool.model.export_info import ExportedSymbolInfo
from durin_header_tool.model.generated_output import (
    generated_output_names,
    header_generated_names,
    validate_unique_reflected_header_stems,
)
from durin_header_tool.model.reflection_generation import ReflectionHeaderGenerationResult
from durin_header_tool.resolver.reflection_resolver import symbol_dependency_snapshot


HEADER = "Public/Fixture/Actor.h"
DIGEST = "a" * 64


def _symbol() -> ExportedSymbolInfo:
    return ExportedSymbolInfo(
        Kind="Class",
        ShortName="AActor",
        Namespace="Durin",
        QualifiedName="Durin::AActor",
        GeneratedHelperName="Z_Construct_DClass_Durin_AActor",
        Header=HEADER,
        API="FIXTURE_API",
        BaseQualifiedName="Durin::DObject",
    )


def _fingerprint() -> FileFingerprint:
    return FileFingerprint(timestamp=1.0, file_size=12, sha256=DIGEST)


def _result() -> ReflectionHeaderGenerationResult:
    return ReflectionHeaderGenerationResult(
        header=HEADER,
        generated_header="// header\n",
        generated_source="// source\n",
        class_count=1,
        property_count=2,
        resolved_symbol_dependencies={
            "Durin::DObject": {"API": "COREDOBJECT_API"},
        },
    )


@pytest.fixture
def isolated_state_root(tmp_path, monkeypatch):
    monkeypatch.setattr(utils, "get_module_dht_cache_root", lambda _module: tmp_path / "DHTCache")
    return tmp_path / "DHTCache"


def test_file_fingerprint_codec_uses_sha256_and_ignores_fast_path_metadata():
    original = _fingerprint()
    touched = FileFingerprint(timestamp=2.0, file_size=13, sha256=DIGEST)

    assert original == touched
    assert FileFingerprint.from_json(original.to_json()) == original
    assert set(original.to_json()) == {"Timestamp", "FileSize", "SHA256"}


def test_legacy_md5_fingerprint_is_a_stale_migration_input():
    legacy = FileFingerprint.from_json(
        {"Timestamp": 1.0, "FileSize": 12, "MD5": "legacy"},
        allow_legacy_md5=True,
    )

    assert not legacy.sha256
    assert legacy != legacy
    with pytest.raises(ValueError, match="invalid JSON object shape"):
        FileFingerprint.from_json({"Timestamp": 1.0, "FileSize": 12, "MD5": "legacy"})


def test_exported_symbol_codec_is_strict_and_round_trips():
    symbol = _symbol()
    assert ExportedSymbolInfo.from_json(symbol.to_json()) == symbol
    malformed = symbol.to_json()
    malformed["Unexpected"] = True
    with pytest.raises(ValueError, match="invalid JSON object shape"):
        ExportedSymbolInfo.from_json(malformed)


def test_generated_output_naming_is_centralized_and_collision_checked():
    assert header_generated_names(HEADER) == ("Actor.gen.h", "Actor.gen.cpp")
    assert generated_output_names("Fixture", [HEADER]) == [
        "Fixture.module.gen.cpp",
        "Actor.gen.cpp",
        "Actor.gen.h",
    ]
    with pytest.raises(ValueError, match="basenames must be unique"):
        validate_unique_reflected_header_stems(["Public/A/Actor.h", "Public/B/Actor.hpp"])


def test_export_phase_bundle_round_trips_canonical_checked_state(isolated_state_root):
    state = ExportPhaseState(
        module="Fixture",
        tool_fingerprint="tool",
        native_libclang_fingerprint=DIGEST,
        platform="Win64",
        runtime_variant="DurinEditor",
        context_digest=DIGEST,
        dependency_exports={"CoreDObject": DIGEST},
        reflect_headers={HEADER: _fingerprint()},
        raw_symbols_by_header={HEADER: {_symbol().QualifiedName: _symbol()}},
        resolved_export_digest=DIGEST,
    )

    content = save_export_phase_state(state)
    loaded = load_export_phase_state("Fixture")

    assert export_state_path("Fixture") == isolated_state_root / "Fixture" / "export-state.json"
    assert loaded == state
    assert content.encode("utf-8") == canonical_json_bytes(json.loads(content))


def test_reflection_phase_bundle_round_trips_generated_content(isolated_state_root):
    result = _result()
    state = ReflectionPhaseState(
        module="Fixture",
        tool_fingerprint="tool",
        native_libclang_fingerprint=DIGEST,
        platform="Win64",
        runtime_variant="DurinEditor",
        context_digest=DIGEST,
        reflect_headers={HEADER: _fingerprint()},
        results_by_header={HEADER: result},
        generated_outputs=["Actor.gen.cpp", "Actor.gen.h", "Fixture.module.gen.cpp"],
        generated_output_digests={"Actor.gen.h": DIGEST},
    )

    save_reflection_phase_state(state)
    loaded = load_reflection_phase_state("Fixture")

    assert reflection_state_path("Fixture") == isolated_state_root / "Fixture" / "reflection-state.json"
    assert loaded == state
    assert loaded.results_by_header[HEADER] == result


def test_corrupt_bundle_is_a_typed_module_miss(isolated_state_root, caplog):
    path = export_state_path("Fixture")
    path.parent.mkdir(parents=True)
    path.write_text('{"SchemaVersion":', encoding="utf-8")

    assert load_export_phase_state("Fixture") is None
    assert "Ignoring invalid export phase state" in caplog.text


def test_invalid_reflection_header_record_only_invalidates_that_record(isolated_state_root):
    first = _result()
    second_header = "Public/Fixture/Pawn.h"
    second = ReflectionHeaderGenerationResult(
        header=second_header,
        generated_header="// pawn header\n",
        generated_source="// pawn source\n",
        class_count=1,
        property_count=0,
    )
    state = ReflectionPhaseState(
        module="Fixture",
        tool_fingerprint="tool",
        native_libclang_fingerprint=DIGEST,
        platform="Win64",
        runtime_variant="DurinEditor",
        context_digest=DIGEST,
        reflect_headers={HEADER: _fingerprint(), second_header: _fingerprint()},
        results_by_header={HEADER: first, second_header: second},
    )
    save_reflection_phase_state(state)
    path = reflection_state_path("Fixture")
    envelope = json.loads(path.read_text(encoding="utf-8"))
    envelope["Payload"]["ResultsByHeader"][HEADER]["ClassCount"] = -1
    envelope["PayloadDigest"] = sha256_bytes(canonical_json_bytes(envelope["Payload"]))
    path.write_bytes(canonical_json_bytes(envelope))

    loaded = load_reflection_phase_state("Fixture")

    assert HEADER not in loaded.results_by_header
    assert loaded.results_by_header[second_header] == second


def test_dependency_export_change_only_invalidates_referenced_symbol_changes():
    symbol = _symbol()
    result = ReflectionHeaderGenerationResult(
        header=HEADER,
        generated_header="// header\n",
        generated_source="// source\n",
        class_count=1,
        property_count=0,
        resolved_symbol_dependencies={
            symbol.QualifiedName: symbol_dependency_snapshot(symbol),
        },
    )
    common = {
        "module": "Fixture",
        "tool_fingerprint": "tool",
        "native_libclang_fingerprint": DIGEST,
        "platform": "Win64",
        "runtime_variant": "DurinEditor",
        "context_digest": DIGEST,
        "reflect_headers": {HEADER: _fingerprint()},
    }
    old_state = ReflectionPhaseState(
        **common,
        dependency_exports={"CoreDObject": "old"},
        results_by_header={HEADER: result},
    )
    unchanged_state = ReflectionPhaseState(
        **common,
        dependency_exports={"CoreDObject": "new"},
    )

    assert get_reflection_headers_requiring_regeneration(
        "Fixture", old_state, unchanged_state, {symbol.QualifiedName: symbol}
    ) == []
    changed_symbol = ExportedSymbolInfo.from_json(
        {**symbol.to_json(), "API": "CHANGED_API"}
    )
    changed_state = ReflectionPhaseState(
        **common,
        dependency_exports={"CoreDObject": "newer"},
    )
    assert get_reflection_headers_requiring_regeneration(
        "Fixture", old_state, changed_state, {symbol.QualifiedName: changed_symbol}
    ) == [HEADER]
