from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed
import logging
import time

from durin_header_tool import config as configs
from durin_header_tool import io as utils
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
from durin_header_tool.parser.reflection_parser import parse_reflection_header
from durin_header_tool.resolver.reflection_resolver import (
    header_symbol_dependencies_changed,
    load_available_symbols,
    resolved_symbol_dependencies_for_header,
    resolve_header_symbols,
)
from durin_header_tool.runtime.worker_context import initialize_worker_config
from durin_header_tool.runtime.parallelism import resolve_worker_count
from durin_header_tool.writers.reflection_source_writer import generate_cpp_content, generate_header_content


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


def get_reflection_headers_requiring_regeneration(
    module_name: str,
    old_manifest: ModuleManifest,
    new_manifest: ModuleManifest,
    symbols: dict[str, object] | None = None,
) -> list[str]:
    if old_manifest is None:
        return list(new_manifest.reflect_headers.keys())

    if reflection_manifest_contract_changed(old_manifest, new_manifest):
        return list(new_manifest.reflect_headers.keys())

    headers_requiring_regeneration = []
    dep_exports_changed = dependency_exports_changed(old_manifest, new_manifest)
    if dep_exports_changed and symbols is None:
        return list(new_manifest.reflect_headers.keys())

    for header, new_fingerprint in new_manifest.reflect_headers.items():
        old_fingerprint = old_manifest.reflect_headers.get(header)
        if (
            old_fingerprint != new_fingerprint
            or _generated_outputs_missing(module_name, header)
            or header not in old_manifest.resolved_symbol_dependencies
            or (dep_exports_changed and header_symbol_dependencies_changed(header, old_manifest, symbols))
        ):
            headers_requiring_regeneration.append(header)
        else:
            new_manifest.resolved_symbol_dependencies[header] = old_manifest.resolved_symbol_dependencies.get(header, {})
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
        generator_options_hash="cpp-packages-v1",
        generated_outputs=make_generated_output_names(module_name, module_config.reflect_headers),
    )
    dependent_modules_with_export_file = configs.collect_all_dependent_module_with_export_file(module_name)

    for dep_module in dependent_modules_with_export_file:
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for dependent module '{dep_module}' not found at expected path: {export_file_path}")
        manifest.dep_module_exports[dep_module] = utils.get_light_file_fingerprint(export_file_path)

    for header in module_config.reflect_headers:
        header_file_path = (module_config.module_dir / header).resolve()
        if not header_file_path.exists():
            raise FileNotFoundError(f"Reflect header file '{header}' for module '{module_name}' not found at expected path: {header_file_path}")

        old_header_fingerprint = old_manifest.reflect_headers.get(header) if old_manifest else None
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


def _generate_reflection_output_impl(module_name: str, header: str, symbols: dict[str, object]) -> dict[str, object]:
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
    utils.generate_file(output_dir / f"{header_filename}.gen.h", result["header_content"])
    utils.generate_file(output_dir / f"{header_filename}.gen.cpp", result["cpp_content"])
    manifest.resolved_symbol_dependencies[header] = result["resolved_symbol_dependencies"]
    logging.debug(
        "[DHT] Reflection %s: wrote %s.gen.* (%d classes, %d properties) in %.0f ms",
        module_name,
        header_filename,
        result["class_count"],
        result["property_count"],
        result["elapsed_ms"],
    )


def _write_reflection_files(
    module_name: str,
    headers_to_regenerate: list[str],
    symbols: dict[str, object],
    manifest: ModuleManifest,
    max_workers: int,
) -> tuple[int, int]:
    output_dir = utils.get_module_dht_output_dir(module_name)
    output_dir.mkdir(parents=True, exist_ok=True)

    if headers_to_regenerate:
        worker_count = resolve_worker_count(len(headers_to_regenerate), max_workers)
        if worker_count > 1:
            logging.debug(
                "[DHT] Reflection %s: parsing %d headers with %d workers",
                module_name,
                len(headers_to_regenerate),
                worker_count,
            )
        results: list[dict[str, object]]
        worker_args = [
            (
                module_name,
                header,
                symbols,
                configs.ARCH,
                configs.RUNTIME_VARIANT,
            )
            for header in headers_to_regenerate
        ]
        if worker_count == 1:
            results = [_generate_reflection_output_worker(args) for args in worker_args]
        else:
            with ProcessPoolExecutor(max_workers=worker_count) as executor:
                futures = [executor.submit(_generate_reflection_output_worker, args) for args in worker_args]
                results = [future.result() for future in as_completed(futures)]

        header_order = {header: index for index, header in enumerate(configs.get_module_config(module_name).reflect_headers)}
        for result in sorted(results, key=lambda item: header_order[item["header"]]):
            _write_reflection_output(module_name, result, manifest)
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
    utils.generate_file(output_dir / f"{module_name}.module.gen.cpp", module_source)
    return (
        sum(result["class_count"] for result in results) if headers_to_regenerate else 0,
        sum(result["property_count"] for result in results) if headers_to_regenerate else 0,
    )


def generate_reflection_files(module_name: str, max_workers: int = 1) -> None:
    start_time = time.perf_counter()
    logging.debug("[DHT] Reflection %s: preparing manifest", module_name)
    old_manifest = _load_previous_manifest(module_name)
    new_manifest = make_new_module_manifest(module_name, old_manifest)

    symbols = None
    dep_exports_changed = (
        old_manifest is not None
        and not reflection_manifest_contract_changed(old_manifest, new_manifest)
        and dependency_exports_changed(old_manifest, new_manifest)
    )
    if dep_exports_changed:
        symbols = load_available_symbols(module_name)

    headers_to_regenerate = get_reflection_headers_requiring_regeneration(
        module_name,
        old_manifest,
        new_manifest,
        symbols,
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
    if headers_to_regenerate and symbols is None:
        symbols = load_available_symbols(module_name)
    elif not headers_to_regenerate and symbols is None:
        logging.debug("[DHT] Reflection %s: no headers require regeneration, skipped symbols loading", module_name)

    class_count, property_count = _write_reflection_files(
        module_name,
        headers_to_regenerate,
        symbols or {},
        new_manifest,
        max_workers,
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
    logging.info(
        "[DHT] Reflection %s: regenerated %d/%d headers (%d classes, %d properties) in %.0f ms",
        module_name,
        len(headers_to_regenerate),
        total_headers,
        class_count,
        property_count,
        elapsed_ms,
    )
