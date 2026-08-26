from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed
import logging
import time

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.cache.phase_state import (
    ReflectionPhaseState,
    canonical_json_bytes,
    fingerprint_native_libclang,
    load_reflection_phase_state,
    save_reflection_phase_state,
    sha256_bytes,
)
from durin_header_tool.model.generated_output import (
    generated_output_names,
    header_generated_names,
    header_generated_paths,
    module_generated_source_name,
)
from durin_header_tool.model.reflection_generation import ReflectionHeaderGenerationResult
from durin_header_tool.model.reflection_info import (
    SYMBOL_NAME_SCHEME,
    TOOL_VERSION,
)
from durin_header_tool.model.export_info import ExportedSymbolInfo
from durin_header_tool.parser.reflection_parser import PARSER_CONTEXT_VERSION, parse_reflection_header
from durin_header_tool.resolver.reflection_resolver import (
    load_available_symbols,
    resolved_symbol_dependencies_for_header,
    resolve_header_symbols,
    symbol_dependency_snapshot,
)
from durin_header_tool.runtime.worker_context import initialize_worker_config
from durin_header_tool.runtime.parallelism import resolve_worker_count
from durin_header_tool.writers.reflection_source_writer import generate_cpp_content, generate_header_content


_GENERATOR_OPTIONS_HASH = "cpp-packages-namespace-scoped-v2"


def _reflection_cache_context_digest() -> str:
    return sha256_bytes(canonical_json_bytes({
        "GeneratorOptionsHash": _GENERATOR_OPTIONS_HASH,
        "ParserContextVersion": PARSER_CONTEXT_VERSION,
        "SymbolNameScheme": SYMBOL_NAME_SCHEME,
    }))


def _load_previous_state(module_name: str) -> ReflectionPhaseState | None:
    return load_reflection_phase_state(module_name)


def _generated_output_paths(module_name: str, header: str) -> tuple[Path, Path]:
    output_dir = utils.get_module_dht_output_dir(module_name)
    return header_generated_paths(output_dir, header)


def _generated_outputs_missing(module_name: str, header: str) -> bool:
    return any(not output_path.exists() for output_path in _generated_output_paths(module_name, header))


def _generated_outputs_damaged(module_name: str, header: str, state: ReflectionPhaseState) -> bool:
    for output_path in _generated_output_paths(module_name, header):
        expected_digest = state.generated_output_digests.get(output_path.name)
        if expected_digest is None or utils.calc_sha256(output_path) != expected_digest:
            return True
    return False


def _copy_generated_output_digests(
    module_name: str,
    header: str,
    old_state: ReflectionPhaseState,
    new_state: ReflectionPhaseState,
) -> None:
    for output_name in header_generated_names(header):
        digest = old_state.generated_output_digests.get(output_name)
        if digest is not None:
            new_state.generated_output_digests[output_name] = digest


def get_reflection_headers_requiring_regeneration(
    module_name: str,
    old_state: ReflectionPhaseState,
    new_state: ReflectionPhaseState,
    available_symbols: dict[str, ExportedSymbolInfo] | None = None,
) -> list[str]:
    if old_state is None:
        return list(new_state.reflect_headers.keys())

    if _reflection_state_contract_changed(old_state, new_state):
        return list(new_state.reflect_headers.keys())

    headers_requiring_regeneration = []
    for header, new_fingerprint in new_state.reflect_headers.items():
        old_fingerprint = old_state.reflect_headers.get(header)
        old_result = old_state.results_by_header.get(header)
        if (
            old_fingerprint != new_fingerprint
            or old_result is None
            or (
                old_state.dependency_exports != new_state.dependency_exports
                and not _resolved_dependencies_match(old_result, available_symbols or {})
            )
        ):
            headers_requiring_regeneration.append(header)
        else:
            new_state.results_by_header[header] = old_result
            _copy_generated_output_digests(module_name, header, old_state, new_state)
    return headers_requiring_regeneration


