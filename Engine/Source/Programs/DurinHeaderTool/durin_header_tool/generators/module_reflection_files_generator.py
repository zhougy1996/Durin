from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed
import logging
import time

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.cache.persistent_header_cache import (
    CacheDiagnostics,
    CacheEntryIdentity,
    CachePhase,
    PersistentHeaderCache,
    ReflectionHeaderCachePayload,
    canonical_json_bytes,
    fingerprint_native_libclang,
    make_persistent_header_cache,
    sha256_bytes,
)
from durin_header_tool.cache.reflection_cache import (
    dependency_exports_changed,
    reflection_manifest_contract_changed,
)
from durin_header_tool.model.reflection_manifest import (
    ModuleManifest,
    load_module_manifest_file,
    make_generated_output_names,
    save_module_manifest_file,
)
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
)
from durin_header_tool.runtime.worker_context import initialize_worker_config
from durin_header_tool.runtime.parallelism import resolve_worker_count
from durin_header_tool.writers.reflection_source_writer import generate_cpp_content, generate_header_content


_GENERATOR_OPTIONS_HASH = "cpp-packages-v1"


def _reflection_cache_context_digest() -> str:
    return sha256_bytes(canonical_json_bytes({
        "GeneratorOptionsHash": _GENERATOR_OPTIONS_HASH,
        "ParserContextVersion": PARSER_CONTEXT_VERSION,
        "SymbolNameScheme": SYMBOL_NAME_SCHEME,
    }))


def _available_symbols_digest(symbols: dict[str, ExportedSymbolInfo]) -> str:
    semantic_symbols = {
        qualified_name: vars(symbol)
        for qualified_name, symbol in sorted(symbols.items())
    }
    return sha256_bytes(canonical_json_bytes(semantic_symbols))


def _make_reflection_cache_identity(
    module_name: str,
    header: str,
    native_libclang_fingerprint: str,
    dependency_digest: str,
) -> CacheEntryIdentity:
    module_config = configs.get_module_config(module_name)
    header_path = (module_config.module_dir / header).resolve()
    return CacheEntryIdentity(
        tool_fingerprint=configs.TOOL_FINGERPRINT or TOOL_VERSION,
        native_libclang_fingerprint=native_libclang_fingerprint,
        platform=configs.ARCH,
        runtime_variant=configs.RUNTIME_VARIANT,
        context_digest=_reflection_cache_context_digest(),
        module=module_name,
        logical_header=header,
        header_content_digest=sha256_bytes(header_path.read_bytes()),
        dependency_digest=dependency_digest,
    )


def _load_previous_manifest(module_name: str) -> ModuleManifest | None:
    manifest_file_path = utils.get_module_manifest_file_path(module_name)
    if not manifest_file_path.exists():
        return None
    try:
        return load_module_manifest_file(module_name)
    except (OSError, UnicodeError, ValueError, TypeError, AttributeError, KeyError) as error:
        logging.warning(
            "[DHT] Reflection %s: ignoring invalid manifest %s (%s)",
            module_name,
            manifest_file_path,
            error,
        )
        return None


def _generated_output_paths(module_name: str, header: str) -> list[Path]:
    output_dir = utils.get_module_dht_output_dir(module_name)
    header_filename = Path(header).stem
    return [
        output_dir / f"{header_filename}.gen.h",
        output_dir / f"{header_filename}.gen.cpp",
    ]


def _generated_outputs_missing(module_name: str, header: str) -> bool:
    return any(not output_path.exists() for output_path in _generated_output_paths(module_name, header))


def _generated_outputs_damaged(module_name: str, header: str, manifest: ModuleManifest) -> bool:
    for output_path in _generated_output_paths(module_name, header):
        expected_digest = manifest.generated_output_digests.get(output_path.name)
        if expected_digest is None or utils.calc_sha256(output_path) != expected_digest:
            return True
    return False


def _copy_generated_output_digests(
    module_name: str,
    header: str,
    old_manifest: ModuleManifest,
    new_manifest: ModuleManifest,
) -> None:
    header_filename = Path(header).stem
    for output_name in (f"{header_filename}.gen.h", f"{header_filename}.gen.cpp"):
        digest = old_manifest.generated_output_digests.get(output_name)
        if digest is not None:
            new_manifest.generated_output_digests[output_name] = digest


