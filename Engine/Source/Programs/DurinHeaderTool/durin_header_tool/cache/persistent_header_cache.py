from __future__ import annotations

from collections import Counter
from dataclasses import asdict, dataclass, field
from enum import Enum
import hashlib
import json
import logging
from pathlib import Path
import re

import clang.cindex

from durin_header_tool import io as utils
from durin_header_tool.model.export_info import ExportedSymbolInfo


CACHE_SCHEMA_VERSION = 2
CACHE_ENTRY_KIND_VERSION = 1
_DIGEST_PATTERN = re.compile(r"[0-9a-f]{64}")
_IDENTIFIER_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


class CachePhase(str, Enum):
    EXPORT = "export"
    REFLECTION = "reflection"


class CacheMissReason(str, Enum):
    NOT_FOUND = "not-found"
    MALFORMED = "malformed"
    SCHEMA = "schema"
    ENTRY_KIND = "entry-kind"
    TOOL_FINGERPRINT = "tool-fingerprint"
    NATIVE_PARSER = "native-parser"
    PLATFORM = "platform"
    RUNTIME_VARIANT = "runtime-variant"
    CONTEXT = "context"
    MODULE = "module"
    LOGICAL_HEADER = "logical-header"
    HEADER_CONTENT = "header-content"
    DEPENDENCY = "dependency"
    PAYLOAD = "payload"
    PAYLOAD_DIGEST = "payload-digest"


@dataclass(frozen=True)
class CacheEntryIdentity:
    tool_fingerprint: str
    native_libclang_fingerprint: str
    platform: str
    runtime_variant: str
    context_digest: str
    module: str
    logical_header: str
    header_content_digest: str
    dependency_digest: str

    def __post_init__(self) -> None:
        _require_nonempty_string(self.tool_fingerprint, "tool fingerprint")
        _require_nonempty_string(self.platform, "platform")
        _require_nonempty_string(self.runtime_variant, "runtime variant")
        _validate_identifier(self.module, "module")
        normalize_logical_header(self.logical_header)
        for field_name, digest in (
            ("native libclang fingerprint", self.native_libclang_fingerprint),
            ("context digest", self.context_digest),
            ("header content digest", self.header_content_digest),
            ("dependency digest", self.dependency_digest),
        ):
            _validate_digest(digest, field_name)


@dataclass(frozen=True)
class ExportHeaderCachePayload:
    symbols: dict[str, ExportedSymbolInfo] = field(default_factory=dict)

    def to_json(self) -> dict[str, object]:
        return {
            "Symbols": {
                qualified_name: asdict(symbol)
                for qualified_name, symbol in sorted(self.symbols.items())
            }
        }

    @classmethod
    def from_json(cls, data: object) -> ExportHeaderCachePayload:
        _require_exact_fields(data, {"Symbols"}, "export payload")
        raw_symbols = data["Symbols"]
        if not isinstance(raw_symbols, dict):
            raise ValueError("Export payload field 'Symbols' must be an object.")
        symbols: dict[str, ExportedSymbolInfo] = {}
        for qualified_name, raw_symbol in raw_symbols.items():
            if not isinstance(qualified_name, str) or not qualified_name:
                raise ValueError("Export payload symbol keys must be non-empty strings.")
            symbols[qualified_name] = _exported_symbol_from_json(raw_symbol)
            if symbols[qualified_name].QualifiedName != qualified_name:
                raise ValueError("Export payload symbol key does not match its qualified name.")
        return cls(symbols=symbols)


@dataclass(frozen=True)
class ReflectionHeaderCachePayload:
    generated_header: str
    generated_source: str
    class_count: int
    property_count: int
    resolved_symbol_dependencies: dict[str, dict[str, str]] = field(default_factory=dict)

    def to_json(self) -> dict[str, object]:
        return {
            "GeneratedHeader": self.generated_header,
            "GeneratedSource": self.generated_source,
            "ClassCount": self.class_count,
            "PropertyCount": self.property_count,
            "ResolvedSymbolDependencies": {
                symbol_name: dict(sorted(snapshot.items()))
                for symbol_name, snapshot in sorted(self.resolved_symbol_dependencies.items())
            },
        }

    @classmethod
    def from_json(cls, data: object) -> ReflectionHeaderCachePayload:
        _require_exact_fields(
            data,
            {
                "GeneratedHeader",
                "GeneratedSource",
                "ClassCount",
                "PropertyCount",
                "ResolvedSymbolDependencies",
            },
            "reflection payload",
        )
        for field_name in ("GeneratedHeader", "GeneratedSource"):
            if not isinstance(data[field_name], str):
                raise ValueError(f"Reflection payload field '{field_name}' must be a string.")
        for field_name in ("ClassCount", "PropertyCount"):
            if not _is_int(data[field_name]) or data[field_name] < 0:
                raise ValueError(f"Reflection payload field '{field_name}' must be a non-negative integer.")
        dependencies = _string_map_map(data["ResolvedSymbolDependencies"], "ResolvedSymbolDependencies")
        return cls(
            generated_header=data["GeneratedHeader"],
            generated_source=data["GeneratedSource"],
            class_count=data["ClassCount"],
            property_count=data["PropertyCount"],
            resolved_symbol_dependencies=dependencies,
        )


