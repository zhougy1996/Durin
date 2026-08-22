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
    "Enum": "FEnumPropertyParams",
    "Object": "FObjectPropertyParams",
    "SoftObject": "FSoftObjectPropertyParams",
    "WeakObject": "FWeakObjectPropertyParams",
    "Array": "FArrayPropertyParams",
    "Map": "FMapPropertyParams",
    "Struct": "FStructPropertyParams",
}

TAB = "\t"


from durin_header_tool.writers.reflection_header_writer import generate_header_content
from durin_header_tool.writers.reflection_property_writer import _property_decls, _property_definitions
from durin_header_tool.writers.reflection_writer_common import _base_name_for_macro, _bool_literal, _constructor_mode, _cpp_string_literal, _line


def generate_cpp_content(header: ReflectedHeaderInfo, symbols: ExportedSymbols) -> str:
    builder: list[str] = [
        "// Generated code exported from DurinHeaderTool.\n\n",
        '#include "DObject/GeneratedCppIncludes.h"\n',
        f'#include "{header.include_path}"\n\n',
    ]
    # All reflected types in a module share its compiled-in C++ package.
    package_path = f"/Cpp/{header.module_name}"

    referenced_class_helpers: dict[str, str] = {}
    referenced_enum_helpers: dict[str, str] = {}
    referenced_struct_helpers: dict[str, str] = {}
    for class_info in header.classes:
        if class_info.base_qualified_name and class_info.base_qualified_name != class_info.qualified_name:
            symbol = symbols.get(class_info.base_qualified_name)
            if symbol:
                referenced_class_helpers[symbol.GeneratedHelperName] = symbol.API
        for prop in class_info.properties:
            _collect_referenced_helpers(prop, symbols, referenced_class_helpers, referenced_enum_helpers, referenced_struct_helpers)
    for struct_info in header.structs:
        for prop in struct_info.properties:
            _collect_referenced_helpers(prop, symbols, referenced_class_helpers, referenced_enum_helpers, referenced_struct_helpers)

    for helper, api in sorted(referenced_class_helpers.items()):
        builder.append(f"{api} Durin::DClass* {helper}();\n")
    for helper, api in sorted(referenced_enum_helpers.items()):
        builder.append(f"{api} Durin::DEnum* {helper}();\n")
    for helper, api in sorted(referenced_struct_helpers.items()):
        builder.append(f"{api} Durin::DStruct* {helper}();\n")
    if referenced_class_helpers or referenced_enum_helpers or referenced_struct_helpers:
        builder.append("\n")

    for enum_info in header.enums:
        builder.append(_enum_definitions(enum_info, package_path))

    for struct_info in header.structs:
        builder.append(_struct_definitions(struct_info, symbols, package_path))

    for class_info in header.classes:
        properties = class_info.properties
        has_properties = bool(properties)
        generated_statics_name = class_info.generated_statics_name
        constructor_mode = _constructor_mode(class_info)
        class_flags = []
        if class_info.is_abstract:
            class_flags.append("Durin::EClassFlags::Abstract")
        if class_info.no_class_default_object:
            class_flags.append("Durin::EClassFlags::NoClassDefaultObject")
        class_flags_expression = " | ".join(class_flags) if class_flags else "Durin::EClassFlags::None"
        builder.append(f"Durin::FClassRegistrationInfo {class_info.registration_info_name};\n\n")

        _append_lines(
            builder,
            (f"Durin::DClass* {class_info.qualified_name}::GetPrivateStaticClass()", 0),
            ("{", 0),
            (f"if (!{class_info.registration_info_name}.InnerSingleton)", 1),
            ("{", 1),
            ("Durin::GetPrivateStaticClassBody(", 2),
            (f"\"{package_path}\",", 3),
            (f"\"{class_info.qualified_name}\",", 3),
            (f"{class_info.registration_info_name}.InnerSingleton,", 3),
            ("nullptr,", 3),
            (f"sizeof({class_info.qualified_name}),", 3),
            (f"alignof({class_info.qualified_name}),", 3),
            (f"{class_flags_expression},", 3),
            (
                "nullptr," if class_info.is_abstract
                else f"(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<{class_info.qualified_name}>,",
                3,
            ),
            (f"&{_base_name_for_macro(class_info)}::StaticClass", 3),
            (");", 2),
            ("}", 1),
            (f"return {class_info.registration_info_name}.InnerSingleton;", 1),
            ("}", 0),
            ("", 0),
        )

        _append_lines(
            builder,
            (f"Durin::DClass* {class_info.generated_helper_no_register_name}()", 0),
            ("{", 0),
            (f"return {class_info.qualified_name}::GetPrivateStaticClass();", 1),
            ("}", 0),
            ("", 0),
        )

        builder.append(f"struct {generated_statics_name}\n")
        builder.append("{\n")
        _append_line(builder, "static const Durin::DurinCodeGen::FClassParams ClassParams;", 1)
        if class_info.legacy_names:
            _append_line(builder, "static const char* const LegacyNames[];", 1)
        for prop in properties:
            builder.extend(_property_decls(prop))
        if has_properties:
            _append_line(builder, "static const Durin::DurinCodeGen::FPropertyParamsBase* const PropertyParams[];", 1)
        builder.append("};\n\n")

        for prop in properties:
            builder.append(_property_definitions(class_info, prop, symbols))
        if has_properties:
            builder.append(f"const Durin::DurinCodeGen::FPropertyParamsBase* const {generated_statics_name}::PropertyParams[] = {{\n")
            for prop in properties:
                _append_line(builder, f"&{generated_statics_name}::NewProp_{prop.name},", 1)
            builder.append("};\n\n")

        property_params = f"{generated_statics_name}::PropertyParams" if has_properties else "nullptr"
        property_count = len(properties)
        display_name = _cpp_string_literal(class_info.display_name) if class_info.display_name else "nullptr"
        default_object_name = _cpp_string_literal(class_info.default_object_name) if class_info.default_object_name else "nullptr"
        if class_info.legacy_names:
            builder.append(f"const char* const {generated_statics_name}::LegacyNames[] = {{\n")
            for legacy_name in class_info.legacy_names:
                _append_line(builder, f"{_cpp_string_literal(legacy_name)},", 1)
            builder.append("};\n\n")
        legacy_names = f"{generated_statics_name}::LegacyNames" if class_info.legacy_names else "nullptr"
        _append_lines(
            builder,
            (f"const Durin::DurinCodeGen::FClassParams {generated_statics_name}::ClassParams = {{", 0),
            (f"{class_info.generated_helper_no_register_name},", 1),
            (f"\"{class_info.qualified_name}\",", 1),
            (f"\"{class_info.short_name}\",", 1),
            (f"{property_params},", 1),
            (f"{property_count},", 1),
            (f"{display_name},", 1),
            (f"{default_object_name},", 1),
            (f"{legacy_names},", 1),
            (str(len(class_info.legacy_names)), 1),
            ("};", 0),
            ("", 0),
        )

        _append_lines(
            builder,
            (f"Durin::DClass* {class_info.generated_helper_name}()", 0),
            ("{", 0),
            (f"Durin::DClass*& Singleton = {class_info.registration_info_name}.OuterSingleton;", 1),
            ("if (!Singleton)", 1),
            ("{", 1),
            (f"Singleton = Durin::DurinCodeGen::ConstructDClass({generated_statics_name}::ClassParams);", 2),
            ("}", 1),
            ("return Singleton;", 1),
            ("}", 0),
            ("", 0),
        )

        if not class_info.is_abstract and constructor_mode == "object_initializer" and not class_info.has_object_initializer_constructor:
            builder.append(f"{class_info.qualified_name}::{class_info.short_name}(const Durin::FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {{}}\n\n")

    if header.classes or header.enums:
        defer_statics = f"Z_CompiledInDeferFile_{header.file_id}_Statics"
        _append_lines(
            builder,
            (f"struct {defer_statics}", 0),
            ("{", 0),
        )
        if header.classes:
            _append_line(builder, "static constexpr Durin::FClassRegisterCompiledInInfo ClassInfo[] = {", 1)
            for class_info in header.classes:
                _append_line(
                    builder,
                    f"{{ {class_info.generated_helper_name}, {class_info.qualified_name}::StaticClass, "
                    f"\"{class_info.qualified_name}\", &{class_info.registration_info_name} }},",
                    2,
                )
            _append_line(builder, "};", 1)
        if header.enums:
            _append_line(builder, "static constexpr Durin::FEnumRegisterCompiledInInfo EnumInfo[] = {", 1)
            for enum_info in header.enums:
                _append_line(
                    builder,
                    f"{{ {enum_info.generated_helper_name}, {enum_info.generated_helper_no_register_name}, "
                    f"\"{enum_info.qualified_name}\", &{enum_info.registration_info_name} }},",
                    2,
                )
            _append_line(builder, "};", 1)
        _append_lines(
            builder,
            ("};", 0),
            ("", 0),
        )
        if header.classes:
            _append_lines(
                builder,
                (f"static Durin::FRegisterCompiledInInfo Z_CompiledInDeferFile_{header.file_id}_Classes(", 0),
                (f"{defer_statics}::ClassInfo,", 1),
                (str(len(header.classes)), 1),
                (");", 0),
            )
        if header.enums:
            _append_lines(
                builder,
                (f"static Durin::FRegisterCompiledInInfo Z_CompiledInDeferFile_{header.file_id}_Enums(", 0),
                (f"{defer_statics}::EnumInfo,", 1),
                (str(len(header.enums)), 1),
                (");", 0),
            )

    return "".join(builder)




