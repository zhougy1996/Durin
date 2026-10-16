import logging
import time
from dataclasses import replace

from durin_header_tool import config as configs
from durin_header_tool.model.export_info import ExportedSymbolInfo, ModuleExportInfo
from durin_header_tool.model.function_signature import resolve_function_type, validate_function_signature
from durin_header_tool.parser.reflection_parser import parse_reflection_header
from durin_header_tool.resolver.reflection_resolver import (
    load_dependency_symbols,
    resolve_symbol,
    symbol_resolution_diagnostic,
)


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
            NamespacePath=class_info.namespace_path,
            Header=class_info.header,
            API=class_info.api,
            BaseQualifiedName=class_info.base_qualified_name,
            IsAbstract=class_info.is_abstract,
            Functions=[{"Name": f.name, "Virtual": f.is_virtual, "Final": f.is_final,
                        "Signature": f.signature, "Line": f.source_line}
                       for f in class_info.functions],
        )
    for enum_info in header_info.enums:
        symbols[enum_info.qualified_name] = ExportedSymbolInfo(
            Kind="enum",
            ShortName=enum_info.short_name,
            Namespace=enum_info.namespace,
            QualifiedName=enum_info.qualified_name,
            NamespacePath=enum_info.namespace_path,
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
            NamespacePath=struct_info.namespace_path,
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
    resolution = resolve_symbol(
        base_name, available_symbols,
        declaring_namespace=symbol.Namespace, kinds=("class",),
    )
    return resolution.qualified_name or None


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
                resolution = resolve_symbol(
                    raw_symbol.BaseQualifiedName, available_symbols,
                    declaring_namespace=raw_symbol.Namespace, kinds=("class",),
                )
                raise ValueError(symbol_resolution_diagnostic(
                    resolution, raw_symbol.Header,
                    f"reflected class '{qualified_name}' base",
                ))
            resolved_base = raw_symbol.BaseQualifiedName
        export_info.Symbols[qualified_name] = replace(
            raw_symbol,
            BaseQualifiedName=resolved_base or "",
            Functions=[dict(function) for function in raw_symbol.Functions],
        )
        for function in export_info.Symbols[qualified_name].Functions:
            signature = function["Signature"]
            function["Signature"] = dict(signature,
                Return=resolve_function_type(signature["Return"], available_symbols, raw_symbol.Namespace),
                Parameters=[dict(p, Type=resolve_function_type(p["Type"], available_symbols, raw_symbol.Namespace))
                            for p in signature["Parameters"]])
            if not validate_function_signature(function["Signature"]):
                raise ValueError(f"DFUNCTION {qualified_name}::{function['Name']}: unsupported resolved function type")
    # The export parse has no inherited C++ semantics. A compatible same-name
    # method remains virtual even when its author omits the override keyword.
    all_symbols = {**dependency_symbols, **export_info.Symbols}
    for symbol in export_info.Symbols.values():
        inherited_virtual = set()
        base_name = symbol.BaseQualifiedName
        visited = set()
        while base_name in all_symbols and base_name not in visited:
            visited.add(base_name)
            base = all_symbols[base_name]
            inherited_virtual.update(f["Name"] for f in base.Functions if f["Virtual"])
            base_name = base.BaseQualifiedName
        symbol.Functions = [dict(f, Virtual=f["Virtual"] or f["Name"] in inherited_virtual)
                            for f in symbol.Functions]
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
