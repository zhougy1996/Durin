from concurrent.futures import ProcessPoolExecutor, as_completed
import hashlib
import logging
import time

from durin_header_tool import config as configs
from durin_header_tool.cache.persistent_header_cache import (
    CacheDiagnostics,
    CacheEntryIdentity,
    CachePhase,
    ExportHeaderCachePayload,
    PersistentHeaderCache,
    canonical_json_bytes,
    fingerprint_native_libclang,
    make_persistent_header_cache,
    sha256_bytes,
)
from durin_header_tool.model.export_info import (
    ModuleExportInfo,
    ModuleExportManifest,
    load_module_export_file,
    load_module_export_manifest_file,
    save_module_export_file,
    save_module_export_manifest_file,
)
from durin_header_tool.model.reflection_info import SYMBOL_NAME_SCHEME, TOOL_VERSION
from durin_header_tool.parser.reflection_parser import PARSER_CONTEXT_VERSION
from durin_header_tool import io as utils
from durin_header_tool.runtime.worker_context import initialize_worker_config
from durin_header_tool.runtime.parallelism import resolve_worker_count
from durin_header_tool.resolver.reflection_resolver import load_dependency_symbols
from durin_header_tool.extractors.export_symbol_extractor import resolve_module_export_info


_GENERATOR_OPTIONS_HASH = "default"
_EMPTY_EXPORT_DEPENDENCY_DIGEST = sha256_bytes(canonical_json_bytes({}))


def _export_cache_context_digest() -> str:
    return sha256_bytes(
        canonical_json_bytes(
            {
                "GeneratorOptionsHash": _GENERATOR_OPTIONS_HASH,
                "ParserContextVersion": PARSER_CONTEXT_VERSION,
                "SymbolNameScheme": SYMBOL_NAME_SCHEME,
            }
        )
    )


def _load_previous_export(module_name: str) -> tuple[ModuleExportInfo | None, ModuleExportManifest | None]:
    export_file_path = utils.get_module_export_file_path(module_name)
    export_manifest_path = utils.get_module_export_manifest_file_path(module_name)
    old_export_info = None
    old_manifest = None

    if export_file_path.exists():
        try:
            old_export_info = load_module_export_file(export_file_path)
        except (OSError, UnicodeError, ValueError, TypeError, AttributeError, KeyError) as error:
            logging.warning(
                "[DHT] Export %s: ignoring invalid export %s (%s)",
                module_name,
                export_file_path,
                error,
            )

    if export_manifest_path.exists():
        try:
            old_manifest = load_module_export_manifest_file(export_manifest_path)
        except (OSError, UnicodeError, ValueError, TypeError, AttributeError, KeyError) as error:
            logging.warning(
                "[DHT] Export %s: ignoring invalid manifest %s (%s)",
                module_name,
                export_manifest_path,
                error,
            )

    # The export and its private manifest form one cache entry. Reuse neither
    # half when the other half is missing or invalid.
    if old_export_info is None or old_manifest is None:
        return None, None
    return old_export_info, old_manifest


def _parse_header_export_worker(args):
    module_name, header, arch, runtime_variant = args

    from durin_header_tool.extractors.export_symbol_extractor import _extract_header_export_symbols_impl as worker_extract

    initialize_worker_config(arch, runtime_variant)

    start_time = time.perf_counter()
    symbols = worker_extract(module_name, header)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    return header, symbols, elapsed_ms


def _make_current_export_manifest(module_name: str, old_manifest: ModuleExportManifest = None) -> ModuleExportManifest:
    module_config = configs.get_module_config(module_name)
    manifest = ModuleExportManifest(
        Module=module_name,
        RuntimeVariant=configs.RUNTIME_VARIANT,
        Platform=configs.ARCH,
        ToolVersion=TOOL_VERSION,
        ToolFingerprint=configs.TOOL_FINGERPRINT or TOOL_VERSION,
        SymbolNameScheme=SYMBOL_NAME_SCHEME,
        GeneratorOptionsHash=_GENERATOR_OPTIONS_HASH,
    )
    for dep_module in configs.collect_all_dependent_module_with_export_file(module_name):
        if dep_module == module_name:
            continue
        export_path = utils.get_module_export_file_path(dep_module)
        if not export_path.exists():
            raise FileNotFoundError(
                f"Export file for dependent module '{dep_module}' not found at expected path: {export_path}"
            )
        manifest.DependencyExports[dep_module] = hashlib.sha256(export_path.read_bytes()).hexdigest()
    for header in module_config.reflect_headers:
        header_file_path = (module_config.module_dir / header).resolve()
        if not header_file_path.exists():
            raise FileNotFoundError(f"Reflect header file '{header}' for module '{module_name}' not found at expected path: {header_file_path}")
        old_fingerprint = old_manifest.ReflectHeaders.get(header) if old_manifest else None
        manifest.ReflectHeaders[header] = utils.get_file_fingerprint_with_old_cache(header_file_path, old_fingerprint)
    return manifest