def _append_line(builder: list[str], content: str = "", indent: int = 0) -> None:
    builder.append(_line(content, indent))


def _append_lines(builder: list[str], *lines: tuple[str, int]) -> None:
    for content, indent in lines:
        _append_line(builder, content, indent)










def _enum_definitions(enum_info: ReflectedEnumInfo, package_path: str) -> str:
    builder: list[str] = []
    generated_statics_name = enum_info.generated_statics_name
    values_name = "EnumValues"
    values_ref = f"{generated_statics_name}::{values_name}" if enum_info.values else "nullptr"
    builder.append(f"Durin::FEnumRegistrationInfo {enum_info.registration_info_name};\n\n")
    builder.append(f"struct {generated_statics_name}\n")
    builder.append("{\n")
    if enum_info.values:
        _append_line(builder, f"static const Durin::DurinCodeGen::FEnumValueParams {values_name}[];", 1)
    if enum_info.legacy_names:
        _append_line(builder, "static const char* const LegacyNames[];", 1)
    _append_line(builder, "static const Durin::DurinCodeGen::FEnumParams EnumParams;", 1)
    builder.append("};\n\n")

    if enum_info.values:
        builder.append(f"const Durin::DurinCodeGen::FEnumValueParams {generated_statics_name}::{values_name}[] = {{\n")
        for value_info in enum_info.values:
            display_name = _cpp_string_literal(value_info.display_name) if value_info.display_name else "nullptr"
            _append_line(
                builder,
                f"{{ {_cpp_string_literal(value_info.name)}, static_cast<Durin::uint64>({value_info.value}), {display_name} }},",
                1,
            )
        builder.append("};\n\n")

    if enum_info.legacy_names:
        builder.append(f"const char* const {generated_statics_name}::LegacyNames[] = {{\n")
        for legacy_name in enum_info.legacy_names:
            _append_line(builder, f"{_cpp_string_literal(legacy_name)},", 1)
        builder.append("};\n\n")
    legacy_names = f"{generated_statics_name}::LegacyNames" if enum_info.legacy_names else "nullptr"

    _append_lines(
        builder,
        (f"const Durin::DurinCodeGen::FEnumParams {generated_statics_name}::EnumParams = {{", 0),
        (f"{enum_info.generated_helper_no_register_name},", 1),
        (f"\"{enum_info.qualified_name}\",", 1),
        (f"\"{enum_info.short_name}\",", 1),
        (f"{_cpp_string_literal(enum_info.display_name) if enum_info.display_name else 'nullptr'},", 1),
        (f"{_bool_literal(enum_info.is_scoped)},", 1),
        (f"Durin::DurinCodeGen::EEnumUnderlyingType::{enum_info.underlying_kind},", 1),
        (f"static_cast<Durin::uint16>({enum_info.underlying_size}),", 1),
        (f"{values_ref},", 1),
        (f"{len(enum_info.values)},", 1),
        (f"{legacy_names},", 1),
        (str(len(enum_info.legacy_names)), 1),
        ("};", 0),
        ("", 0),
    )

    _append_lines(
        builder,
        (f"Durin::DEnum* {enum_info.generated_helper_no_register_name}()", 0),
        ("{", 0),
        (f"Durin::DEnum*& Singleton = {enum_info.registration_info_name}.InnerSingleton;", 1),
        ("if (!Singleton)", 1),
        ("{", 1),
        ("std::vector<Durin::FEnumValue> Values;", 2),
        (f"Values.reserve({generated_statics_name}::EnumParams.NumValues);", 2),
        (f"for (size_t Index = 0; Index < {generated_statics_name}::EnumParams.NumValues; ++Index)", 2),
        ("{", 2),
        (f"const Durin::DurinCodeGen::FEnumValueParams& ValueParams = {generated_statics_name}::EnumParams.Values[Index];", 3),
        ("Values.push_back({ Durin::FName(ValueParams.NameUTF8), ValueParams.Value, ValueParams.DisplayName ? ValueParams.DisplayName : \"\" });", 3),
        ("}", 2),
        ("Singleton = new Durin::DEnum(", 2),
        ("Durin::EC_StaticConstructor,", 3),
        (f"Durin::FName(\"{enum_info.short_name}\"),", 3),
        (f"Durin::FName(\"{enum_info.qualified_name}\"),", 3),
        (f"Durin::FName(\"{enum_info.short_name}\"),", 3),
        (f"{generated_statics_name}::EnumParams.DisplayName ? {generated_statics_name}::EnumParams.DisplayName : \"\",", 3),
        (f"{generated_statics_name}::EnumParams.bIsScoped,", 3),
        (f"{generated_statics_name}::EnumParams.UnderlyingType,", 3),
        (f"{generated_statics_name}::EnumParams.UnderlyingSize,", 3),
        ("std::move(Values),", 3),
        ("Durin::EObjectFlags::NoFlags", 3),
        (");", 2),
        (f"Singleton->Register(Durin::DEnum::StaticClass, \"{package_path}\", \"{enum_info.qualified_name}\");", 2),
        ("}", 1),
        ("return Singleton;", 1),
        ("}", 0),
        ("", 0),
        (f"Durin::DEnum* {enum_info.generated_helper_name}()", 0),
        ("{", 0),
        (f"Durin::DEnum*& Singleton = {enum_info.registration_info_name}.OuterSingleton;", 1),
        ("if (!Singleton)", 1),
        ("{", 1),
        (f"Singleton = Durin::DurinCodeGen::ConstructDEnum({generated_statics_name}::EnumParams);", 2),
        ("}", 1),
        ("return Singleton;", 1),
        ("}", 0),
        ("", 0),
    )
    return "".join(builder)