CachePayload = ExportHeaderCachePayload | ReflectionHeaderCachePayload


@dataclass(frozen=True)
class CacheLookupResult:
    payload: CachePayload | None = None
    miss_reason: CacheMissReason | None = None
    detail: str = ""

    @property
    def is_hit(self) -> bool:
        return self.payload is not None


@dataclass
class CacheDiagnostics:
    hits: int = 0
    misses: Counter[str] = field(default_factory=Counter)
    materialized: int = 0
    parsed: int = 0

    def record_lookup(self, result: CacheLookupResult) -> None:
        if result.is_hit:
            self.hits += 1
        elif result.miss_reason is not None:
            self.misses[result.miss_reason.value] += 1

    def record_materialization(self, count: int = 1) -> None:
        self.materialized += count

    def record_parse(self, count: int = 1) -> None:
        self.parsed += count

    def log_summary(self, module: str, phase: CachePhase, elapsed_ms: float) -> None:
        miss_summary = ",".join(f"{reason}:{count}" for reason, count in sorted(self.misses.items())) or "none"
        logging.info(
            "[DHT] %s %s cache: hits=%d misses=%d materialized=%d parsed=%d reasons=%s in %.0f ms",
            phase.value.capitalize(),
            module,
            self.hits,
            sum(self.misses.values()),
            self.materialized,
            self.parsed,
            miss_summary,
            elapsed_ms,
        )


