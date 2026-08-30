from concurrent.futures import ProcessPoolExecutor, as_completed
import hashlib
import logging
import time

from durin_header_tool import config as configs
from durin_header_tool.cache.phase_state import (
    ExportPhaseState,
    canonical_json_bytes,
    fingerprint_native_libclang,
    load_export_phase_state,
    save_export_phase_state,
    sha256_bytes,
)
from durin_header_tool.model.export_info import (
    ModuleExportSchemaMismatchError,
    ModuleExportInfo,
    load_module_export_file,
    save_module_export_file,
)
from durin_header_tool.model.reflection_info import SYMBOL_NAME_SCHEME, TOOL_VERSION
from durin_header_tool.parser.reflection_parser import PARSER_CONTEXT_VERSION
from durin_header_tool import io as utils
from durin_header_tool.runtime.worker_context import initialize_worker_config
from durin_header_tool.runtime.parallelism import resolve_worker_count
from durin_header_tool.resolver.reflection_resolver import load_dependency_symbols
from durin_header_tool.extractors.export_symbol_extractor import resolve_module_export_info


_GENERATOR_OPTIONS_HASH = "namespace-scoped-v2"
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


def _load_previous_export(module_name: str) -> tuple[ModuleExportInfo | None, ExportPhaseState | None]:
    export_file_path = utils.get_module_export_file_path(module_name)
    old_export_info = None

    if export_file_path.exists():
        try:
            old_export_info = load_module_export_file(export_file_path)
        except ModuleExportSchemaMismatchError as error:
            logging.debug(
                "[DHT] Export %s: ignoring incompatible export schema in %s (found %r, expected %d)",
                module_name,
                export_file_path,
                error.found,
                error.expected,
            )
        except (OSError, UnicodeError, ValueError, TypeError, AttributeError, KeyError) as error:
            logging.warning(
                "[DHT] Export %s: ignoring invalid export %s (%s)",
                module_name,
                export_file_path,
                error,
            )

    return old_export_info, load_export_phase_state(module_name)


def _parse_header_export_worker(args):
    module_name, header = args

    from durin_header_tool.extractors.export_symbol_extractor import _extract_header_export_symbols_impl as worker_extract

    start_time = time.perf_counter()
    symbols = worker_extract(module_name, header)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    return header, symbols, elapsed_ms


def _make_current_export_state(module_name: str, old_state: ExportPhaseState = None) -> ExportPhaseState:
    module_config = configs.get_module_config(module_name)
    state = ExportPhaseState(
        module=module_name,
        runtime_variant=configs.RUNTIME_VARIANT,
        platform=configs.ARCH,
        tool_fingerprint=configs.TOOL_FINGERPRINT or TOOL_VERSION,
        native_libclang_fingerprint=fingerprint_native_libclang(
            configs.NATIVE_LIBCLANG_FINGERPRINT
        ),
        context_digest=_export_cache_context_digest(),
    )
    for dep_module in configs.collect_all_dependent_module_with_export_file(module_name):
        if dep_module == module_name:
            continue
        export_path = utils.get_module_export_file_path(dep_module)
        if not export_path.exists():
            raise FileNotFoundError(
                f"Export file for dependent module '{dep_module}' not found at expected path: {export_path}"
            )
        state.dependency_exports[dep_module] = hashlib.sha256(export_path.read_bytes()).hexdigest()
    for header in module_config.reflect_headers:
        header_file_path = (module_config.module_dir / header).resolve()
        if not header_file_path.exists():
            raise FileNotFoundError(f"Reflect header file '{header}' for module '{module_name}' not found at expected path: {header_file_path}")
        old_fingerprint = old_state.reflect_headers.get(header) if old_state else None
        state.reflect_headers[header] = utils.get_file_fingerprint_with_old_cache(header_file_path, old_fingerprint)
    return state