def get_reflection_headers_requiring_regeneration(
    module_name: str,
    old_manifest: ModuleManifest,
    new_manifest: ModuleManifest,
    symbols: dict[str, ExportedSymbolInfo] | None = None,
) -> list[str]:
    if old_manifest is None:
        return list(new_manifest.reflect_headers.keys())

    if reflection_manifest_contract_changed(old_manifest, new_manifest):
        return list(new_manifest.reflect_headers.keys())

    headers_requiring_regeneration = []
    dep_exports_changed = dependency_exports_changed(old_manifest, new_manifest)
    if dep_exports_changed:
        return list(new_manifest.reflect_headers.keys())

    for header, new_fingerprint in new_manifest.reflect_headers.items():
        old_fingerprint = old_manifest.reflect_headers.get(header)
        if (
            old_fingerprint != new_fingerprint
            or _generated_outputs_missing(module_name, header)
            or _generated_outputs_damaged(module_name, header, old_manifest)
            or header not in old_manifest.resolved_symbol_dependencies
        ):
            headers_requiring_regeneration.append(header)
        else:
            new_manifest.resolved_symbol_dependencies[header] = old_manifest.resolved_symbol_dependencies.get(header, {})
            _copy_generated_output_digests(module_name, header, old_manifest, new_manifest)
    return headers_requiring_regeneration


def make_new_module_manifest(module_name: str, old_manifest: ModuleManifest = None) -> ModuleManifest:
    module_config = configs.get_module_config(module_name)
    manifest = ModuleManifest(
        module_name=module_name,
        runtime_variant=configs.RUNTIME_VARIANT,
        platform=configs.ARCH,
        tool_version=TOOL_VERSION,
        tool_fingerprint=configs.TOOL_FINGERPRINT or TOOL_VERSION,
        symbol_name_scheme=SYMBOL_NAME_SCHEME,
        generator_options_hash=_GENERATOR_OPTIONS_HASH,
        generated_outputs=make_generated_output_names(module_name, module_config.reflect_headers),
    )
    reuse_old_header_fingerprints = (
        old_manifest is not None
        and not reflection_manifest_contract_changed(old_manifest, manifest)
    )
    dependent_modules_with_export_file = configs.collect_all_dependent_module_with_export_file(module_name)

    for dep_module in dependent_modules_with_export_file:
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for dependent module '{dep_module}' not found at expected path: {export_file_path}")
        manifest.dep_module_exports[dep_module] = utils.calc_sha256(export_file_path)

    for header in module_config.reflect_headers:
        header_file_path = (module_config.module_dir / header).resolve()
        if not header_file_path.exists():
            raise FileNotFoundError(f"Reflect header file '{header}' for module '{module_name}' not found at expected path: {header_file_path}")

        old_header_fingerprint = (
            old_manifest.reflect_headers.get(header)
            if reuse_old_header_fingerprints
            else None
        )
        manifest.reflect_headers[header] = utils.get_file_fingerprint_with_old_cache(header_file_path, old_header_fingerprint)

    return manifest


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


def _generate_reflection_output_impl(module_name: str, header: str, symbols: dict[str, ExportedSymbolInfo]) -> dict[str, object]:
    header_info = parse_reflection_header(module_name, header, exported_symbols=symbols)
    resolve_header_symbols(header_info, symbols)
    return {
        "header": header,
        "header_content": generate_header_content(header_info),
        "cpp_content": generate_cpp_content(header_info, symbols),
        "class_count": len(header_info.classes),
        "property_count": sum(len(class_info.properties) for class_info in header_info.classes),
        "resolved_symbol_dependencies": resolved_symbol_dependencies_for_header(header_info, symbols),
    }


def _generate_reflection_output_worker(args):
    module_name, header, symbols, arch, runtime_variant = args

    from durin_header_tool.generators.module_reflection_files_generator import _generate_reflection_output_impl as worker_generate

    initialize_worker_config(arch, runtime_variant)

    start_time = time.perf_counter()
    result = worker_generate(module_name, header, symbols)
    result["elapsed_ms"] = (time.perf_counter() - start_time) * 1000.0
    return result


