from dataclasses import dataclass
from pathlib import Path
import re
from typing import TypeAlias

import clang.cindex

from durin_header_tool import config as configs
from durin_header_tool.model.export_info import ExportedSymbolInfo
from durin_header_tool.model.reflection_info import (
    ReflectedClassInfo,
    ReflectedEnumInfo,
    ReflectedEnumValueInfo,
    ReflectedHeaderInfo,
    ReflectedPropertyInfo,
    ReflectedStructInfo,
    make_generated_enum_helper_name,
    make_generated_helper_name,
    make_generated_struct_helper_name,
)
from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner
from durin_header_tool.resolver.reflection_resolver import (
    resolve_symbol,
    resolve_symbol_name,
    symbol_resolution_diagnostic,
)

ExportedSymbols: TypeAlias = dict[str, ExportedSymbolInfo]
MAX_CONTAINER_PROPERTY_DEPTH = 4

from durin_header_tool.parser.annotation_rewriter import (
    _DMetaUse,
    _class_specifiers_from_payload,
    _dmeta_use_id,
    _make_dht_parse_source,
    make_dht_parse_source,
)
from durin_header_tool.parser.reflection_ast_helpers import (
    _base_qualified_name,
    _cursor_source_line_range,
    _get_annotation,
    _is_default_constructor,
    _is_object_initializer_constructor,
    _qualified_name,
    _scan_generated_body_line,
    _semantic_namespace,
    _source_base_name,
)
from durin_header_tool.parser.property_parser import (
    _cpp_type_spelling,
    _make_property,
    _make_property_from_spelling,
    _make_property_from_type,
    _normalize_type_spelling,
    _scan_source_properties_for_class,
    _string_metadata_from_annotation,
    _string_list_metadata_from_annotation,
    _underlying_kind_from_type_spelling,
    _validate_explicit_container_spelling,
    _validate_soft_object_spelling,
)
from durin_header_tool.parser.clang_context import (
    PARSER_CONTEXT_VERSION,
    _file_id_for_header,
    _include_path_for_header,
    _parse_translation_unit,
    _validate_preprocessor_context,
)








def _is_scoped_enum(enum_cursor: clang.cindex.Cursor) -> bool:
    return bool(enum_cursor.is_scoped_enum())




def _make_enum(
    enum_cursor: clang.cindex.Cursor,
    module_config,
    header: str,
    annotation: str,
    consumed_dmeta_uses: set[int],
) -> ReflectedEnumInfo:
    qualified_name = _qualified_name(enum_cursor)
    underlying_type = _normalize_type_spelling(enum_cursor.enum_type.spelling)
    values: list[ReflectedEnumValueInfo] = []
    for child in enum_cursor.get_children():
        if child.kind == clang.cindex.CursorKind.ENUM_CONSTANT_DECL:
            value_annotation = _get_annotation(child)
            display_name = ""
            dmeta_use_id = _dmeta_use_id(value_annotation)
            if dmeta_use_id is not None:
                display_name = _string_metadata_from_annotation(value_annotation, "DisplayName")
                consumed_dmeta_uses.add(dmeta_use_id)
            values.append(
                ReflectedEnumValueInfo(
                    name=child.spelling,
                    value=int(child.enum_value),
                    display_name=display_name,
                )
            )
    return ReflectedEnumInfo(
        short_name=enum_cursor.spelling,
        namespace=_semantic_namespace(enum_cursor),
        qualified_name=qualified_name,
        generated_helper_name=make_generated_enum_helper_name(qualified_name),
        header=header,
        api=module_config.api_macro,
        is_scoped=_is_scoped_enum(enum_cursor),
        underlying_type=underlying_type,
        underlying_kind=_underlying_kind_from_type_spelling(underlying_type),
        underlying_size=max(int(enum_cursor.enum_type.get_size() or 0), 0),
        display_name=_string_metadata_from_annotation(annotation, "DisplayName"),
        legacy_names=_string_list_metadata_from_annotation(annotation, "LegacyNames"),
        values=values,
    )






def _validate_property_legacy_names(
    owner_name: str, properties: list[ReflectedPropertyInfo]
) -> None:
    current_names = {prop.name for prop in properties}
    legacy_owners: dict[str, str] = {}
    for prop in properties:
        for legacy_name in prop.legacy_names:
            if legacy_name == prop.name:
                raise ValueError(
                    f"reflected type '{owner_name}' property '{prop.name}': "
                    "LegacyNames must not contain the current property name"
                )
            if legacy_name in current_names:
                raise ValueError(
                    f"reflected type '{owner_name}' property '{prop.name}': legacy name "
                    f"'{legacy_name}' collides with a current property name"
                )
            previous = legacy_owners.get(legacy_name)
            if previous is not None:
                raise ValueError(
                    f"reflected type '{owner_name}' properties '{previous}' and "
                    f"'{prop.name}' share legacy name '{legacy_name}'"
                )
            legacy_owners[legacy_name] = prop.name


