from __future__ import annotations

from dataclasses import dataclass, field
import hashlib
import json
import logging
from pathlib import Path

import clang.cindex

from durin_header_tool import io as utils
from durin_header_tool.io import FileFingerprint
from durin_header_tool.model.export_info import ExportedSymbolInfo
from durin_header_tool.model.reflection_generation import ReflectionHeaderGenerationResult


PHASE_STATE_ENVELOPE_SCHEMA_VERSION = 1
EXPORT_PHASE_STATE_SCHEMA_VERSION = 1
REFLECTION_PHASE_STATE_SCHEMA_VERSION = 1


def canonical_json_bytes(value: object) -> bytes:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")


def sha256_bytes(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def fingerprint_native_libclang() -> str:
    return utils.calc_sha256(Path(clang.cindex.conf.get_filename()))


@dataclass
class ExportPhaseState:
    module: str
    schema_version: int = EXPORT_PHASE_STATE_SCHEMA_VERSION
    tool_fingerprint: str = ""
    native_libclang_fingerprint: str = ""
    platform: str = ""
    runtime_variant: str = ""
    context_digest: str = ""
    dependency_exports: dict[str, str] = field(default_factory=dict)
    reflect_headers: dict[str, FileFingerprint] = field(default_factory=dict)
    raw_symbols_by_header: dict[str, dict[str, ExportedSymbolInfo]] = field(default_factory=dict)
    resolved_export_digest: str = ""

    def to_json(self) -> dict[str, object]:
        return {
            "SchemaVersion": self.schema_version,
            "ToolFingerprint": self.tool_fingerprint,
            "NativeLibClangFingerprint": self.native_libclang_fingerprint,
            "Platform": self.platform,
            "RuntimeVariant": self.runtime_variant,
            "ContextDigest": self.context_digest,
            "Module": self.module,
            "DependencyExports": dict(sorted(self.dependency_exports.items())),
            "ReflectHeaders": {
                header: fingerprint.to_json()
                for header, fingerprint in sorted(self.reflect_headers.items())
            },
            "RawSymbolsByHeader": {
                header: {
                    qualified_name: symbol.to_json()
                    for qualified_name, symbol in sorted(symbols.items())
                }
                for header, symbols in sorted(self.raw_symbols_by_header.items())
            },
            "ResolvedExportDigest": self.resolved_export_digest,
        }

    @classmethod
    def from_json(cls, data: object) -> "ExportPhaseState":
        expected = {
            "SchemaVersion", "ToolFingerprint", "NativeLibClangFingerprint",
            "Platform", "RuntimeVariant", "ContextDigest", "Module",
            "DependencyExports", "ReflectHeaders", "RawSymbolsByHeader",
            "ResolvedExportDigest",
        }
        _require_exact_fields(data, expected, "export phase state")
        state = cls(
            module=_string(data, "Module"),
            schema_version=_integer(data, "SchemaVersion"),
            tool_fingerprint=_string(data, "ToolFingerprint"),
            native_libclang_fingerprint=_string(data, "NativeLibClangFingerprint"),
            platform=_string(data, "Platform"),
            runtime_variant=_string(data, "RuntimeVariant"),
            context_digest=_string(data, "ContextDigest"),
            dependency_exports=_string_map(data["DependencyExports"], "DependencyExports"),
            resolved_export_digest=_string(data, "ResolvedExportDigest", allow_empty=True),
        )
        raw_fingerprints = data["ReflectHeaders"]
        if not isinstance(raw_fingerprints, dict):
            raise ValueError("Export phase ReflectHeaders must be an object.")
        state.reflect_headers = {
            _nonempty_key(header, "ReflectHeaders"): FileFingerprint.from_json(raw)
            for header, raw in raw_fingerprints.items()
        }
        raw_records = data["RawSymbolsByHeader"]
        if not isinstance(raw_records, dict):
            raise ValueError("Export phase RawSymbolsByHeader must be an object.")
        for header, raw_symbols in raw_records.items():
            if not isinstance(header, str) or not header or not isinstance(raw_symbols, dict):
                continue
            try:
                symbols = {
                    _nonempty_key(name, "RawSymbolsByHeader"): ExportedSymbolInfo.from_json(raw)
                    for name, raw in raw_symbols.items()
                }
                if any(symbol.QualifiedName != name for name, symbol in symbols.items()):
                    raise ValueError("Export phase symbol key disagrees with QualifiedName.")
            except (TypeError, ValueError, KeyError):
                logging.warning("[DHT] Ignoring invalid export phase record for %s", header)
                continue
            state.raw_symbols_by_header[header] = symbols
        return state


@dataclass
class ReflectionPhaseState:
    module: str
    schema_version: int = REFLECTION_PHASE_STATE_SCHEMA_VERSION
    tool_fingerprint: str = ""
    native_libclang_fingerprint: str = ""
    platform: str = ""
    runtime_variant: str = ""
    context_digest: str = ""
    dependency_exports: dict[str, str] = field(default_factory=dict)
    reflect_headers: dict[str, FileFingerprint] = field(default_factory=dict)
    results_by_header: dict[str, ReflectionHeaderGenerationResult] = field(default_factory=dict)
    generated_outputs: list[str] = field(default_factory=list)
    generated_output_digests: dict[str, str] = field(default_factory=dict)
    pending_cleanup_outputs: list[str] = field(default_factory=list)

    def to_json(self) -> dict[str, object]:
        return {
            "SchemaVersion": self.schema_version,
            "ToolFingerprint": self.tool_fingerprint,
            "NativeLibClangFingerprint": self.native_libclang_fingerprint,
            "Platform": self.platform,
            "RuntimeVariant": self.runtime_variant,
            "ContextDigest": self.context_digest,
            "Module": self.module,
            "DependencyExports": dict(sorted(self.dependency_exports.items())),
            "ReflectHeaders": {
                header: fingerprint.to_json()
                for header, fingerprint in sorted(self.reflect_headers.items())
            },
            "ResultsByHeader": {
                header: result.to_json()
                for header, result in sorted(self.results_by_header.items())
            },
            "GeneratedOutputs": sorted(set(self.generated_outputs)),
            "GeneratedOutputDigests": dict(sorted(self.generated_output_digests.items())),
            "PendingCleanupOutputs": sorted(set(self.pending_cleanup_outputs)),
        }

    @classmethod
    def from_json(cls, data: object) -> "ReflectionPhaseState":
        expected = {
            "SchemaVersion", "ToolFingerprint", "NativeLibClangFingerprint",
            "Platform", "RuntimeVariant", "ContextDigest", "Module",
            "DependencyExports", "ReflectHeaders", "ResultsByHeader",
            "GeneratedOutputs", "GeneratedOutputDigests", "PendingCleanupOutputs",
        }
        _require_exact_fields(data, expected, "reflection phase state")
        state = cls(
            module=_string(data, "Module"),
            schema_version=_integer(data, "SchemaVersion"),
            tool_fingerprint=_string(data, "ToolFingerprint"),
            native_libclang_fingerprint=_string(data, "NativeLibClangFingerprint"),
            platform=_string(data, "Platform"),
            runtime_variant=_string(data, "RuntimeVariant"),
            context_digest=_string(data, "ContextDigest"),
            dependency_exports=_string_map(data["DependencyExports"], "DependencyExports"),
            generated_outputs=_output_names(data["GeneratedOutputs"], "GeneratedOutputs"),
            generated_output_digests=_string_map(data["GeneratedOutputDigests"], "GeneratedOutputDigests"),
            pending_cleanup_outputs=_output_names(data["PendingCleanupOutputs"], "PendingCleanupOutputs"),
        )
        raw_fingerprints = data["ReflectHeaders"]
        if not isinstance(raw_fingerprints, dict):
            raise ValueError("Reflection phase ReflectHeaders must be an object.")
        state.reflect_headers = {
            _nonempty_key(header, "ReflectHeaders"): FileFingerprint.from_json(raw)
            for header, raw in raw_fingerprints.items()
        }
        raw_results = data["ResultsByHeader"]
        if not isinstance(raw_results, dict):
            raise ValueError("Reflection phase ResultsByHeader must be an object.")
        for header, raw_result in raw_results.items():
            if not isinstance(header, str) or not header:
                continue
            try:
                result = ReflectionHeaderGenerationResult.from_json(raw_result)
                if result.header != header:
                    raise ValueError("Reflection result header disagrees with its key.")
            except (TypeError, ValueError, KeyError):
                logging.warning("[DHT] Ignoring invalid reflection phase record for %s", header)
                continue
            state.results_by_header[header] = result
        return state


def export_state_path(module_name: str) -> Path:
    return utils.get_module_dht_state_dir(module_name) / "export-state.json"


def reflection_state_path(module_name: str) -> Path:
    return utils.get_module_dht_state_dir(module_name) / "reflection-state.json"


def load_export_phase_state(module_name: str) -> ExportPhaseState | None:
    return _load_phase_state(export_state_path(module_name), "export", ExportPhaseState.from_json)


def load_reflection_phase_state(module_name: str) -> ReflectionPhaseState | None:
    return _load_phase_state(reflection_state_path(module_name), "reflection", ReflectionPhaseState.from_json)


def save_export_phase_state(state: ExportPhaseState) -> str:
    return _save_phase_state(export_state_path(state.module), "export", state.to_json())


def save_reflection_phase_state(state: ReflectionPhaseState) -> str:
    return _save_phase_state(reflection_state_path(state.module), "reflection", state.to_json())


def _load_phase_state(path: Path, kind: str, decoder):
    if not path.is_file():
        return None
    try:
        envelope = json.loads(path.read_text(encoding="utf-8"))
        _require_exact_fields(envelope, {"SchemaVersion", "Kind", "PayloadDigest", "Payload"}, "phase-state envelope")
        if envelope["SchemaVersion"] != PHASE_STATE_ENVELOPE_SCHEMA_VERSION or envelope["Kind"] != kind:
            raise ValueError("Phase-state envelope identity is incompatible.")
        if envelope["PayloadDigest"] != sha256_bytes(canonical_json_bytes(envelope["Payload"])):
            raise ValueError("Phase-state payload checksum disagreement.")
        return decoder(envelope["Payload"])
    except (OSError, UnicodeError, json.JSONDecodeError, TypeError, ValueError, KeyError) as error:
        logging.warning("[DHT] Ignoring invalid %s phase state %s (%s)", kind, path, error)
        return None


def _save_phase_state(path: Path, kind: str, payload: dict[str, object]) -> str:
    envelope = {
        "SchemaVersion": PHASE_STATE_ENVELOPE_SCHEMA_VERSION,
        "Kind": kind,
        "PayloadDigest": sha256_bytes(canonical_json_bytes(payload)),
        "Payload": payload,
    }
    content = canonical_json_bytes(envelope).decode("utf-8")
    utils.generate_file(path, content)
    return content


def _require_exact_fields(data: object, expected: set[str], description: str) -> None:
    if not isinstance(data, dict) or set(data) != expected:
        raise ValueError(f"The {description} has an invalid JSON object shape.")


def _string(data: dict[str, object], field_name: str, *, allow_empty: bool = False) -> str:
    value = data[field_name]
    if not isinstance(value, str) or (not allow_empty and not value):
        raise ValueError(f"Phase-state field '{field_name}' must be a string.")
    return value


def _integer(data: dict[str, object], field_name: str) -> int:
    value = data[field_name]
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ValueError(f"Phase-state field '{field_name}' must be a non-negative integer.")
    return value


def _nonempty_key(value: object, field_name: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"Phase-state field '{field_name}' requires non-empty string keys.")
    return value


def _string_map(value: object, field_name: str) -> dict[str, str]:
    if not isinstance(value, dict):
        raise ValueError(f"Phase-state field '{field_name}' must be an object.")
    result: dict[str, str] = {}
    for key, item in value.items():
        result[_nonempty_key(key, field_name)] = _nonempty_key(item, field_name)
    return result


def _output_names(value: object, field_name: str) -> list[str]:
    if not isinstance(value, list):
        raise ValueError(f"Phase-state field '{field_name}' must be an array.")
    outputs: list[str] = []
    for output in value:
        if not isinstance(output, str) or not output or "/" in output or "\\" in output:
            raise ValueError(f"Phase-state field '{field_name}' contains an invalid output name.")
        outputs.append(output)
    return sorted(set(outputs))