def _resolved_dependencies_match(
    result: ReflectionHeaderGenerationResult,
    available_symbols: dict[str, ExportedSymbolInfo],
) -> bool:
    return all(
        qualified_name in available_symbols
        and symbol_dependency_snapshot(available_symbols[qualified_name]) == snapshot
        for qualified_name, snapshot in result.resolved_symbol_dependencies.items()
    )


def _reflection_state_contract_changed(
    old_state: ReflectionPhaseState,
    new_state: ReflectionPhaseState,
) -> bool:
    return (
        old_state.schema_version != new_state.schema_version
        or old_state.tool_fingerprint != new_state.tool_fingerprint
        or old_state.native_libclang_fingerprint != new_state.native_libclang_fingerprint
        or old_state.module != new_state.module
        or old_state.runtime_variant != new_state.runtime_variant
        or old_state.platform != new_state.platform
        or old_state.context_digest != new_state.context_digest
    )


def make_new_reflection_state(
    module_name: str,
    old_state: ReflectionPhaseState = None,
) -> ReflectionPhaseState:
    module_config = configs.get_module_config(module_name)
    state = ReflectionPhaseState(
        module=module_name,
        runtime_variant=configs.RUNTIME_VARIANT,
        platform=configs.ARCH,
        tool_fingerprint=configs.TOOL_FINGERPRINT or TOOL_VERSION,
        native_libclang_fingerprint=fingerprint_native_libclang(),
        context_digest=_reflection_cache_context_digest(),
        generated_outputs=sorted(generated_output_names(module_name, module_config.reflect_headers)),
    )
    reuse_old_header_fingerprints = (
        old_state is not None
        and not _reflection_state_contract_changed(old_state, state)
    )
    dependent_modules_with_export_file = configs.collect_all_dependent_module_with_export_file(module_name)

    for dep_module in dependent_modules_with_export_file:
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for dependent module '{dep_module}' not found at expected path: {export_file_path}")
        state.dependency_exports[dep_module] = utils.calc_sha256(export_file_path)

    for header in module_config.reflect_headers:
        header_file_path = (module_config.module_dir / header).resolve()
        if not header_file_path.exists():
            raise FileNotFoundError(f"Reflect header file '{header}' for module '{module_name}' not found at expected path: {header_file_path}")

        old_header_fingerprint = (
            old_state.reflect_headers.get(header)
            if reuse_old_header_fingerprints
            else None
        )
        state.reflect_headers[header] = utils.get_file_fingerprint_with_old_cache(header_file_path, old_header_fingerprint)

    return state


def _cleanup_stale_generated_outputs(module_name: str, output_names: list[str]) -> None:
    if not output_names:
        return

    output_dir = utils.get_module_dht_output_dir(module_name)
    for output_name in output_names:
        (output_dir / output_name).unlink(missing_ok=True)
    logging.debug(
        "[DHT] Reflection %s: removed %d stale generated outputs",
        module_name,
        len(output_names),
    )


def _generate_reflection_output_impl(
    module_name: str,
    header: str,
    symbols: dict[str, ExportedSymbolInfo],
) -> ReflectionHeaderGenerationResult:
    header_info = parse_reflection_header(module_name, header, exported_symbols=symbols)
    resolve_header_symbols(header_info, symbols)
    return ReflectionHeaderGenerationResult(
        header=header,
        generated_header=generate_header_content(header_info),
        generated_source=generate_cpp_content(header_info, symbols),
        class_count=len(header_info.classes),
        property_count=sum(len(class_info.properties) for class_info in header_info.classes),
        resolved_symbol_dependencies=resolved_symbol_dependencies_for_header(header_info, symbols),
    )


