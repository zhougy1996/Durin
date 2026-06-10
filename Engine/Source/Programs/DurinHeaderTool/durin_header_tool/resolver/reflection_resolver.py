import logging

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.model.export_info import load_module_export_file
from durin_header_tool.model.reflection_info import ReflectedHeaderInfo


def load_available_symbols(module_name: str) -> dict[str, object]:
    symbols: dict[str, object] = {}
    dep_modules = configs.collect_all_dependent_module_with_export_file(module_name)
    logging.info("[DHT] Reflection %s: loading exports from %d modules", module_name, len(dep_modules))
    for dep_module in dep_modules:
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for module '{dep_module}' not found at expected path: {export_file_path}")
        export_info = load_module_export_file(export_file_path)
        symbols.update(export_info.Symbols)
    logging.info("[DHT] Reflection %s: loaded %d reflected symbols", module_name, len(symbols))
    return symbols


def resolve_header_symbols(header: ReflectedHeaderInfo, symbols: dict[str, object]) -> None:
    for class_info in header.classes:
        class_info.base_qualified_name = _resolve_short_symbol(class_info.base_qualified_name, symbols)
        for prop in class_info.properties:
            prop.referenced_type = _resolve_short_symbol(prop.referenced_type, symbols)


def resolved_symbol_dependencies_for_header(header_info: ReflectedHeaderInfo, symbols: dict[str, object]) -> dict[str, dict[str, str]]:
    dependencies: dict[str, dict[str, str]] = {}
    for class_info in header_info.classes:
        if class_info.base_qualified_name in symbols:
            dependencies[class_info.base_qualified_name] = symbol_dependency_snapshot(symbols[class_info.base_qualified_name])
        for prop in class_info.properties:
            if prop.referenced_type in symbols:
                dependencies[prop.referenced_type] = symbol_dependency_snapshot(symbols[prop.referenced_type])
    return dependencies


def header_symbol_dependencies_changed(header: str, old_manifest, symbols: dict[str, object]) -> bool:
    if header not in old_manifest.resolved_symbol_dependencies:
        return True
    for symbol_name, old_snapshot in old_manifest.resolved_symbol_dependencies[header].items():
        current_symbol = symbols.get(symbol_name)
        if current_symbol is None:
            return True
        if symbol_dependency_snapshot(current_symbol) != old_snapshot:
            return True
    return False


def symbol_dependency_snapshot(symbol: object) -> dict[str, str]:
    return {
        "GeneratedHelperName": getattr(symbol, "GeneratedHelperName", ""),
        "API": getattr(symbol, "API", ""),
        "BaseQualifiedName": getattr(symbol, "BaseQualifiedName", ""),
    }


def _resolve_short_symbol(short_or_qualified_name: str, symbols: dict[str, object]) -> str:
    if not short_or_qualified_name or "::" in short_or_qualified_name:
        return short_or_qualified_name
    matches = [qualified_name for qualified_name, symbol in symbols.items() if getattr(symbol, "ShortName", "") == short_or_qualified_name]
    return matches[0] if len(matches) == 1 else short_or_qualified_name
