import logging
from dataclasses import dataclass

from durin_header_tool import config as configs
from durin_header_tool import io as utils
from durin_header_tool.model.export_info import ExportedSymbolInfo, load_module_export_file
from durin_header_tool.model.reflection_info import ReflectedHeaderInfo
from durin_header_tool.model.reflection_info import namespace_path_from_name


ExportedSymbols = dict[str, ExportedSymbolInfo]


@dataclass(frozen=True)
class SymbolResolution:
    spelling: str
    declaring_namespace: str
    kinds: tuple[str, ...]
    qualified_name: str = ""
    attempted_names: tuple[str, ...] = ()
    candidates: tuple[str, ...] = ()

    @property
    def resolved(self) -> bool:
        return bool(self.qualified_name)

    @property
    def ambiguous(self) -> bool:
        # A context-free spelling with several exported matches is genuinely
        # ambiguous. In a declaring namespace those unrelated matches are
        # diagnostic suggestions, not lexically viable candidates.
        return (
            not self.resolved
            and not self.declaring_namespace
            and len(self.candidates) > 1
        )


def _add_builtin_symbols(symbols: ExportedSymbols) -> None:
    for short_name in (
        "FVector2f", "FVector3f", "FVector4f",
        "FVector2", "FVector3", "FVector4",
        "FQuatf", "FQuat", "FMatrix4f", "FTransform", "FLinearColor",
    ):
        qualified_name = f"Durin::{short_name}"
        symbols.setdefault(qualified_name, ExportedSymbolInfo(
            Kind="struct", ShortName=short_name, Namespace="Durin", QualifiedName=qualified_name,
            NamespacePath=namespace_path_from_name("Durin"),
            Header="DObject/MathStructs.h", API="COREDOBJECT_API"
        ))
    for alias_name, canonical_name in (
        ("FVector2d", "FVector2"),
        ("FVector3d", "FVector3"),
        ("FVector4d", "FVector4"),
        ("FQuatd", "FQuat"),
    ):
        symbols.setdefault(f"Durin::{alias_name}", symbols[f"Durin::{canonical_name}"])


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
        if class_info.base_qualified_name:
            resolution = resolve_symbol(
                class_info.base_qualified_name, symbols,
                declaring_namespace=class_info.namespace, kinds=("class",),
            )
            if not resolution.resolved:
                raise ValueError(symbol_resolution_diagnostic(
                    resolution, header.header,
                    f"reflected class '{class_info.qualified_name}' base",
                ))
            class_info.base_qualified_name = resolution.qualified_name
        for prop in class_info.properties:
            _resolve_property_symbols(prop, symbols, class_info.namespace, header.header, class_info.qualified_name)
    for struct_info in header.structs:
        for prop in struct_info.properties:
            _resolve_property_symbols(prop, symbols, struct_info.namespace, header.header, struct_info.qualified_name)


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
        "GeneratedHelperReference": symbol.generated_symbol.helper_reference,
        "NamespacePath": "/".join(
            ("inline:" if segment.is_inline else "namespace:") + segment.name
            for segment in symbol.NamespacePath
        ),
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
    declaring_namespace: str = "",
    kinds: tuple[str, ...] = ("class", "enum", "struct"),
) -> str | None:
    resolution = resolve_symbol(
        short_or_qualified_name, symbols,
        declaring_namespace=declaring_namespace, kinds=kinds,
    )
    return resolution.qualified_name or None


def resolve_symbol(
    spelling: str,
    symbols: ExportedSymbols,
    *,
    declaring_namespace: str = "",
    kinds: tuple[str, ...] = ("class", "enum", "struct"),
) -> SymbolResolution:
    normalized = spelling.strip()
    globally_qualified = normalized.startswith("::")
    normalized = normalized[2:] if globally_qualified else normalized
    namespace_parts = [part for part in declaring_namespace.split("::") if part]

    if not normalized:
        attempted: tuple[str, ...] = ()
    elif globally_qualified:
        attempted = (normalized,)
    else:
        names: list[str] = []
        # An already fully qualified exported identity is authoritative.
        if "::" in normalized and normalized in symbols:
            names.append(normalized)
        for length in range(len(namespace_parts), -1, -1):
            candidate = "::".join((*namespace_parts[:length], normalized))
            if candidate not in names:
                names.append(candidate)
        attempted = tuple(names)

    for candidate_name in attempted:
        candidate = symbols.get(candidate_name)
        if candidate is not None and candidate.Kind in kinds:
            return SymbolResolution(
                spelling, declaring_namespace, kinds,
                qualified_name=candidate_name, attempted_names=attempted,
            )

    short_name = normalized.rsplit("::", 1)[-1]
    candidates = tuple(sorted(
        qualified_name for qualified_name, candidate in symbols.items()
        if candidate.Kind in kinds and candidate.ShortName == short_name
    ))
    return SymbolResolution(
        spelling, declaring_namespace, kinds,
        attempted_names=attempted, candidates=candidates,
    )


def symbol_resolution_diagnostic(resolution: SymbolResolution, header: str, subject: str) -> str:
    allowed = ", ".join(sorted(resolution.kinds))
    attempted = ", ".join(resolution.attempted_names) or "<none>"
    candidates = ", ".join(resolution.candidates) or "<none>"
    category = "ambiguous" if resolution.ambiguous else "unresolved"
    return (
        f"{header}: {subject} has {category} reflected type spelling '{resolution.spelling}' "
        f"from namespace '{resolution.declaring_namespace or '::'}' (allowed kinds: {allowed}); "
        f"lexical lookup: {attempted}; candidates: {candidates}"
    )


def _resolve_property_symbols(prop, symbols: ExportedSymbols, namespace: str, header: str, owner: str) -> None:
    for attribute, kinds in (
        ("referenced_type", ("class",)),
        ("referenced_enum_type", ("enum",)),
        ("referenced_struct_type", ("struct",)),
    ):
        spelling = getattr(prop, attribute)
        if not spelling:
            continue
        resolution = resolve_symbol(spelling, symbols, declaring_namespace=namespace, kinds=kinds)
        if not resolution.resolved:
            raise ValueError(symbol_resolution_diagnostic(
                resolution, header, f"'{owner}::{prop.name}' property",
            ))
        setattr(prop, attribute, resolution.qualified_name)
    if prop.inner:
        _resolve_property_symbols(prop.inner, symbols, namespace, header, owner)
    if prop.key:
        _resolve_property_symbols(prop.key, symbols, namespace, header, owner)
    if prop.value:
        _resolve_property_symbols(prop.value, symbols, namespace, header, owner)


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
