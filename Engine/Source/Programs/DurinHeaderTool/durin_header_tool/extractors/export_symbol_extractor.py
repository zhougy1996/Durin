import logging
import time

from durin_header_tool import config as configs
from durin_header_tool.model.export_info import ExportedSymbolInfo, ModuleExportInfo
from durin_header_tool.parser.reflection_parser import parse_reflection_header


def _extract_header_export_symbols_impl(module_name: str, header: str) -> dict[str, ExportedSymbolInfo]:
    symbols: dict[str, ExportedSymbolInfo] = {}
    header_info = parse_reflection_header(module_name, header, export_mode=True)
    for class_info in header_info.classes:
        symbols[class_info.qualified_name] = ExportedSymbolInfo(
            Kind="class",
            ShortName=class_info.short_name,
            Namespace=class_info.namespace,
            QualifiedName=class_info.qualified_name,
            GeneratedHelperName=class_info.generated_helper_name,
            Header=class_info.header,
            API=class_info.api,
            BaseQualifiedName=class_info.base_qualified_name,
        )
    return symbols


def extract_header_export_symbols(module_name: str, header: str) -> dict[str, ExportedSymbolInfo]:
    header_start_time = time.perf_counter()
    logging.info("[DHT] Export %s: parsing %s", module_name, header)
    symbols = _extract_header_export_symbols_impl(module_name, header)
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
    export_info = ModuleExportInfo(Module=module_name)

    for header in module_config.reflect_headers:
        export_info.Symbols.update(extract_header_export_symbols(module_name, header))

    return export_info
