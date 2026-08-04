import logging

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.model.export_info import ExportedSymbolInfo, load_module_export_file
from durin_header_tool.model.reflection_info import ReflectedHeaderInfo


ExportedSymbols = dict[str, ExportedSymbolInfo]


def _add_builtin_symbols(symbols: ExportedSymbols) -> None:
    for qualified_name, short_name in (
        ("Durin::FVector2", "FVector2"),
        ("Durin::FVector3", "FVector3"),
        ("Durin::FVector4", "FVector4"),
        ("Durin::FQuat", "FQuat"),
        ("Durin::FTransform", "FTransform"),
        ("Durin::FLinearColor", "FLinearColor"),
    ):
        symbols.setdefault(qualified_name, ExportedSymbolInfo(
            Kind="struct", ShortName=short_name, Namespace="Durin", QualifiedName=qualified_name,
            GeneratedHelperName=f"Z_Construct_DStruct_{qualified_name.replace('::', '_')}",
            Header="DObject/MathStructs.h", API="COREDOBJECT_API"
        ))


def load_dependency_symbols(module_name: str) -> ExportedSymbols:
    symbols: ExportedSymbols = {}
    dep_modules = sorted(
        dep_module
        for dep_module in configs.collect_all_dependent_modules(module_name)
        if configs.get_module_config(dep_module).has_export_file()
    )
    logging.debug("[DHT] Export %s: loading exports from %d dependencies", module_name, len(dep_modules))
    for dep_module in dep_modules:
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for module '{dep_module}' not found at expected path: {export_file_path}")
        export_info = load_module_export_file(export_file_path)
        symbols.update(export_info.Symbols)
    _add_builtin_symbols(symbols)
    return symbols


def load_available_symbols(module_name: str) -> ExportedSymbols:
    symbols = load_dependency_symbols(module_name)
    if module_name in configs.collect_all_dependent_module_with_export_file(module_name):
        export_file_path = utils.get_module_export_file_path(module_name)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for module '{module_name}' not found at expected path: {export_file_path}")
        symbols.update(load_module_export_file(export_file_path).Symbols)
    logging.debug("[DHT] Reflection %s: loaded %d reflected symbols", module_name, len(symbols))
    return symbols


def resolve_header_symbols(header: ReflectedHeaderInfo, symbols: ExportedSymbols) -> None:
    for class_info in header.classes:
        class_info.base_qualified_name = _resolve_short_symbol(class_info.base_qualified_name, symbols)
        if class_info.base_qualified_name and class_info.base_qualified_name not in symbols:
            raise ValueError(
                f"{header.header}: reflected class '{class_info.qualified_name}' has unsupported "
                f"non-hermetic base type '{class_info.base_qualified_name}'"
            )
        for prop in class_info.properties:
            _resolve_property_symbols(prop, symbols)
    for struct_info in header.structs:
        for prop in struct_info.properties:
            _resolve_property_symbols(prop, symbols)


def resolved_symbol_dependencies_for_header(header_info: ReflectedHeaderInfo, symbols: ExportedSymbols) -> dict[str, dict[str, str]]:
    dependencies: dict[str, dict[str, str]] = {}
    for class_info in header_info.classes:
        if class_info.base_qualified_name in symbols:
            dependencies[class_info.base_qualified_name] = symbol_dependency_snapshot(symbols[class_info.base_qualified_name])
        for prop in class_info.properties:
            _collect_property_dependencies(prop, symbols, dependencies)
    for struct_info in header_info.structs:
        for prop in struct_info.properties:
            _collect_property_dependencies(prop, symbols, dependencies)
    return dependencies


def symbol_dependency_snapshot(symbol: ExportedSymbolInfo) -> dict[str, str]:
    return {
        "GeneratedHelperName": symbol.GeneratedHelperName,
        "API": symbol.API,
        "BaseQualifiedName": symbol.BaseQualifiedName,
        "Kind": symbol.Kind,
        "UnderlyingKind": symbol.UnderlyingKind,
        "UnderlyingType": symbol.UnderlyingType,
    }


def resolve_symbol_name(
    short_or_qualified_name: str,
    symbols: ExportedSymbols,
    *,
    kinds: tuple[str, ...] = ("class", "enum", "struct"),
) -> str | None:
    if "::" in short_or_qualified_name:
        symbol = symbols.get(short_or_qualified_name)
        return (
            short_or_qualified_name
            if symbol is not None and symbol.Kind in kinds
            else None
        )
    matches = [
        qualified_name
        for qualified_name, candidate in symbols.items()
        if candidate.Kind in kinds and candidate.ShortName == short_or_qualified_name
    ]
    return matches[0] if len(matches) == 1 else None


def _resolve_short_symbol(short_or_qualified_name: str, symbols: ExportedSymbols) -> str:
    if not short_or_qualified_name or "::" in short_or_qualified_name:
        return short_or_qualified_name
    return resolve_symbol_name(short_or_qualified_name, symbols) or short_or_qualified_name


def _resolve_property_symbols(prop, symbols: ExportedSymbols) -> None:
    prop.referenced_type = _resolve_short_symbol(prop.referenced_type, symbols)
    prop.referenced_enum_type = _resolve_short_symbol(prop.referenced_enum_type, symbols)
    prop.referenced_struct_type = _resolve_short_symbol(prop.referenced_struct_type, symbols)
    if prop.inner:
        _resolve_property_symbols(prop.inner, symbols)
    if prop.key:
        _resolve_property_symbols(prop.key, symbols)
    if prop.value:
        _resolve_property_symbols(prop.value, symbols)


def _collect_property_dependencies(prop, symbols: ExportedSymbols, dependencies: dict[str, dict[str, str]]) -> None:
    if prop.referenced_type in symbols:
        dependencies[prop.referenced_type] = symbol_dependency_snapshot(symbols[prop.referenced_type])
    if prop.referenced_enum_type in symbols:
        dependencies[prop.referenced_enum_type] = symbol_dependency_snapshot(symbols[prop.referenced_enum_type])
    if prop.referenced_struct_type in symbols:
        dependencies[prop.referenced_struct_type] = symbol_dependency_snapshot(symbols[prop.referenced_struct_type])
    if prop.inner:
        _collect_property_dependencies(prop.inner, symbols, dependencies)
    if prop.key:
        _collect_property_dependencies(prop.key, symbols, dependencies)
    if prop.value:
        _collect_property_dependencies(prop.value, symbols, dependencies)