def _is_export_current(old_manifest: ModuleExportManifest, new_manifest: ModuleExportManifest, export_exists: bool) -> bool:
    if old_manifest is None or not export_exists:
        return False
    return (
        old_manifest.SchemaVersion == new_manifest.SchemaVersion
        and old_manifest.ToolVersion == new_manifest.ToolVersion
        and old_manifest.ToolFingerprint == new_manifest.ToolFingerprint
        and old_manifest.SymbolNameScheme == new_manifest.SymbolNameScheme
        and old_manifest.Module == new_manifest.Module
        and old_manifest.RuntimeVariant == new_manifest.RuntimeVariant
        and old_manifest.Platform == new_manifest.Platform
        and old_manifest.GeneratorOptionsHash == new_manifest.GeneratorOptionsHash
        and old_manifest.DependencyExports == new_manifest.DependencyExports
        and old_manifest.ReflectHeaders == new_manifest.ReflectHeaders
        and set(old_manifest.RawSymbolsByHeader) == set(new_manifest.ReflectHeaders)
    )


def _is_manifest_contract_compatible(old_manifest: ModuleExportManifest, new_manifest: ModuleExportManifest) -> bool:
    if old_manifest is None:
        return False
    return (
        old_manifest.SchemaVersion == new_manifest.SchemaVersion
        and old_manifest.ToolVersion == new_manifest.ToolVersion
        and old_manifest.ToolFingerprint == new_manifest.ToolFingerprint
        and old_manifest.SymbolNameScheme == new_manifest.SymbolNameScheme
        and old_manifest.Module == new_manifest.Module
        and old_manifest.RuntimeVariant == new_manifest.RuntimeVariant
        and old_manifest.Platform == new_manifest.Platform
        and old_manifest.GeneratorOptionsHash == new_manifest.GeneratorOptionsHash
    )


def _is_header_current(old_manifest: ModuleExportManifest, new_manifest: ModuleExportManifest, header: str) -> bool:
    return (
        _is_manifest_contract_compatible(old_manifest, new_manifest)
        and old_manifest.ReflectHeaders.get(header) == new_manifest.ReflectHeaders[header]
    )


def _load_or_parse_header_export(
    module_name: str,
    header: str,
    old_manifest: ModuleExportManifest,
    new_manifest: ModuleExportManifest,
) -> dict:
    if _is_header_current(old_manifest, new_manifest, header):
        symbols = old_manifest.RawSymbolsByHeader.get(header)
        if symbols is not None:
            logging.debug(
                "[DHT] Export %s: reused %s from export (%d symbols)",
                module_name,
                header,
                len(symbols),
            )
            return symbols
        logging.debug("[DHT] Export %s: cache miss for unchanged %s", module_name, header)

    return None


def _make_export_cache_identity(
    module_name: str,
    header: str,
    native_libclang_fingerprint: str,
) -> CacheEntryIdentity:
    module_config = configs.get_module_config(module_name)
    header_path = (module_config.module_dir / header).resolve()
    return CacheEntryIdentity(
        tool_fingerprint=configs.TOOL_FINGERPRINT or TOOL_VERSION,
        native_libclang_fingerprint=native_libclang_fingerprint,
        platform=configs.ARCH,
        runtime_variant=configs.RUNTIME_VARIANT,
        context_digest=_export_cache_context_digest(),
        module=module_name,
        logical_header=header,
        header_content_digest=utils.calc_sha256(header_path),
        dependency_digest=_EMPTY_EXPORT_DEPENDENCY_DIGEST,
    )


