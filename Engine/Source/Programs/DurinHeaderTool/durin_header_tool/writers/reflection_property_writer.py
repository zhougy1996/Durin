
from durin_header_tool.model.export_info import ExportedSymbolInfo
from durin_header_tool.model.reflection_info import (
    ReflectedClassInfo,
    ReflectedEnumInfo,
    ReflectedHeaderInfo,
    ReflectedPropertyInfo,
    ReflectedStructInfo,
)
from durin_header_tool.parser.property_parser import _cpp_type_spelling


ExportedSymbols = dict[str, ExportedSymbolInfo]


PROPERTY_PARAM_BY_KIND = {
    "Int8": "FInt8PropertyParams",
    "Int16": "FInt16PropertyParams",
    "Int32": "FInt32PropertyParams",
    "Int64": "FInt64PropertyParams",
    "UInt8": "FUInt8PropertyParams",
    "UInt16": "FUInt16PropertyParams",
    "UInt32": "FUInt32PropertyParams",
    "UInt64": "FUInt64PropertyParams",
    "Float": "FFloatPropertyParams",
    "Double": "FDoublePropertyParams",
    "Bool": "FBoolPropertyParams",
    "String": "FStringPropertyParams",
    "Name": "FNamePropertyParams",
    "Guid": "FGuidPropertyParams",
    "Byte": "FBytePropertyParams",
    "Blob": "FBlobPropertyParams",
    "BulkData": "FBulkDataPropertyParams",
    "Enum": "FEnumPropertyParams",
    "Object": "FObjectPropertyParams",
    "SoftObject": "FSoftObjectPropertyParams",
    "WeakObject": "FWeakObjectPropertyParams",
    "Array": "FArrayPropertyParams",
    "Map": "FMapPropertyParams",
    "Struct": "FStructPropertyParams",
}

TAB = "\t"


from durin_header_tool.writers.reflection_writer_common import _bool_literal, _cpp_string_literal, _line


def _property_decls(prop: ReflectedPropertyInfo) -> list[str]:
    decls: list[str] = []
    if prop.inner:
        decls.extend(_property_decls(prop.inner))
    if prop.key:
        decls.extend(_property_decls(prop.key))
    if prop.value:
        decls.extend(_property_decls(prop.value))
    if prop.metadata:
        decls.append(_line(f"static const Durin::DurinCodeGen::FMetaDataPair NewProp_{prop.name}_MetaData[];", 1))
    if prop.typed_metadata:
        decls.append(_line(f"static const Durin::FPropertyMetadataParams NewProp_{prop.name}_TypedMetaData;", 1))
    if prop.deprecation:
        decls.append(_line(f"static const char* const NewProp_{prop.name}_MigrationTargets[];", 1))
        decls.append(_line(f"static const Durin::FPropertyDeprecationParams NewProp_{prop.name}_Deprecation;", 1))
    if prop.legacy_names:
        decls.append(_line(f"static const char* const NewProp_{prop.name}_LegacyNames[];", 1))
    param_type = PROPERTY_PARAM_BY_KIND[prop.kind]
    decls.append(_line(f"static const Durin::DurinCodeGen::{param_type} NewProp_{prop.name};", 1))
    return decls


def _property_definitions(class_info: ReflectedClassInfo, prop: ReflectedPropertyInfo, symbols: ExportedSymbols, nested: bool = False) -> str:
    content = []
    if prop.inner:
        content.append(_property_definitions(class_info, prop.inner, symbols, True))
    if prop.key:
        content.append(_property_definitions(class_info, prop.key, symbols, True))
    if prop.value:
        content.append(_property_definitions(class_info, prop.value, symbols, True))
    content.append(_property_definition(class_info, prop, symbols, nested))
    return "".join(content)


