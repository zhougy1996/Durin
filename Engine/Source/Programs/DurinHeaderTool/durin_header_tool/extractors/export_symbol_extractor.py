import logging
import time
from dataclasses import replace

from durin_header_tool import config as configs
from durin_header_tool.model.export_info import ExportedSymbolInfo, ModuleExportInfo
from durin_header_tool.parser.reflection_parser import parse_reflection_header
from durin_header_tool.resolver.reflection_resolver import load_dependency_symbols, resolve_symbol_name


def _extract_header_export_symbols_impl(
    module_name: str,
    header: str,
) -> dict[str, ExportedSymbolInfo]:
    symbols: dict[str, ExportedSymbolInfo] = {}
    header_info = parse_reflection_header(
        module_name,
        header,
        export_mode=True,
    )
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
            IsAbstract=class_info.is_abstract,
        )
    for enum_info in header_info.enums:
        symbols[enum_info.qualified_name] = ExportedSymbolInfo(
            Kind="enum",
            ShortName=enum_info.short_name,
            Namespace=enum_info.namespace,
            QualifiedName=enum_info.qualified_name,
            GeneratedHelperName=enum_info.generated_helper_name,
            Header=enum_info.header,
            API=enum_info.api,
            IsScoped=enum_info.is_scoped,
            UnderlyingType=enum_info.underlying_type,
            UnderlyingKind=enum_info.underlying_kind,
            UnderlyingSize=enum_info.underlying_size,
        )
    for struct_info in header_info.structs:
        symbols[struct_info.qualified_name] = ExportedSymbolInfo(
            Kind="struct",
            ShortName=struct_info.short_name,
            Namespace=struct_info.namespace,
            QualifiedName=struct_info.qualified_name,
            GeneratedHelperName=struct_info.generated_helper_name,
            Header=struct_info.header,
            API=struct_info.api,
        )
    return symbols


def extract_header_export_symbols(
    module_name: str,
    header: str,
) -> dict[str, ExportedSymbolInfo]:
    header_start_time = time.perf_counter()
    logging.debug("[DHT] Export %s: parsing %s", module_name, header)
    symbols = _extract_header_export_symbols_impl(module_name, header)
    elapsed_ms = (time.perf_counter() - header_start_time) * 1000.0
    logging.debug(
        "[DHT] Export %s: scanned %s (%d symbols) in %.0f ms",
        module_name,
        header,
        len(symbols),
        elapsed_ms,
    )
    return symbols


def _resolve_base_name(
    symbol: ExportedSymbolInfo,
    available_symbols: dict[str, ExportedSymbolInfo],
) -> str | None:
    base_name = symbol.BaseQualifiedName
    if not base_name:
        return ""
    if base_name in available_symbols:
        return base_name
    if "::" not in base_name and symbol.Namespace:
        local_name = f"{symbol.Namespace}::{base_name}"
        if local_name in available_symbols:
            return local_name
    return resolve_symbol_name(base_name, available_symbols, kinds=("class",))


def resolve_module_export_info(
    module_name: str,
    raw_symbols_by_header: dict[str, dict[str, ExportedSymbolInfo]],
    dependency_symbols: dict[str, ExportedSymbolInfo],
    *,
    strict: bool = True,
) -> ModuleExportInfo:
    raw_symbols = {
        qualified_name: symbol
        for header_symbols in raw_symbols_by_header.values()
        for qualified_name, symbol in header_symbols.items()
    }
    available_symbols = {**dependency_symbols, **raw_symbols}
    export_info = ModuleExportInfo(Module=module_name)
    for qualified_name, raw_symbol in sorted(raw_symbols.items()):
        resolved_base = _resolve_base_name(raw_symbol, available_symbols)
        if raw_symbol.BaseQualifiedName and resolved_base is None:
            if strict:
                raise ValueError(
                    f"{raw_symbol.Header}: reflected class '{qualified_name}' has unsupported "
                    f"non-hermetic base type '{raw_symbol.BaseQualifiedName}'"
                )
            resolved_base = raw_symbol.BaseQualifiedName
        export_info.Symbols[qualified_name] = replace(
            raw_symbol,
            BaseQualifiedName=resolved_base or "",
        )
    return export_info


def extract_module_export_info(module_name: str) -> ModuleExportInfo:
    module_config = configs.get_module_config(module_name)
    raw_symbols_by_header = {}

    for header in module_config.reflect_headers:
        raw_symbols_by_header[header] = extract_header_export_symbols(module_name, header)

    dependency_symbols = load_dependency_symbols(module_name)
    return resolve_module_export_info(
        module_name,
        raw_symbols_by_header,
        dependency_symbols,
        strict=False,
    )