def _struct_definitions(struct_info: ReflectedStructInfo, symbols: ExportedSymbols, package_path: str) -> str:
    builder: list[str] = []
    statics = struct_info.generated_statics_name
    properties = struct_info.properties
    builder.append(f"struct {statics}\n{{\n")
    builder.append("\tstatic const Durin::DurinCodeGen::FStructParams StructParams;\n")
    if struct_info.legacy_names:
        builder.append("\tstatic const char* const LegacyNames[];\n")
    for prop in properties:
        builder.extend(_property_decls(prop))
    if properties:
        builder.append("\tstatic const Durin::DurinCodeGen::FPropertyParamsBase* const PropertyParams[];\n")
    builder.append("};\n\n")
    for prop in properties:
        builder.append(_property_definitions(struct_info, prop, symbols))
    if properties:
        builder.append(f"const Durin::DurinCodeGen::FPropertyParamsBase* const {statics}::PropertyParams[] = {{\n")
        for prop in properties:
            builder.append(f"\t&{statics}::NewProp_{prop.name},\n")
        builder.append("};\n\n")
    prop_ref = f"{statics}::PropertyParams" if properties else "nullptr"
    if struct_info.legacy_names:
        builder.append(f"const char* const {statics}::LegacyNames[] = {{\n")
        for legacy_name in struct_info.legacy_names:
            builder.append(f"\t{_cpp_string_literal(legacy_name)},\n")
        builder.append("};\n\n")
    legacy_names = f"{statics}::LegacyNames" if struct_info.legacy_names else "nullptr"
    builder.append(
        f"const Durin::DurinCodeGen::FStructParams {statics}::StructParams = {{ "
        f"{struct_info.generated_helper_no_register_name}, \"{struct_info.qualified_name}\", \"{struct_info.short_name}\", "
        f"sizeof({struct_info.qualified_name}), alignof({struct_info.qualified_name}), {prop_ref}, {len(properties)}, "
        f"&Durin::GetDStructOps<{struct_info.qualified_name}>(), {legacy_names}, {len(struct_info.legacy_names)} }};\n\n"
    )
    builder.append(
        f"Durin::DStruct* {struct_info.generated_helper_no_register_name}()\n{{\n"
        f"\tstatic Durin::DStruct* Singleton = nullptr;\n"
        f"\tif (!Singleton)\n\t{{\n"
        f"\t\tSingleton = new Durin::DStruct(Durin::EC_StaticConstructor, Durin::FName(\"{struct_info.qualified_name}\"), Durin::FName(\"{struct_info.short_name}\"), sizeof({struct_info.qualified_name}), alignof({struct_info.qualified_name}), Durin::EObjectFlags::Intrinsic);\n"
        f"\t\tSingleton->Register(Durin::DStruct::StaticClass, \"{package_path}\", \"{struct_info.qualified_name}\");\n"
        f"\t}}\n\treturn Singleton;\n}}\n\n"
        f"Durin::DStruct* {struct_info.generated_helper_name}()\n{{\n"
        f"\tstatic Durin::DStruct* Singleton = nullptr;\n"
        f"\tif (!Singleton) Singleton = Durin::DurinCodeGen::ConstructDStruct({statics}::StructParams);\n"
        f"\treturn Singleton;\n}}\n\n"
    )
    return "".join(builder)