def _write_reflection_output(module_name: str, result: dict[str, object], manifest: ModuleManifest) -> None:
    output_dir = utils.get_module_dht_output_dir(module_name)
    output_dir.mkdir(parents=True, exist_ok=True)

    header = result["header"]
    header_filename = Path(header).stem
    generated_header_name = f"{header_filename}.gen.h"
    generated_source_name = f"{header_filename}.gen.cpp"
    utils.generate_file(output_dir / generated_header_name, result["header_content"])
    utils.generate_file(output_dir / generated_source_name, result["cpp_content"])
    manifest.generated_output_digests[generated_header_name] = sha256_bytes(result["header_content"].encode("utf-8"))
    manifest.generated_output_digests[generated_source_name] = sha256_bytes(result["cpp_content"].encode("utf-8"))
    manifest.resolved_symbol_dependencies[header] = result["resolved_symbol_dependencies"]
    logging.debug(
        "[DHT] Reflection %s: wrote %s.gen.* (%d classes, %d properties) in %.0f ms",
        module_name,
        header_filename,
        result["class_count"],
        result["property_count"],
        result["elapsed_ms"],
    )


def _reflection_result_from_payload(header: str, payload: ReflectionHeaderCachePayload) -> dict[str, object]:
    return {
        "header": header,
        "header_content": payload.generated_header,
        "cpp_content": payload.generated_source,
        "class_count": payload.class_count,
        "property_count": payload.property_count,
        "resolved_symbol_dependencies": payload.resolved_symbol_dependencies,
        "elapsed_ms": 0.0,
    }


def _reflection_payload_from_result(result: dict[str, object]) -> ReflectionHeaderCachePayload:
    return ReflectionHeaderCachePayload(
        generated_header=result["header_content"],
        generated_source=result["cpp_content"],
        class_count=result["class_count"],
        property_count=result["property_count"],
        resolved_symbol_dependencies=result["resolved_symbol_dependencies"],
    )


def _write_reflection_files(
    module_name: str,
    headers_to_regenerate: list[str],
    symbols: dict[str, ExportedSymbolInfo],
    manifest: ModuleManifest,
    max_workers: int,
    *,
    persistent_cache: PersistentHeaderCache | None = None,
    native_libclang_fingerprint: str = "",
    dependency_digest: str = "",
    diagnostics: CacheDiagnostics | None = None,
) -> tuple[int, int]:
    output_dir = utils.get_module_dht_output_dir(module_name)
    output_dir.mkdir(parents=True, exist_ok=True)

    results_by_header: dict[str, dict[str, object]] = {}
    cache_identities: dict[str, CacheEntryIdentity] = {}
    headers_to_parse: list[str] = []
    if headers_to_regenerate:
        for header in headers_to_regenerate:
            if persistent_cache is None:
                headers_to_parse.append(header)
                continue
            identity = _make_reflection_cache_identity(
                module_name,
                header,
                native_libclang_fingerprint,
                dependency_digest,
            )
            cache_identities[header] = identity
            lookup = persistent_cache.read(CachePhase.REFLECTION, identity)
            if diagnostics is not None:
                diagnostics.record_lookup(lookup)
            if lookup.is_hit:
                payload = lookup.payload
                if not isinstance(payload, ReflectionHeaderCachePayload):
                    raise TypeError("Persistent reflection cache returned an incompatible payload.")
                results_by_header[header] = _reflection_result_from_payload(header, payload)
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
        parsed_results: list[dict[str, object]] = []
        if worker_count == 1:
            parsed_results = [_generate_reflection_output_worker(args) for args in worker_args]
        elif worker_count > 1:
            with ProcessPoolExecutor(max_workers=worker_count) as executor:
                futures = [executor.submit(_generate_reflection_output_worker, args) for args in worker_args]
                parsed_results = [future.result() for future in as_completed(futures)]

        if diagnostics is not None:
            diagnostics.record_parse(len(parsed_results))
        for result in parsed_results:
            results_by_header[result["header"]] = result

        # Publish only after the complete parse/resolve/source-generation batch
        # succeeds. Entries are committed before output materialization so an
        # interrupted materialization can recover through an ordinary rerun.
        if persistent_cache is not None:
            for result in parsed_results:
                header = result["header"]
                persistent_cache.write(
                    CachePhase.REFLECTION,
                    cache_identities[header],
                    _reflection_payload_from_result(result),
                )

        header_order = {header: index for index, header in enumerate(configs.get_module_config(module_name).reflect_headers)}
        ordered_results = sorted(results_by_header.values(), key=lambda item: header_order[item["header"]])
        for result in ordered_results:
            _write_reflection_output(module_name, result, manifest)
            if diagnostics is not None:
                diagnostics.record_materialization()
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
    module_source_name = f"{module_name}.module.gen.cpp"
    utils.generate_file(output_dir / module_source_name, module_source)
    manifest.generated_output_digests[module_source_name] = sha256_bytes(module_source.encode("utf-8"))
    return (
        sum(result["class_count"] for result in results_by_header.values()),
        sum(result["property_count"] for result in results_by_header.values()),
    )


