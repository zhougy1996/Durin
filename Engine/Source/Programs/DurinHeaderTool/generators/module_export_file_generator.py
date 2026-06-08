from concurrent.futures import ProcessPoolExecutor, as_completed
import logging
import os
import time

import configs
from models.export_infos import (
    ModuleExportInfo,
    ModuleExportManifest,
    load_module_export_file,
    load_module_export_manifest_file,
    save_module_export_file,
    save_module_export_manifest_file,
)
from models.reflection_info import SYMBOL_NAME_SCHEME, TOOL_VERSION
import utils


def _parse_header_export_worker(args):
    module_name, header, arch, profile = args

    import configs as worker_configs
    from extractors.module_export_info_extractor import _extract_header_export_symbols_impl as worker_extract

    worker_configs.ARCH = arch
    worker_configs.PROFILE_NAME = profile
    worker_configs.init_configs()

    start_time = time.perf_counter()
    symbols = worker_extract(module_name, header)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    return header, symbols, elapsed_ms


def _make_current_export_manifest(module_name: str, old_manifest: ModuleExportManifest = None) -> ModuleExportManifest:
    module_config = configs.get_module_config(module_name)
    manifest = ModuleExportManifest(
        module=module_name,
        profile=configs.PROFILE_NAME,
        platform=configs.ARCH,
        toolVersion=TOOL_VERSION,
        symbolNameScheme=SYMBOL_NAME_SCHEME,
        generatorOptionsHash="default",
    )
    for header in module_config.reflect_headers:
        header_file_path = (module_config.module_dir / header).resolve()
        if not header_file_path.exists():
            raise FileNotFoundError(f"Reflect header file '{header}' for module '{module_name}' not found at expected path: {header_file_path}")
        old_fingerprint = old_manifest.reflectHeaders.get(header) if old_manifest else None
        manifest.reflectHeaders[header] = utils.get_file_fingerprint_with_old_cache(header_file_path, old_fingerprint)
    return manifest


def _is_export_current(old_manifest: ModuleExportManifest, new_manifest: ModuleExportManifest, export_exists: bool) -> bool:
    if old_manifest is None or not export_exists:
        return False
    return (
        old_manifest.schemaVersion == new_manifest.schemaVersion
        and old_manifest.toolVersion == new_manifest.toolVersion
        and old_manifest.symbolNameScheme == new_manifest.symbolNameScheme
        and old_manifest.module == new_manifest.module
        and old_manifest.profile == new_manifest.profile
        and old_manifest.platform == new_manifest.platform
        and old_manifest.generatorOptionsHash == new_manifest.generatorOptionsHash
        and old_manifest.reflectHeaders == new_manifest.reflectHeaders
    )


def _is_manifest_contract_compatible(old_manifest: ModuleExportManifest, new_manifest: ModuleExportManifest) -> bool:
    if old_manifest is None:
        return False
    return (
        old_manifest.schemaVersion == new_manifest.schemaVersion
        and old_manifest.toolVersion == new_manifest.toolVersion
        and old_manifest.symbolNameScheme == new_manifest.symbolNameScheme
        and old_manifest.module == new_manifest.module
        and old_manifest.profile == new_manifest.profile
        and old_manifest.platform == new_manifest.platform
        and old_manifest.generatorOptionsHash == new_manifest.generatorOptionsHash
    )


def _is_header_current(old_manifest: ModuleExportManifest, new_manifest: ModuleExportManifest, header: str) -> bool:
    return (
        _is_manifest_contract_compatible(old_manifest, new_manifest)
        and old_manifest.reflectHeaders.get(header) == new_manifest.reflectHeaders[header]
    )


def _symbols_for_header_from_export(export_info: ModuleExportInfo, header: str) -> dict | None:
    if export_info is None:
        return None
    symbols = {
        qualified_name: symbol
        for qualified_name, symbol in export_info.symbols.items()
        if symbol.header == header
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
        symbols = None
        if old_manifest and header in old_manifest.headerSymbols:
            symbols = old_manifest.headerSymbols[header]
        else:
            symbols = _symbols_for_header_from_export(old_export_info, header)
        if symbols is not None:
            new_manifest.headerSymbols[header] = symbols
            logging.info(
                "[DHT] Export %s: reused %s from manifest (%d symbols)",
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
    export_info = ModuleExportInfo(module=module_name)
    headers_to_parse: list[str] = []

    for header in module_config.reflect_headers:
        symbols = _load_or_parse_header_export(module_name, header, old_manifest, new_manifest, old_export_info)
        if symbols is None:
            headers_to_parse.append(header)
        else:
            export_info.symbols.update(symbols)

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
            (module_name, header, configs.ARCH, configs.PROFILE_NAME)
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
            new_manifest.headerSymbols[header] = symbols
            logging.info(
                "[DHT] Export %s: scanned %s (%d symbols) in %.0f ms",
                module_name,
                header,
                len(symbols),
                elapsed_ms,
            )

        for header in module_config.reflect_headers:
            if header in parsed_symbols_by_header:
                export_info.symbols.update(parsed_symbols_by_header[header])

    return export_info


def generate_module_export_file(module_name):
    start_time = time.perf_counter()
    export_file_path = utils.get_module_export_file_path(module_name)
    export_manifest_path = utils.get_module_export_manifest_file_path(module_name)
    old_export_info = load_module_export_file(export_file_path) if export_file_path.exists() else None
    old_manifest = load_module_export_manifest_file(export_manifest_path) if export_manifest_path.exists() else None
    new_manifest = _make_current_export_manifest(module_name, old_manifest)
    if _is_export_current(old_manifest, new_manifest, export_file_path.exists()):
        elapsed_ms = (time.perf_counter() - start_time) * 1000.0
        logging.info(
            "[DHT] Export %s: inputs unchanged, skipped %d headers in %.0f ms",
            module_name,
            len(new_manifest.reflectHeaders),
            elapsed_ms,
        )
        return

    logging.info("[DHT] Export %s: scanning %d reflected headers", module_name, len(new_manifest.reflectHeaders))
    export_info = _build_module_export_from_manifest_cache(module_name, old_manifest, new_manifest, old_export_info)
    save_module_export_file(export_info)
    save_module_export_manifest_file(new_manifest)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    logging.info("[DHT] Export %s: wrote %d symbols in %.0f ms", module_name, len(export_info.symbols), elapsed_ms)
