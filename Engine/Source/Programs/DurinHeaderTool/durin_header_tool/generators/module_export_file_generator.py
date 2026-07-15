from concurrent.futures import ProcessPoolExecutor, as_completed
import logging
import os
import time

from durin_header_tool import config as configs
from durin_header_tool.model.export_info import (
    ModuleExportInfo,
    ModuleExportManifest,
    load_module_export_file,
    load_module_export_manifest_file,
    save_module_export_file,
    save_module_export_manifest_file,
)
from durin_header_tool.model.reflection_info import SYMBOL_NAME_SCHEME, TOOL_VERSION
from durin_header_tool import io as utils
from durin_header_tool.runtime.worker_context import initialize_worker_config


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
    module_name, header, arch, profile, build_config, build_identifier = args

    from durin_header_tool.extractors.export_symbol_extractor import _extract_header_export_symbols_impl as worker_extract

    initialize_worker_config(arch, profile, build_config, build_identifier)

    start_time = time.perf_counter()
    symbols = worker_extract(module_name, header)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    return header, symbols, elapsed_ms


def _make_current_export_manifest(module_name: str, old_manifest: ModuleExportManifest = None) -> ModuleExportManifest:
    module_config = configs.get_module_config(module_name)
    manifest = ModuleExportManifest(
        Module=module_name,
        Profile=configs.PROFILE_NAME,
        Platform=configs.ARCH,
        ToolVersion=TOOL_VERSION,
        SymbolNameScheme=SYMBOL_NAME_SCHEME,
        GeneratorOptionsHash="default",
    )
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
        and old_manifest.SymbolNameScheme == new_manifest.SymbolNameScheme
        and old_manifest.Module == new_manifest.Module
        and old_manifest.Profile == new_manifest.Profile
        and old_manifest.Platform == new_manifest.Platform
        and old_manifest.GeneratorOptionsHash == new_manifest.GeneratorOptionsHash
        and old_manifest.ReflectHeaders == new_manifest.ReflectHeaders
    )


def _is_manifest_contract_compatible(old_manifest: ModuleExportManifest, new_manifest: ModuleExportManifest) -> bool:
    if old_manifest is None:
        return False
    return (
        old_manifest.SchemaVersion == new_manifest.SchemaVersion
        and old_manifest.ToolVersion == new_manifest.ToolVersion
        and old_manifest.SymbolNameScheme == new_manifest.SymbolNameScheme
        and old_manifest.Module == new_manifest.Module
        and old_manifest.Profile == new_manifest.Profile
        and old_manifest.Platform == new_manifest.Platform
        and old_manifest.GeneratorOptionsHash == new_manifest.GeneratorOptionsHash
    )


def _is_header_current(old_manifest: ModuleExportManifest, new_manifest: ModuleExportManifest, header: str) -> bool:
    return (
        _is_manifest_contract_compatible(old_manifest, new_manifest)
        and old_manifest.ReflectHeaders.get(header) == new_manifest.ReflectHeaders[header]
    )


def _symbols_for_header_from_export(export_info: ModuleExportInfo, header: str) -> dict | None:
    if export_info is None:
        return None
    symbols = {
        qualified_name: symbol
        for qualified_name, symbol in export_info.Symbols.items()
        if symbol.Header == header
    }
    return symbols if symbols else None


def _load_or_parse_header_export(
    module_name: str,
    header: str,
    old_manifest: ModuleExportManifest,
    new_manifest: ModuleExportManifest,
    old_export_info: ModuleExportInfo,
) -> dict:
    if _is_header_current(old_manifest, new_manifest, header):
        symbols = _symbols_for_header_from_export(old_export_info, header)
        if symbols is not None:
            logging.info(
                "[DHT] Export %s: reused %s from export (%d symbols)",
                module_name,
                header,
                len(symbols),
            )
            return symbols
        logging.info("[DHT] Export %s: cache miss for unchanged %s", module_name, header)

    return None


def _build_module_export_from_manifest_cache(
    module_name: str,
    old_manifest: ModuleExportManifest,
    new_manifest: ModuleExportManifest,
    old_export_info: ModuleExportInfo,
) -> ModuleExportInfo:
    module_config = configs.get_module_config(module_name)
    export_info = ModuleExportInfo(Module=module_name)
    headers_to_parse: list[str] = []

    for header in module_config.reflect_headers:
        symbols = _load_or_parse_header_export(module_name, header, old_manifest, new_manifest, old_export_info)
        if symbols is None:
            headers_to_parse.append(header)
        else:
            export_info.Symbols.update(symbols)

    if headers_to_parse:
        worker_count = min(len(headers_to_parse), os.cpu_count() or 1, 8)
        logging.info(
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
                configs.PROFILE_NAME,
                configs.BUILD_CONFIG,
                configs.BUILD_IDENTIFIER,
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
            logging.info(
                "[DHT] Export %s: scanned %s (%d symbols) in %.0f ms",
                module_name,
                header,
                len(symbols),
                elapsed_ms,
            )

        for header in module_config.reflect_headers:
            if header in parsed_symbols_by_header:
                export_info.Symbols.update(parsed_symbols_by_header[header])

    return export_info


def generate_module_export_file(module_name):
    start_time = time.perf_counter()
    export_file_path = utils.get_module_export_file_path(module_name)
    export_manifest_path = utils.get_module_export_manifest_file_path(module_name)
    old_export_info, old_manifest = _load_previous_export(module_name)
    new_manifest = _make_current_export_manifest(module_name, old_manifest)
    if _is_export_current(old_manifest, new_manifest, old_export_info is not None):
        save_module_export_manifest_file(new_manifest)
        elapsed_ms = (time.perf_counter() - start_time) * 1000.0
        logging.info(
            "[DHT] Export %s: inputs unchanged, skipped %d headers in %.0f ms",
            module_name,
            len(new_manifest.ReflectHeaders),
            elapsed_ms,
        )
        return

    logging.info("[DHT] Export %s: scanning %d reflected headers", module_name, len(new_manifest.ReflectHeaders))
    export_info = _build_module_export_from_manifest_cache(module_name, old_manifest, new_manifest, old_export_info)
    save_module_export_file(export_info)
    save_module_export_manifest_file(new_manifest)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    logging.info("[DHT] Export %s: wrote %d symbols in %.0f ms", module_name, len(export_info.Symbols), elapsed_ms)