def generate_reflection_files(module_name: str, max_workers: int = 1) -> None:
    start_time = time.perf_counter()
    logging.debug("[DHT] Reflection %s: preparing manifest", module_name)
    old_manifest = _load_previous_manifest(module_name)
    new_manifest = make_new_module_manifest(module_name, old_manifest)
    persistent_cache = make_persistent_header_cache(module_name)
    persistent_cache.cleanup_stale_headers(
        CachePhase.REFLECTION,
        module_name,
        list(new_manifest.reflect_headers),
    )
    diagnostics = CacheDiagnostics()

    dep_exports_changed = (
        old_manifest is not None
        and not reflection_manifest_contract_changed(old_manifest, new_manifest)
        and dependency_exports_changed(old_manifest, new_manifest)
    )

    headers_to_regenerate = get_reflection_headers_requiring_regeneration(
        module_name,
        old_manifest,
        new_manifest,
    )
    if dep_exports_changed:
        logging.debug(
            "[DHT] Reflection %s: dependency exports changed, %d/%d headers affected",
            module_name,
            len(headers_to_regenerate),
            len(new_manifest.reflect_headers),
        )

    total_headers = len(new_manifest.reflect_headers)
    skipped_headers = total_headers - len(headers_to_regenerate)
    logging.debug(
        "[DHT] Reflection %s: %d/%d headers require regeneration (%d skipped)",
        module_name,
        len(headers_to_regenerate),
        total_headers,
        skipped_headers,
    )
    symbols: dict[str, ExportedSymbolInfo] = {}
    dependency_digest = ""
    native_libclang_fingerprint = ""
    if headers_to_regenerate:
        symbols = load_available_symbols(module_name)
        dependency_digest = _available_symbols_digest(symbols)
        native_libclang_fingerprint = fingerprint_native_libclang()
    else:
        logging.debug("[DHT] Reflection %s: no headers require regeneration, skipped symbols loading", module_name)

    class_count, property_count = _write_reflection_files(
        module_name,
        headers_to_regenerate,
        symbols,
        new_manifest,
        max_workers,
        persistent_cache=persistent_cache,
        native_libclang_fingerprint=native_libclang_fingerprint,
        dependency_digest=dependency_digest,
        diagnostics=diagnostics,
    )
    old_outputs = set(old_manifest.generated_outputs) if old_manifest else set()
    old_pending_cleanup = set(old_manifest.pending_cleanup_outputs) if old_manifest else set()
    new_outputs = set(new_manifest.generated_outputs)
    new_manifest.pending_cleanup_outputs = sorted((old_outputs | old_pending_cleanup) - new_outputs)
    save_module_manifest_file(new_manifest)
    if new_manifest.pending_cleanup_outputs:
        _cleanup_stale_generated_outputs(module_name, new_manifest.pending_cleanup_outputs)
        new_manifest.pending_cleanup_outputs = []
        save_module_manifest_file(new_manifest)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    diagnostics.log_summary(module_name, CachePhase.REFLECTION, elapsed_ms)
    logging.info(
        "[DHT] Reflection %s: regenerated %d/%d headers (%d classes, %d properties) in %.0f ms",
        module_name,
        len(headers_to_regenerate),
        total_headers,
        class_count,
        property_count,
        elapsed_ms,
    )