def _generate_reflection_output_worker(args):
    module_name, header, symbols, arch, runtime_variant = args

    from durin_header_tool.generators.module_reflection_files_generator import _generate_reflection_output_impl as worker_generate

    initialize_worker_config(arch, runtime_variant)

    start_time = time.perf_counter()
    result = worker_generate(module_name, header, symbols)
    return ReflectionHeaderGenerationResult(
        header=result.header,
        generated_header=result.generated_header,
        generated_source=result.generated_source,
        class_count=result.class_count,
        property_count=result.property_count,
        resolved_symbol_dependencies=result.resolved_symbol_dependencies,
        elapsed_ms=(time.perf_counter() - start_time) * 1000.0,
    )


def _write_reflection_output(
    module_name: str,
    result: ReflectionHeaderGenerationResult,
    state: ReflectionPhaseState,
) -> None:
    output_dir = utils.get_module_dht_output_dir(module_name)
    output_dir.mkdir(parents=True, exist_ok=True)

    generated_header_name, generated_source_name = header_generated_names(result.header)
    generated_header_path = output_dir / generated_header_name
    generated_source_path = output_dir / generated_source_name
    utils.generate_file(generated_header_path, result.generated_header)
    utils.generate_file(generated_source_path, result.generated_source)
    # Hash the published bytes. Text-mode output uses the platform newline
    # convention, so hashing the in-memory LF text would mark every Windows
    # output as damaged on the next invocation.
    state.generated_output_digests[generated_header_name] = utils.calc_sha256(generated_header_path)
    state.generated_output_digests[generated_source_name] = utils.calc_sha256(generated_source_path)
    state.results_by_header[result.header] = result
    logging.debug(
        "[DHT] Reflection %s: wrote %s.gen.* (%d classes, %d properties) in %.0f ms",
        module_name,
        Path(result.header).stem,
        result.class_count,
        result.property_count,
        result.elapsed_ms,
    )


def _write_reflection_files(
    module_name: str,
    headers_to_regenerate: list[str],
    symbols: dict[str, ExportedSymbolInfo],
    state: ReflectionPhaseState,
    max_workers: int,
) -> tuple[int, int]:
    output_dir = utils.get_module_dht_output_dir(module_name)
    output_dir.mkdir(parents=True, exist_ok=True)

    results_by_header: dict[str, ReflectionHeaderGenerationResult] = {}
    headers_to_parse: list[str] = []
    if headers_to_regenerate:
        for header in headers_to_regenerate:
            if header in state.results_by_header:
                results_by_header[header] = state.results_by_header[header]
            else:
                headers_to_parse.append(header)

        worker_count = resolve_worker_count(len(headers_to_parse), max_workers)
        if worker_count > 1:
            logging.debug(
                "[DHT] Reflection %s: parsing %d headers with %d workers",
                module_name,
                len(headers_to_parse),
                worker_count,
            )
        worker_args = [
            (
                module_name,
                header,
                symbols,
                configs.ARCH,
                configs.RUNTIME_VARIANT,
            )
            for header in headers_to_parse
        ]
        parsed_results: list[ReflectionHeaderGenerationResult] = []
        if worker_count == 1:
            parsed_results = [_generate_reflection_output_worker(args) for args in worker_args]
        elif worker_count > 1:
            with ProcessPoolExecutor(max_workers=worker_count) as executor:
                futures = [executor.submit(_generate_reflection_output_worker, args) for args in worker_args]
                parsed_results = [future.result() for future in as_completed(futures)]

        for result in parsed_results:
            results_by_header[result.header] = result

        header_order = {header: index for index, header in enumerate(configs.get_module_config(module_name).reflect_headers)}
        ordered_results = sorted(results_by_header.values(), key=lambda item: header_order[item.header])
        for result in ordered_results:
            _write_reflection_output(module_name, result, state)
    else:
        logging.debug(
            "[DHT] Reflection %s: no headers require regeneration",
            module_name,
        )

    module_source = (
        "// Generated module reflection source.\n\n"
        '#include "DObject/Package.h"\n\n'
        "namespace\n{\n"
        f"\tstruct FRegisterCppPackage_{module_name}\n\t{{\n"
        f"\t\tFRegisterCppPackage_{module_name}() {{ Durin::RegisterCompiledInPackage(\"{module_name}\"); }}\n"
        "\t};\n"
        f"\tstatic FRegisterCppPackage_{module_name} GRegisterCppPackage_{module_name};\n"
        "}\n"
    )
    module_source_name = module_generated_source_name(module_name)
    module_source_path = output_dir / module_source_name
    utils.generate_file(module_source_path, module_source)
    state.generated_output_digests[module_source_name] = utils.calc_sha256(module_source_path)
    return (
        sum(result.class_count for result in results_by_header.values()),
        sum(result.property_count for result in results_by_header.values()),
    )