def _is_export_current(old_state: ExportPhaseState, new_state: ExportPhaseState, export_exists: bool) -> bool:
    if old_state is None or not export_exists:
        return False
    return (
        _is_state_contract_compatible(old_state, new_state)
        and old_state.dependency_exports == new_state.dependency_exports
        and old_state.reflect_headers == new_state.reflect_headers
        and set(old_state.raw_symbols_by_header) == set(new_state.reflect_headers)
        and old_state.resolved_export_digest == utils.calc_sha256(utils.get_module_export_file_path(new_state.module))
    )


def _is_state_contract_compatible(old_state: ExportPhaseState, new_state: ExportPhaseState) -> bool:
    if old_state is None:
        return False
    return (
        old_state.schema_version == new_state.schema_version
        and old_state.tool_fingerprint == new_state.tool_fingerprint
        and old_state.native_libclang_fingerprint == new_state.native_libclang_fingerprint
        and old_state.module == new_state.module
        and old_state.runtime_variant == new_state.runtime_variant
        and old_state.platform == new_state.platform
        and old_state.context_digest == new_state.context_digest
    )


def _is_header_current(old_state: ExportPhaseState, new_state: ExportPhaseState, header: str) -> bool:
    return (
        _is_state_contract_compatible(old_state, new_state)
        and old_state.reflect_headers.get(header) == new_state.reflect_headers[header]
    )


def _load_or_parse_header_export(
    module_name: str,
    header: str,
    old_state: ExportPhaseState,
    new_state: ExportPhaseState,
) -> dict:
    if _is_header_current(old_state, new_state, header):
        symbols = old_state.raw_symbols_by_header.get(header)
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


def _build_module_export_from_state(
    module_name: str,
    old_state: ExportPhaseState,
    new_state: ExportPhaseState,
    max_workers: int,
) -> tuple[dict[str, dict], int]:
    module_config = configs.get_module_config(module_name)
    raw_symbols_by_header: dict[str, dict] = {}
    headers_to_parse: list[str] = []

    for header in module_config.reflect_headers:
        symbols = _load_or_parse_header_export(module_name, header, old_state, new_state)
        if symbols is None:
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
            )
            for header in headers_to_parse
        ]
        if worker_count == 1:
            results = [_parse_header_export_worker(args) for args in worker_args]
        else:
            with ProcessPoolExecutor(
                max_workers=worker_count,
                initializer=initialize_worker_config,
                initargs=(
                    configs.ARCH,
                    configs.RUNTIME_VARIANT,
                    configs.get_loaded_project_files(),
                ),
            ) as executor:
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
    old_export_info, old_state = _load_previous_export(module_name)
    new_state = _make_current_export_state(module_name, old_state)
    if _is_export_current(old_state, new_state, old_export_info is not None):
        elapsed_ms = (time.perf_counter() - start_time) * 1000.0
        logging.info(
            "[DHT] Export %s: up to date (%d headers, %d symbols) in %.0f ms",
            module_name,
            len(new_state.reflect_headers),
            len(old_export_info.Symbols),
            elapsed_ms,
        )
        return

    logging.debug("[DHT] Export %s: scanning %d reflected headers", module_name, len(new_state.reflect_headers))
    raw_symbols_by_header, parsed_header_count = _build_module_export_from_state(
        module_name,
        old_state,
        new_state,
        max_workers,
    )
    dependency_symbols = load_dependency_symbols(module_name)
    export_info = resolve_module_export_info(
        module_name,
        raw_symbols_by_header,
        dependency_symbols,
    )
    new_state.raw_symbols_by_header = raw_symbols_by_header
    save_module_export_file(export_info)
    # The digest describes the atomically published bytes, including the host
    # text newline convention, so the next invocation can validate the file.
    new_state.resolved_export_digest = utils.calc_sha256(export_file_path)
    save_export_phase_state(new_state)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    logging.info(
        "[DHT] Export %s: updated %d/%d headers, wrote %d symbols in %.0f ms",
        module_name,
        parsed_header_count,
        len(new_state.reflect_headers),
        len(export_info.Symbols),
        elapsed_ms,
    )