def _property_definition(class_info: ReflectedClassInfo, prop: ReflectedPropertyInfo, symbols: ExportedSymbols, nested: bool) -> str:
    content = ""
    if prop.typed_metadata:
        metadata_name = f"{class_info.generated_statics_name}::NewProp_{prop.name}_TypedMetaData"
        typed = prop.typed_metadata
        def string_or_null(value: str) -> str:
            return _cpp_string_literal(value) if value else "nullptr"
        def number(value: str) -> str:
            if not value:
                return "{}"
            if typed.numeric_kind == "Signed":
                literal = "(-9223372036854775807LL - 1)" if value == "-9223372036854775808" else f"{value}LL"
                return f"Durin::FPropertyMetadataNumber::FromSigned({literal})"
            if typed.numeric_kind == "Unsigned":
                return f"Durin::FPropertyMetadataNumber::FromUnsigned({value}ULL)"
            if typed.numeric_kind == "Float":
                literal = value if any(marker in value.lower() for marker in (".", "e")) else f"{value}.0"
                return f"Durin::FPropertyMetadataNumber::FromFloat({literal}f)"
            literal = value if any(marker in value.lower() for marker in (".", "e")) else f"{value}.0"
            return f"Durin::FPropertyMetadataNumber::FromDouble({literal})"
        precision = str(typed.precision) if typed.precision is not None else "-1"
        content += (
            f"const Durin::FPropertyMetadataParams {metadata_name} = {{ "
            f"{string_or_null(typed.display_name)}, {string_or_null(typed.tooltip)}, "
            f"{string_or_null(typed.category)}, Durin::EPropertyUnit::{typed.units or 'None'}, "
            f"{number(typed.step)}, {precision}, {number(typed.clamp_min)}, "
            f"{number(typed.clamp_max)}, {number(typed.ui_min)}, {number(typed.ui_max)} }};\n"
        )
    if prop.deprecation:
        deprecation = prop.deprecation
        targets_name = f"{class_info.generated_statics_name}::NewProp_{prop.name}_MigrationTargets"
        targets = ", ".join(_cpp_string_literal(target) for target in deprecation.migrates_to)
        content += f"const char* const {targets_name}[] = {{ {targets} }};\n"
        deprecation_name = f"{class_info.generated_statics_name}::NewProp_{prop.name}_Deprecation"
        content += (
            f"const Durin::FPropertyDeprecationParams {deprecation_name} = {{ "
            f"{deprecation.custom_version_type}::Guid, "
            f"static_cast<Durin::int32>({deprecation.deprecated_before}), "
            f"static_cast<Durin::int32>({deprecation.custom_version_type}::LatestVersion), "
            f"{_cpp_string_literal(deprecation.historical_name)}, {targets_name}, "
            f"{len(deprecation.migrates_to)} }};\n"
        )
    metadata_ref = "nullptr"
    metadata_count = "0"
    if prop.metadata:
        metadata_name = f"{class_info.generated_statics_name}::NewProp_{prop.name}_MetaData"
        entries = ", ".join(
            f"{{ {_cpp_string_literal(key)}, {_cpp_string_literal(value)} }}" for key, value in prop.metadata
        )
        content += f"const Durin::DurinCodeGen::FMetaDataPair {metadata_name}[] = {{ {entries} }};\n"
        metadata_ref = metadata_name
        metadata_count = str(len(prop.metadata))
    if prop.legacy_names:
        legacy_names_name = f"{class_info.generated_statics_name}::NewProp_{prop.name}_LegacyNames"
        entries = ", ".join(_cpp_string_literal(name) for name in prop.legacy_names)
        content += f"const char* const {legacy_names_name}[] = {{ {entries} }};\n"
    param_type = PROPERTY_PARAM_BY_KIND[prop.kind]
    property_flags = prop.flags
    if property_flags == "None":
        property_flags = "Durin::EPropertyFlags::None"
    offset = "0" if nested else f"static_cast<Durin::uint16>(STRUCT_OFFSET({class_info.qualified_name}, {prop.name}))"
    if prop.kind == "Struct":
        referenced_struct_helper = "nullptr"
        if prop.referenced_struct_type:
            referenced_symbol = symbols.get(prop.referenced_struct_type)
            if referenced_symbol:
                referenced_struct_helper = referenced_symbol.GeneratedHelperName
        metadata_arguments = f", {metadata_ref}, {metadata_count}" if prop.metadata else ""
        initializer = (
            f"{{ \"{prop.name}\", {property_flags}, {prop.array_dim}, {offset}, "
            f"{referenced_struct_helper}{metadata_arguments} }}"
        )
        content += _property_assignment(class_info, prop, param_type, initializer)
        return content
    inner = f"&{class_info.generated_statics_name}::NewProp_{prop.inner.name}" if prop.inner else "nullptr"
    key = f"&{class_info.generated_statics_name}::NewProp_{prop.key.name}" if prop.key else "nullptr"
    value = f"&{class_info.generated_statics_name}::NewProp_{prop.value.name}" if prop.value else "nullptr"
    value_type = (
        _cpp_type_spelling(prop.type_name, symbols, class_info.namespace)
        if nested
        else f"std::remove_extent_t<decltype((({class_info.qualified_name}*)0)->{prop.name})>"
    )
    if prop.kind == "Array":
        metadata_arguments = f", {metadata_ref}, {metadata_count}" if prop.metadata else ""
        initializer = (
            f"{{ \"{prop.name}\", {property_flags}, {prop.array_dim}, {offset}, {inner}, "
            f"&Durin::ResolveArrayOps<{value_type}>{metadata_arguments} }}"
        )
        content += _property_assignment(class_info, prop, param_type, initializer)
        return content
    if prop.kind == "Map":
        metadata_arguments = f", {metadata_ref}, {metadata_count}" if prop.metadata else ""
        initializer = (
            f"{{ \"{prop.name}\", {property_flags}, {prop.array_dim}, {offset}, {key}, {value}, "
            f"&Durin::ResolveMapOps<{value_type}>{metadata_arguments} }}"
        )
        content += _property_assignment(class_info, prop, param_type, initializer)
        return content
    if prop.kind in ("SoftObject", "WeakObject"):
        referenced_class_helper = "nullptr"
        if prop.referenced_type:
            referenced_symbol = symbols.get(prop.referenced_type)
            if referenced_symbol:
                referenced_class_helper = referenced_symbol.GeneratedHelperName
        metadata_arguments = f", {metadata_ref}, {metadata_count}" if prop.metadata else ""
        initializer = (
            f"Durin::DurinCodeGen::{param_type}::Create<{value_type}>("
            f"\"{prop.name}\", {property_flags}, {prop.array_dim}, {offset}, "
            f"{referenced_class_helper}{metadata_arguments})"
        )
        content += _property_assignment(class_info, prop, param_type, initializer)
        return content
    metadata_arguments = f", {metadata_ref}, {metadata_count}" if prop.metadata else ""
    if prop.kind == "Enum":
        referenced_enum_helper = "nullptr"
        if prop.referenced_enum_type:
            referenced_symbol = symbols.get(prop.referenced_enum_type)
            if referenced_symbol:
                referenced_enum_helper = referenced_symbol.GeneratedHelperName
        initializer = (
            f"{{ \"{prop.name}\", {property_flags}, {prop.array_dim}, {offset}, "
            f"{referenced_enum_helper}{metadata_arguments} }}"
        )
        content += _property_assignment(class_info, prop, param_type, initializer)
        return content
    if prop.kind == "Object":
        referenced_class_helper = "nullptr"
        if prop.referenced_type:
            referenced_symbol = symbols.get(prop.referenced_type)
            if referenced_symbol:
                referenced_class_helper = referenced_symbol.GeneratedHelperName
        target_type = _cpp_type_spelling(prop.referenced_type, symbols)
        factory = "ObjectPtr" if prop.is_object_ptr_wrapper else "Raw"
        initializer = (
            f"Durin::DurinCodeGen::{param_type}::{factory}<{target_type}>("
            f"\"{prop.name}\", {property_flags}, {prop.array_dim}, {offset}, "
            f"{referenced_class_helper}{metadata_arguments})"
        )
        content += _property_assignment(class_info, prop, param_type, initializer)
        return content
    if prop.kind == "BulkData":
        initializer = (
            f"Durin::DurinCodeGen::{param_type}::Create<{value_type}>("
            f"\"{prop.name}\", {property_flags}, {prop.array_dim}, {offset}{metadata_arguments})"
        )
        content += _property_assignment(class_info, prop, param_type, initializer)
        return content
    initializer = f"{{ \"{prop.name}\", {property_flags}, {prop.array_dim}, {offset}{metadata_arguments} }}"
    content += _property_assignment(class_info, prop, param_type, initializer)
    return content