def generate_reflection_files(module_name: str, max_workers: int = 1) -> None:
    start_time = time.perf_counter()
    logging.debug("[DHT] Reflection %s: preparing phase state", module_name)
    old_state = _load_previous_state(module_name)
    new_state = make_new_reflection_state(module_name, old_state)

    dep_exports_changed = (
        old_state is not None
        and not _reflection_state_contract_changed(old_state, new_state)
        and old_state.dependency_exports != new_state.dependency_exports
    )
    symbols: dict[str, ExportedSymbolInfo] = (
        load_available_symbols(module_name) if dep_exports_changed else {}
    )

    headers_to_regenerate = get_reflection_headers_requiring_regeneration(
        module_name,
        old_state,
        new_state,
        symbols,
    )
    for header in new_state.reflect_headers:
        if (
            header not in headers_to_regenerate
            and (
                _generated_outputs_missing(module_name, header)
                or _generated_outputs_damaged(module_name, header, new_state)
            )
        ):
            headers_to_regenerate.append(header)
    if dep_exports_changed:
        logging.debug(
            "[DHT] Reflection %s: dependency exports changed, %d/%d headers affected",
            module_name,
            len(headers_to_regenerate),
            len(new_state.reflect_headers),
        )

    total_headers = len(new_state.reflect_headers)
    skipped_headers = total_headers - len(headers_to_regenerate)
    logging.debug(
        "[DHT] Reflection %s: %d/%d headers require regeneration (%d skipped)",
        module_name,
        len(headers_to_regenerate),
        total_headers,
        skipped_headers,
    )
    headers_to_parse = [
        header for header in headers_to_regenerate
        if header not in new_state.results_by_header
    ]
    if headers_to_parse:
        if not symbols:
            symbols = load_available_symbols(module_name)
    else:
        logging.debug("[DHT] Reflection %s: no headers require parsing, skipped symbols loading", module_name)

    class_count, property_count = _write_reflection_files(
        module_name,
        headers_to_regenerate,
        symbols,
        new_state,
        max_workers,
    )
    old_outputs = set(old_state.generated_outputs) if old_state else set()
    old_pending_cleanup = set(old_state.pending_cleanup_outputs) if old_state else set()
    new_outputs = set(new_state.generated_outputs)
    new_state.pending_cleanup_outputs = sorted((old_outputs | old_pending_cleanup) - new_outputs)
    save_reflection_phase_state(new_state)
    if new_state.pending_cleanup_outputs:
        _cleanup_stale_generated_outputs(module_name, new_state.pending_cleanup_outputs)
        new_state.pending_cleanup_outputs = []
        save_reflection_phase_state(new_state)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    logging.info(
        "[DHT] Reflection %s: prepared %d/%d headers (%d parsed, %d rematerialized, "
        "%d classes, %d properties) in %.0f ms",
        module_name,
        len(headers_to_regenerate),
        total_headers,
        len(headers_to_parse),
        len(headers_to_regenerate) - len(headers_to_parse),
        class_count,
        property_count,
        elapsed_ms,
    )