def _collect_referenced_helpers(
    prop: ReflectedPropertyInfo,
    symbols: ExportedSymbols,
    referenced_class_helpers: dict[str, str],
    referenced_enum_helpers: dict[str, str],
    referenced_struct_helpers: dict[str, str],
) -> None:
    if prop.referenced_type:
        symbol = symbols.get(prop.referenced_type)
        if symbol:
            referenced_class_helpers[symbol.GeneratedHelperName] = symbol.API
    if prop.referenced_enum_type:
        symbol = symbols.get(prop.referenced_enum_type)
        if symbol:
            referenced_enum_helpers[symbol.GeneratedHelperName] = symbol.API
    if prop.referenced_struct_type:
        symbol = symbols.get(prop.referenced_struct_type)
        if symbol:
            referenced_struct_helpers[symbol.GeneratedHelperName] = symbol.API
    if prop.inner:
        _collect_referenced_helpers(prop.inner, symbols, referenced_class_helpers, referenced_enum_helpers, referenced_struct_helpers)
    if prop.key:
        _collect_referenced_helpers(prop.key, symbols, referenced_class_helpers, referenced_enum_helpers, referenced_struct_helpers)
    if prop.value:
        _collect_referenced_helpers(prop.value, symbols, referenced_class_helpers, referenced_enum_helpers, referenced_struct_helpers)