def _build_module_export_from_cache(
    module_name: str,
    old_manifest: ModuleExportManifest,
    new_manifest: ModuleExportManifest,
    max_workers: int,
    persistent_cache: PersistentHeaderCache,
    native_libclang_fingerprint: str,
    diagnostics: CacheDiagnostics,
) -> tuple[dict[str, dict], int]:
    module_config = configs.get_module_config(module_name)
    raw_symbols_by_header: dict[str, dict] = {}
    headers_to_parse: list[str] = []
    cache_identities: dict[str, CacheEntryIdentity] = {}

    for header in module_config.reflect_headers:
        symbols = _load_or_parse_header_export(module_name, header, old_manifest, new_manifest)
        if symbols is None:
            identity = _make_export_cache_identity(
                module_name,
                header,
                native_libclang_fingerprint,
            )
            cache_identities[header] = identity
            lookup = persistent_cache.read(CachePhase.EXPORT, identity)
            diagnostics.record_lookup(lookup)
            if lookup.is_hit:
                payload = lookup.payload
                if not isinstance(payload, ExportHeaderCachePayload):
                    raise TypeError("Persistent export cache returned an incompatible payload.")
                symbols = payload.symbols
                diagnostics.record_materialization()
                logging.debug(
                    "[DHT] Export %s: reconstructed %s from persistent cache (%d symbols)",
                    module_name,
                    header,
                    len(symbols),
                )
            else:
                headers_to_parse.append(header)

        if symbols is not None:
            raw_symbols_by_header[header] = symbols

    if headers_to_parse:
        worker_count = resolve_worker_count(len(headers_to_parse), max_workers)
        logging.debug(
            "[DHT] Export %s: parsing %d headers with %d workers",
            module_name,
            len(headers_to_parse),
            worker_count,
        )
        parsed_symbols_by_header = {}
        worker_args = [
            (
                module_name,
                header,
                configs.ARCH,
                configs.RUNTIME_VARIANT,
            )
            for header in headers_to_parse
        ]
        if worker_count == 1:
            results = [_parse_header_export_worker(args) for args in worker_args]
        else:
            with ProcessPoolExecutor(max_workers=worker_count) as executor:
                futures = [executor.submit(_parse_header_export_worker, args) for args in worker_args]
                results = [future.result() for future in as_completed(futures)]

        for header, symbols, elapsed_ms in sorted(results, key=lambda result: module_config.reflect_headers.index(result[0])):
            parsed_symbols_by_header[header] = symbols
            logging.debug(
                "[DHT] Export %s: scanned %s (%d symbols) in %.0f ms",
                module_name,
                header,
                len(symbols),
                elapsed_ms,
            )

        diagnostics.record_parse(len(parsed_symbols_by_header))
        for header, symbols in parsed_symbols_by_header.items():
            persistent_cache.write(
                CachePhase.EXPORT,
                cache_identities[header],
                ExportHeaderCachePayload(symbols=symbols),
            )

        for header in module_config.reflect_headers:
            if header in parsed_symbols_by_header:
                raw_symbols_by_header[header] = parsed_symbols_by_header[header]

    ordered_symbols_by_header = {
        header: raw_symbols_by_header[header]
        for header in module_config.reflect_headers
    }
    return ordered_symbols_by_header, len(headers_to_parse)


def generate_module_export_file(module_name: str, max_workers: int = 1) -> None:
    start_time = time.perf_counter()
    export_file_path = utils.get_module_export_file_path(module_name)
    export_manifest_path = utils.get_module_export_manifest_file_path(module_name)
    old_export_info, old_manifest = _load_previous_export(module_name)
    new_manifest = _make_current_export_manifest(module_name, old_manifest)
    persistent_cache = make_persistent_header_cache(module_name)
    persistent_cache.cleanup_stale_headers(
        CachePhase.EXPORT,
        module_name,
        list(new_manifest.ReflectHeaders),
    )
    if _is_export_current(old_manifest, new_manifest, old_export_info is not None):
        new_manifest.RawSymbolsByHeader = old_manifest.RawSymbolsByHeader
        save_module_export_manifest_file(new_manifest)
        elapsed_ms = (time.perf_counter() - start_time) * 1000.0
        logging.info(
            "[DHT] Export %s: up to date (%d headers, %d symbols) in %.0f ms",
            module_name,
            len(new_manifest.ReflectHeaders),
            len(old_export_info.Symbols),
            elapsed_ms,
        )
        return

    logging.debug("[DHT] Export %s: scanning %d reflected headers", module_name, len(new_manifest.ReflectHeaders))
    diagnostics = CacheDiagnostics()
    raw_symbols_by_header, parsed_header_count = _build_module_export_from_cache(
        module_name,
        old_manifest,
        new_manifest,
        max_workers,
        persistent_cache,
        fingerprint_native_libclang(),
        diagnostics,
    )
    dependency_symbols = load_dependency_symbols(module_name)
    export_info = resolve_module_export_info(
        module_name,
        raw_symbols_by_header,
        dependency_symbols,
    )
    new_manifest.RawSymbolsByHeader = raw_symbols_by_header
    save_module_export_file(export_info)
    save_module_export_manifest_file(new_manifest)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    diagnostics.log_summary(module_name, CachePhase.EXPORT, elapsed_ms)
    logging.info(
        "[DHT] Export %s: updated %d/%d headers, wrote %d symbols in %.0f ms",
        module_name,
        parsed_header_count,
        len(new_manifest.ReflectHeaders),
        len(export_info.Symbols),
        elapsed_ms,
    )