def _validate_property_deprecations(owner_name: str, properties: list[ReflectedPropertyInfo]) -> None:
    current = {prop.name: prop for prop in properties}
    routes: set[str] = set()
    for prop in properties:
        route = prop.deprecation
        if not route:
            continue
        if route.historical_name in routes:
            raise ValueError(
                f"reflected type '{owner_name}' has duplicate deprecated route '{route.historical_name}'"
            )
        routes.add(route.historical_name)
        for target in route.migrates_to:
            target_prop = current.get(target)
            if not target_prop:
                raise ValueError(
                    f"reflected type '{owner_name}' deprecated property '{prop.name}' targets missing property '{target}'"
                )
            if target_prop.deprecation:
                raise ValueError(
                    f"reflected type '{owner_name}' deprecated property '{prop.name}' targets deprecated property '{target}'"
                )


def parse_reflection_header(
    module_name: str,
    header: str,
    exported_symbols: ExportedSymbols | None = None,
    export_mode: bool = False,
) -> ReflectedHeaderInfo:
    module_config = configs.get_module_config(module_name)
    header_path = (module_config.module_dir / header).resolve()
    source = header_path.read_text(encoding="utf-8")
    _validate_preprocessor_context(source)
    tu, dmeta_uses = _parse_translation_unit(
        module_name,
        header,
        header_path,
        source,
        export_mode,
        exported_symbols,
    )

    classes: list[ReflectedClassInfo] = []
    enums: list[ReflectedEnumInfo] = []
    structs: list[ReflectedStructInfo] = []
    consumed_dmeta_uses: set[int] = set()

    def visit(parent: clang.cindex.Cursor) -> None:
        children = list(parent.get_children())
        pending_dclass_annotation = ""
        pending_dstruct_annotation = ""
        pending_denum_annotation = ""
        for child in children:
            if child.location.file is None or Path(str(child.location.file)) != header_path:
                continue

            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling.startswith("DHT_CLASS_"):
                annotation = _get_annotation(child)
                pending_dclass_annotation = annotation if annotation.startswith("DCLASS") else ""
                continue
            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling.startswith("DHT_STRUCT_"):
                annotation = _get_annotation(child)
                pending_dstruct_annotation = annotation if annotation.startswith("DSTRUCT") else ""
                continue
            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling.startswith("DHT_ENUM_"):
                annotation = _get_annotation(child)
                pending_denum_annotation = annotation if annotation.startswith("DENUM") else ""
                continue

            if pending_dstruct_annotation and child.kind == clang.cindex.CursorKind.STRUCT_DECL and child.spelling:
                qualified_name = _qualified_name(child)
                declaring_namespace = _semantic_namespace(child)
                reflected_struct = ReflectedStructInfo(
                    short_name=child.spelling,
                    namespace=declaring_namespace,
                    qualified_name=qualified_name,
                    generated_helper_name=make_generated_struct_helper_name(qualified_name),
                    header=header,
                    api=module_config.api_macro,
                    generated_body_line=_scan_generated_body_line(source, child),
                    legacy_names=_string_list_metadata_from_annotation(
                        pending_dstruct_annotation, "LegacyNames"
                    ),
                )
                for member in child.get_children():
                    if member.kind == clang.cindex.CursorKind.FIELD_DECL:
                        prop = _make_property(
                            member, exported_symbols, source, declaring_namespace
                        )
                        if prop:
                            reflected_struct.properties.append(prop)
                existing_property_names = {prop.name for prop in reflected_struct.properties}
                for prop in _scan_source_properties_for_class(
                    source,
                    child,
                    exported_symbols,
                    existing_property_names,
                    reject_unsupported=not export_mode,
                    declaring_namespace=declaring_namespace,
                ):
                    if prop.name not in existing_property_names:
                        reflected_struct.properties.append(prop)
                        existing_property_names.add(prop.name)
                _validate_property_legacy_names(
                    reflected_struct.qualified_name, reflected_struct.properties
                )
                _validate_property_deprecations(
                    reflected_struct.qualified_name, reflected_struct.properties
                )
                structs.append(reflected_struct)
                pending_dstruct_annotation = ""
                continue

            if pending_dclass_annotation and child.kind in (clang.cindex.CursorKind.CLASS_DECL, clang.cindex.CursorKind.STRUCT_DECL) and child.spelling:
                qualified_name = _qualified_name(child)
                declaring_namespace = _semantic_namespace(child)
                helper_name = make_generated_helper_name(qualified_name)
                class_payload = pending_dclass_annotation.split(",", 1)[1] if "," in pending_dclass_annotation else ""
                class_specifiers = _class_specifiers_from_payload(
                    class_payload, child.location.line, child.location.column
                )
                source_base_name = _source_base_name(source, child)
                ast_base_name = _base_qualified_name(child)
                base_qualified_name = source_base_name or ast_base_name
                if exported_symbols and source_base_name and ast_base_name:
                    source_base_identity = resolve_symbol_name(
                        source_base_name, exported_symbols,
                        declaring_namespace=declaring_namespace, kinds=("class",),
                    )
                    ast_base_identity = resolve_symbol_name(
                        ast_base_name, exported_symbols,
                        declaring_namespace=declaring_namespace, kinds=("class",),
                    )
                    if (
                        source_base_identity and ast_base_identity
                        and source_base_identity != ast_base_identity
                    ):
                        raise ValueError(
                            f"{header}: reflected class '{qualified_name}' base AST/source "
                            f"identity disagreement: source={source_base_identity}, "
                            f"AST={ast_base_identity}"
                        )
                    base_qualified_name = ast_base_identity or source_base_identity or base_qualified_name
                reflected_class = ReflectedClassInfo(
                    short_name=child.spelling,
                    namespace=declaring_namespace,
                    qualified_name=qualified_name,
                    generated_helper_name=helper_name,
                    header=header,
                    api=module_config.api_macro,
                    base_qualified_name=base_qualified_name,
                    generated_body_line=_scan_generated_body_line(source, child),
                    is_abstract=class_specifiers.is_abstract,
                    no_class_default_object=class_specifiers.no_class_default_object,
                    display_name=class_specifiers.display_name,
                    default_object_name=class_specifiers.default_object_name,
                    legacy_names=list(class_specifiers.legacy_names),
                )
                for member in child.get_children():
                    if _is_default_constructor(member):
                        reflected_class.has_default_constructor = True
                    elif _is_object_initializer_constructor(member):
                        reflected_class.has_object_initializer_constructor = True
                    elif member.kind == clang.cindex.CursorKind.DESTRUCTOR:
                        reflected_class.has_destructor = True
                    elif member.kind == clang.cindex.CursorKind.FIELD_DECL:
                        prop = _make_property(
                            member, exported_symbols, source, declaring_namespace
                        )
                        if prop:
                            reflected_class.properties.append(prop)
                existing_property_names = {prop.name for prop in reflected_class.properties}
                for prop in _scan_source_properties_for_class(
                    source,
                    child,
                    exported_symbols,
                    existing_property_names,
                    reject_unsupported=not export_mode,
                    declaring_namespace=declaring_namespace,
                ):
                    if prop.name not in existing_property_names:
                        reflected_class.properties.append(prop)
                        existing_property_names.add(prop.name)
                _validate_property_legacy_names(
                    reflected_class.qualified_name, reflected_class.properties
                )
                _validate_property_deprecations(
                    reflected_class.qualified_name, reflected_class.properties
                )
                classes.append(reflected_class)
                pending_dclass_annotation = ""
                continue

            if pending_denum_annotation and child.kind == clang.cindex.CursorKind.ENUM_DECL and child.spelling:
                enums.append(
                    _make_enum(
                        child,
                        module_config,
                        header,
                        pending_denum_annotation,
                        consumed_dmeta_uses,
                    )
                )
                pending_denum_annotation = ""
                continue

            if child.kind in (clang.cindex.CursorKind.NAMESPACE, clang.cindex.CursorKind.TRANSLATION_UNIT):
                visit(child)

    visit(tu.cursor)

    unconsumed_dmeta_uses = dmeta_uses.keys() - consumed_dmeta_uses
    if unconsumed_dmeta_uses:
        use = dmeta_uses[min(unconsumed_dmeta_uses)]
        raise ValueError(
            f"DMETA at line {use.line}, column {use.column}: "
            "annotation is only valid on an enumerator in a reflected enum"
        )

    rel = Path(header)
    include_path = _include_path_for_header(header)
    file_id = _file_id_for_header(module_name, header)
    return ReflectedHeaderInfo(module_name, header, header_path, include_path, file_id, classes, enums, structs)