def _property_assignment(
    class_info: ReflectedClassInfo,
    prop: ReflectedPropertyInfo,
    param_type: str,
    initializer: str,
) -> str:
    if prop.legacy_names:
        if initializer.startswith("{"):
            initializer = f"Durin::DurinCodeGen::{param_type}{initializer}"
        legacy_names = f"{class_info.generated_statics_name}::NewProp_{prop.name}_LegacyNames"
        initializer = (
            f"Durin::DurinCodeGen::WithLegacyNames({initializer}, {legacy_names}, "
            f"{len(prop.legacy_names)})"
        )
    if prop.typed_metadata:
        if initializer.startswith("{"):
            initializer = f"Durin::DurinCodeGen::{param_type}{initializer}"
        typed_metadata = f"{class_info.generated_statics_name}::NewProp_{prop.name}_TypedMetaData"
        initializer = f"Durin::DurinCodeGen::WithTypedMetadata({initializer}, &{typed_metadata})"
    if prop.deprecation:
        if initializer.startswith("{"):
            initializer = f"Durin::DurinCodeGen::{param_type}{initializer}"
        deprecation = f"{class_info.generated_statics_name}::NewProp_{prop.name}_Deprecation"
        initializer = f"Durin::DurinCodeGen::WithDeprecation({initializer}, &{deprecation})"
    return (
        f"const Durin::DurinCodeGen::{param_type} "
        f"{class_info.generated_statics_name}::NewProp_{prop.name} = {initializer};\n"
    )
