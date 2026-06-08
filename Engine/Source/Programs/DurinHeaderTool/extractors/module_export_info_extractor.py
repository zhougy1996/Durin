import logging
import time

import configs
from models.export_infos import ExportedSymbolInfo, ModuleExportInfo
from models.reflection_info import parse_reflection_header


def extract_header_export_symbols(module_name: str, header: str) -> dict[str, ExportedSymbolInfo]:
    symbols: dict[str, ExportedSymbolInfo] = {}
    header_start_time = time.perf_counter()
    logging.info("[DHT] Export %s: parsing %s", module_name, header)
    header_info = parse_reflection_header(module_name, header, export_mode=True)
    for class_info in header_info.classes:
        symbols[class_info.qualified_name] = ExportedSymbolInfo(
            kind="class",
            shortName=class_info.short_name,
            namespace=class_info.namespace,
            qualifiedName=class_info.qualified_name,
            generatedHelperName=class_info.generated_helper_name,
            header=class_info.header,
            api=class_info.api,
            baseQualifiedName=class_info.base_qualified_name,
        )
    elapsed_ms = (time.perf_counter() - header_start_time) * 1000.0
    logging.info(
        "[DHT] Export %s: scanned %s (%d symbols) in %.0f ms",
        module_name,
        header,
        len(symbols),
        elapsed_ms,
    )
    return symbols


def extract_module_export_info(module_name: str) -> ModuleExportInfo:
    module_config = configs.get_module_config(module_name)
    export_info = ModuleExportInfo(module=module_name)

    for header in module_config.reflect_headers:
        export_info.symbols.update(extract_header_export_symbols(module_name, header))

    return export_info