class PersistentHeaderCache:
    def __init__(self, cache_root: Path):
        self.cache_root = cache_root.resolve()

    def entry_path(self, phase: CachePhase, module: str, logical_header: str) -> Path:
        _validate_identifier(module, "module")
        normalized_header = normalize_logical_header(logical_header)
        header_key = hashlib.sha256(normalized_header.encode("utf-8")).hexdigest()
        entry_path = self.cache_root / module / phase.value / f"{header_key}.json"
        try:
            entry_path.resolve().relative_to(self.cache_root)
        except ValueError as error:
            raise ValueError("Persistent cache entry path escapes its cache root.") from error
        return entry_path

    def read(
        self,
        phase: CachePhase,
        identity: CacheEntryIdentity,
    ) -> CacheLookupResult:
        entry_path = self.entry_path(phase, identity.module, identity.logical_header)
        if not entry_path.is_file():
            return CacheLookupResult(miss_reason=CacheMissReason.NOT_FOUND)

        try:
            data = json.loads(entry_path.read_bytes().decode("utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            return self._rejected(entry_path, CacheMissReason.MALFORMED, str(error), warning=True)

        try:
            return self._validate_entry(phase, identity, data)
        except (TypeError, ValueError, KeyError) as error:
            return self._rejected(entry_path, CacheMissReason.MALFORMED, str(error), warning=True)

    def write(
        self,
        phase: CachePhase,
        identity: CacheEntryIdentity,
        payload: CachePayload,
    ) -> Path:
        """Atomically replace one latest entry while the caller holds the module writer lock."""
        _validate_payload_phase(phase, payload)
        entry_path = self.entry_path(phase, identity.module, identity.logical_header)
        content = _encode_entry(phase, identity, payload).decode("utf-8")
        _cleanup_abandoned_temporary_files(entry_path.parent)
        utils.generate_file(entry_path, content, compare=False)
        return entry_path

    def cleanup_stale_headers(
        self,
        phase: CachePhase,
        module: str,
        current_logical_headers: list[str],
    ) -> int:
        """Remove non-current latest entries while the caller holds the module writer lock."""
        phase_dir = self.entry_path(phase, module, "placeholder.h").parent
        if not phase_dir.is_dir():
            return 0
        current_paths = {
            self.entry_path(phase, module, logical_header).name
            for logical_header in current_logical_headers
        }
        removed = 0
        for entry_path in phase_dir.glob("*.json"):
            if entry_path.name not in current_paths:
                entry_path.unlink()
                removed += 1
        removed += _cleanup_abandoned_temporary_files(phase_dir)
        if removed:
            logging.debug("[DHT] %s %s cache: removed %d stale entries", phase.value.capitalize(), module, removed)
        return removed

    def _validate_entry(
        self,
        phase: CachePhase,
        identity: CacheEntryIdentity,
        data: object,
    ) -> CacheLookupResult:
        expected_fields = {
            "SchemaVersion",
            "EntryKind",
            "EntryKindVersion",
            "ToolFingerprint",
            "NativeLibClangFingerprint",
            "Platform",
            "RuntimeVariant",
            "ContextDigest",
            "Module",
            "LogicalHeader",
            "HeaderContentDigest",
            "DependencyDigest",
            "PayloadDigest",
            "Payload",
        }
        _require_exact_fields(data, expected_fields, "cache entry")

        comparisons = (
            ("SchemaVersion", CACHE_SCHEMA_VERSION, CacheMissReason.SCHEMA),
            ("EntryKind", phase.value, CacheMissReason.ENTRY_KIND),
            ("EntryKindVersion", CACHE_ENTRY_KIND_VERSION, CacheMissReason.ENTRY_KIND),
            ("ToolFingerprint", identity.tool_fingerprint, CacheMissReason.TOOL_FINGERPRINT),
            ("NativeLibClangFingerprint", identity.native_libclang_fingerprint, CacheMissReason.NATIVE_PARSER),
            ("Platform", identity.platform, CacheMissReason.PLATFORM),
            ("RuntimeVariant", identity.runtime_variant, CacheMissReason.RUNTIME_VARIANT),
            ("ContextDigest", identity.context_digest, CacheMissReason.CONTEXT),
            ("Module", identity.module, CacheMissReason.MODULE),
            ("LogicalHeader", normalize_logical_header(identity.logical_header), CacheMissReason.LOGICAL_HEADER),
            ("HeaderContentDigest", identity.header_content_digest, CacheMissReason.HEADER_CONTENT),
            ("DependencyDigest", identity.dependency_digest, CacheMissReason.DEPENDENCY),
        )
        for field_name, expected, reason in comparisons:
            if data[field_name] != expected or type(data[field_name]) is not type(expected):
                return CacheLookupResult(miss_reason=reason, detail=field_name)

        payload_digest = _digest_json(data["Payload"])
        if data["PayloadDigest"] != payload_digest:
            return self._rejected(
                self.entry_path(phase, identity.module, identity.logical_header),
                CacheMissReason.PAYLOAD_DIGEST,
                "payload checksum disagreement",
                warning=True,
            )

        try:
            payload = (
                ExportHeaderCachePayload.from_json(data["Payload"])
                if phase is CachePhase.EXPORT
                else ReflectionHeaderCachePayload.from_json(data["Payload"])
            )
        except (TypeError, ValueError, KeyError) as error:
            return self._rejected(
                self.entry_path(phase, identity.module, identity.logical_header),
                CacheMissReason.PAYLOAD,
                str(error),
                warning=True,
            )
        return CacheLookupResult(payload=payload)

    @staticmethod
    def _rejected(
        entry_path: Path,
        reason: CacheMissReason,
        detail: str,
        *,
        warning: bool,
    ) -> CacheLookupResult:
        if warning:
            logging.warning("[DHT] Ignoring invalid persistent cache entry %s (%s: %s)", entry_path, reason.value, detail)
        return CacheLookupResult(miss_reason=reason, detail=detail)


def make_persistent_header_cache(module_name: str) -> PersistentHeaderCache:
    return PersistentHeaderCache(utils.get_module_dht_cache_root(module_name))


def normalize_logical_header(logical_header: str) -> str:
    if not isinstance(logical_header, str) or not logical_header:
        raise ValueError("Logical header must be a non-empty string.")
    normalized = logical_header.replace("\\", "/")
    if normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
        raise ValueError(f"Logical header '{logical_header}' must be relative.")
    segments = normalized.split("/")
    if any(segment in ("", ".", "..") for segment in segments):
        raise ValueError(f"Logical header '{logical_header}' contains an invalid path segment.")
    return "/".join(segments)


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
    """Hash the native binary that the installed clang.cindex binding will load."""
    return utils.calc_sha256(Path(clang.cindex.conf.get_filename()))


def _encode_entry(phase: CachePhase, identity: CacheEntryIdentity, payload: CachePayload) -> bytes:
    payload_json = payload.to_json()
    if phase is CachePhase.EXPORT:
        ExportHeaderCachePayload.from_json(payload_json)
    else:
        ReflectionHeaderCachePayload.from_json(payload_json)
    data = {
        "SchemaVersion": CACHE_SCHEMA_VERSION,
        "EntryKind": phase.value,
        "EntryKindVersion": CACHE_ENTRY_KIND_VERSION,
        "ToolFingerprint": identity.tool_fingerprint,
        "NativeLibClangFingerprint": identity.native_libclang_fingerprint,
        "Platform": identity.platform,
        "RuntimeVariant": identity.runtime_variant,
        "ContextDigest": identity.context_digest,
        "Module": identity.module,
        "LogicalHeader": normalize_logical_header(identity.logical_header),
        "HeaderContentDigest": identity.header_content_digest,
        "DependencyDigest": identity.dependency_digest,
        "PayloadDigest": _digest_json(payload_json),
        "Payload": payload_json,
    }
    return canonical_json_bytes(data)


def _digest_json(value: object) -> str:
    return sha256_bytes(canonical_json_bytes(value))


def _cleanup_abandoned_temporary_files(cache_dir: Path) -> int:
    removed = 0
    if cache_dir.is_dir():
        for temp_path in cache_dir.glob(".*.tmp"):
            temp_path.unlink()
            removed += 1
    return removed


def _validate_payload_phase(phase: CachePhase, payload: CachePayload) -> None:
    expected_type = ExportHeaderCachePayload if phase is CachePhase.EXPORT else ReflectionHeaderCachePayload
    if not isinstance(payload, expected_type):
        raise TypeError(f"Payload type '{type(payload).__name__}' is incompatible with {phase.value} cache entries.")


def _validate_identifier(value: object, field_name: str) -> None:
    if not isinstance(value, str) or _IDENTIFIER_PATTERN.fullmatch(value) is None:
        raise ValueError(f"Cache {field_name} '{value}' is not a repository identifier.")


def _validate_digest(value: object, field_name: str) -> None:
    if not isinstance(value, str) or _DIGEST_PATTERN.fullmatch(value) is None:
        raise ValueError(f"Cache {field_name} must be a lowercase SHA-256 digest.")


def _require_nonempty_string(value: object, field_name: str) -> None:
    if not isinstance(value, str) or not value:
        raise ValueError(f"Cache {field_name} must be a non-empty string.")


def _require_exact_fields(data: object, expected_fields: set[str], description: str) -> None:
    if not isinstance(data, dict) or set(data) != expected_fields:
        raise ValueError(f"The {description} has an invalid JSON object shape.")


def _is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _string_map_map(value: object, field_name: str) -> dict[str, dict[str, str]]:
    if not isinstance(value, dict):
        raise ValueError(f"Field '{field_name}' must be an object.")
    result: dict[str, dict[str, str]] = {}
    for outer_key, raw_snapshot in value.items():
        if not isinstance(outer_key, str) or not outer_key or not isinstance(raw_snapshot, dict):
            raise ValueError(f"Field '{field_name}' must contain string-keyed objects.")
        snapshot: dict[str, str] = {}
        for key, item in raw_snapshot.items():
            if not isinstance(key, str) or not key or not isinstance(item, str):
                raise ValueError(f"Field '{field_name}' snapshots must contain string pairs.")
            snapshot[key] = item
        result[outer_key] = snapshot
    return result


def _exported_symbol_from_json(data: object) -> ExportedSymbolInfo:
    expected_fields = {
        "Kind",
        "ShortName",
        "Namespace",
        "QualifiedName",
        "GeneratedHelperName",
        "Header",
        "API",
        "BaseQualifiedName",
        "IsAbstract",
        "IsScoped",
        "UnderlyingType",
        "UnderlyingKind",
        "UnderlyingSize",
    }
    _require_exact_fields(data, expected_fields, "exported symbol")
    for field_name in expected_fields - {"IsAbstract", "IsScoped", "UnderlyingSize"}:
        if not isinstance(data[field_name], str):
            raise ValueError(f"Exported symbol field '{field_name}' must be a string.")
    for field_name in ("IsAbstract", "IsScoped"):
        if not isinstance(data[field_name], bool):
            raise ValueError(f"Exported symbol field '{field_name}' must be a boolean.")
    if not _is_int(data["UnderlyingSize"]) or data["UnderlyingSize"] < 0:
        raise ValueError("Exported symbol field 'UnderlyingSize' must be a non-negative integer.")
    return ExportedSymbolInfo(**data)
