from durin_header_tool.model.reflection_info import (
    ReflectedClassInfo,
    ReflectedHeaderInfo,
    ReflectedPropertyInfo,
)


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
    "Enum": "FEnumPropertyParams",
    "Object": "FObjectPropertyParams",
}

TAB = "\t"


def generate_header_content(header: ReflectedHeaderInfo) -> str:
    builder: list[str] = [
        "// Generated code exported from DurinHeaderTool.\n\n",
        "#pragma once\n\n",
    ]

    for class_info in header.classes:
        builder.append(f"struct {class_info.generated_statics_name};\n")
        builder.append(f"{class_info.api} Durin::DClass* {class_info.generated_helper_name}();\n")
        builder.append(f"{class_info.api} Durin::DClass* {class_info.generated_helper_no_register_name}();\n\n")

        if class_info.generated_body_line == 0:
            continue

        generated_body_id = f"{header.file_id}_{class_info.generated_body_line}"
        no_pure_decls = f"{generated_body_id}_INCLASS_NO_PURE_DECLS"
        enhanced_constructors = f"{generated_body_id}_ENHANCED_CONSTRUCTORS"
        generated_body = f"{generated_body_id}_GENERATED_BODY"
        constructor_mode = _constructor_mode(class_info)

        _append_macro_line(builder, f"#define {no_pure_decls}")
        _append_macro_line(builder, "private:", 1)
        _append_macro_line(builder, f"friend struct ::{class_info.generated_statics_name};", 2)
        _append_macro_line(builder, "static Durin::DClass* GetPrivateStaticClass();", 2)
        _append_macro_line(builder, f"friend {class_info.api} Durin::DClass* ::{class_info.generated_helper_no_register_name}();", 2)
        _append_macro_line(builder, "public:", 1)
        _append_macro_line(
            builder,
            f"DECLARE_CLASS({class_info.short_name}, {_base_name_for_macro(class_info)}, ::{class_info.generated_helper_no_register_name})",
            2,
            True,
        )
        builder.append("\n")

        _append_macro_line(builder, f"#define {enhanced_constructors}")
        if constructor_mode == "object_initializer" and not class_info.has_object_initializer_constructor:
            _append_macro_line(builder, f"NO_API {class_info.short_name}(const Durin::FObjectInitializer& ObjectInitializer);", 1)
        if not class_info.has_destructor:
            _append_macro_line(builder, f"NO_API ~{class_info.short_name}() override = default;", 1)
        _append_macro_line(builder, f"{class_info.short_name}({class_info.short_name}&&) = delete;", 1)
        _append_macro_line(builder, f"{class_info.short_name}(const {class_info.short_name}&) = delete;", 1)
        if constructor_mode == "default":
            _append_macro_line(builder, f"DEFINE_DEFAULT_CONSTRUCTOR_CALL({class_info.short_name})", 1, True)
        else:
            _append_macro_line(builder, f"DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL({class_info.short_name})", 1, True)
        builder.append("\n")

        _append_macro_line(builder, f"#define {generated_body}")
        _append_macro_line(builder, "public:", 1)
        _append_macro_line(builder, no_pure_decls, 2)
        _append_macro_line(builder, enhanced_constructors, 2)
        _append_macro_line(builder, "private:", 1, True)
        builder.append("\n")

    _append_lines_no_indent(
        builder,
        "#undef CURRENT_FILE_ID",
        f"#define CURRENT_FILE_ID {header.file_id}",
    )
    return "".join(builder)


def generate_cpp_content(header: ReflectedHeaderInfo, symbols: dict[str, object]) -> str:
    builder: list[str] = [
        "// Generated code exported from DurinHeaderTool.\n\n",
        '#include "DObject/GeneratedCppIncludes.h"\n',
        f'#include "{header.include_path}"\n\n',
    ]

    referenced_helpers: dict[str, str] = {}
    for class_info in header.classes:
        if class_info.base_qualified_name and class_info.base_qualified_name != class_info.qualified_name:
            symbol = symbols.get(class_info.base_qualified_name)
            if symbol:
                referenced_helpers[getattr(symbol, "GeneratedHelperName")] = getattr(symbol, "API")
        for prop in class_info.properties:
            if prop.referenced_type:
                symbol = symbols.get(prop.referenced_type)
                if not symbol:
                    continue
                referenced_helpers[getattr(symbol, "GeneratedHelperName")] = getattr(symbol, "API")

    for helper, api in sorted(referenced_helpers.items()):
        builder.append(f"{api} Durin::DClass* {helper}();\n")
    if referenced_helpers:
        builder.append("\n")

    for class_info in header.classes:
        properties = class_info.properties
        has_properties = bool(properties)
        generated_statics_name = class_info.generated_statics_name
        constructor_mode = _constructor_mode(class_info)
        builder.append(f"Durin::FClassRegistrationInfo {class_info.registration_info_name};\n\n")

        _append_lines(
            builder,
            (f"Durin::DClass* {class_info.qualified_name}::GetPrivateStaticClass()", 0),
            ("{", 0),
            (f"if (!{class_info.registration_info_name}.InnerSingleton)", 1),
            ("{", 1),
            ("Durin::GetPrivateStaticClassBody(", 2),
            ("\"\",", 3),
            (f"\"{class_info.qualified_name}\",", 3),
            (f"{class_info.registration_info_name}.InnerSingleton,", 3),
            ("nullptr,", 3),
            (f"sizeof({class_info.qualified_name}),", 3),
            (f"alignof({class_info.qualified_name}),", 3),
            ("Durin::EClassFlags::None,", 3),
            (f"(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<{class_info.qualified_name}>,", 3),
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
        for prop in properties:
            builder.append(_property_decl(prop))
        if has_properties:
            _append_line(builder, "static const Durin::DurinCodeGen::FPropertyParamsBase* const PropertyParams[];", 1)
        builder.append("};\n\n")

        for prop in properties:
            builder.append(_property_definition(class_info, prop, symbols))
        if has_properties:
            builder.append(f"const Durin::DurinCodeGen::FPropertyParamsBase* const {generated_statics_name}::PropertyParams[] = {{\n")
            for prop in properties:
                _append_line(builder, f"&{generated_statics_name}::NewProp_{prop.name},", 1)
            builder.append("};\n\n")

        property_params = f"{generated_statics_name}::PropertyParams" if has_properties else "nullptr"
        property_count = len(properties)
        _append_lines(
            builder,
            (f"const Durin::DurinCodeGen::FClassParams {generated_statics_name}::ClassParams = {{", 0),
            (f"{class_info.generated_helper_no_register_name},", 1),
            (f"\"{class_info.qualified_name}\",", 1),
            (f"\"{class_info.short_name}\",", 1),
            (f"{property_params},", 1),
            (str(property_count), 1),
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

        if constructor_mode == "object_initializer" and not class_info.has_object_initializer_constructor:
            builder.append(f"{class_info.qualified_name}::{class_info.short_name}(const Durin::FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {{}}\n\n")

    if header.classes:
        defer_statics = f"Z_CompiledInDeferFile_{header.file_id}_Statics"
        _append_lines(
            builder,
            (f"struct {defer_statics}", 0),
            ("{", 0),
            ("static constexpr Durin::FClassRegisterCompiledInInfo ClassInfo[] = {", 1),
        )
        for class_info in header.classes:
            _append_line(
                builder,
                f"{{ {class_info.generated_helper_name}, {class_info.qualified_name}::StaticClass, "
                f"\"{class_info.qualified_name}\", &{class_info.registration_info_name} }},",
                2,
            )
        _append_line(builder, "};", 1)
        _append_lines(
            builder,
            ("};", 0),
            ("", 0),
            (f"static Durin::FRegisterCompiledInInfo Z_CompiledInDeferFile_{header.file_id}(", 0),
            (f"{defer_statics}::ClassInfo,", 1),
            (str(len(header.classes)), 1),
            (");", 0),
        )

    return "".join(builder)


def _append_macro_line(builder: list[str], content: str, indent: int = 0, last: bool = False) -> None:
    builder.append((TAB * indent) + content + ("\n" if last else " \\\n"))


def _append_line(builder: list[str], content: str = "", indent: int = 0) -> None:
    builder.append(_line(content, indent))


def _append_lines(builder: list[str], *lines: tuple[str, int]) -> None:
    for content, indent in lines:
        _append_line(builder, content, indent)


def _append_lines_no_indent(builder: list[str], *lines: str) -> None:
    for line in lines:
        _append_line(builder, line)


def _line(content: str = "", indent: int = 0) -> str:
    return f"{TAB * indent}{content}\n"


def _constructor_mode(class_info: ReflectedClassInfo) -> str:
    if class_info.has_default_constructor:
        return "default"
    return "object_initializer"


def _base_name_for_macro(class_info: ReflectedClassInfo) -> str:
    return class_info.base_qualified_name or "Durin::DObject"


def _property_decl(prop: ReflectedPropertyInfo) -> str:
    param_type = PROPERTY_PARAM_BY_KIND[prop.kind]
    return _line(f"static const Durin::DurinCodeGen::{param_type} NewProp_{prop.name};", 1)


def _property_definition(class_info: ReflectedClassInfo, prop: ReflectedPropertyInfo, symbols: dict[str, object]) -> str:
    param_type = PROPERTY_PARAM_BY_KIND[prop.kind]
    referenced_helper = "nullptr"
    if prop.referenced_type:
        referenced_symbol = symbols.get(prop.referenced_type)
        if referenced_symbol:
            referenced_helper = getattr(referenced_symbol, "GeneratedHelperName")
    return (
        f"const Durin::DurinCodeGen::{param_type} {class_info.generated_statics_name}::NewProp_{prop.name} = "
        f"{{ \"{prop.name}\", Durin::EPropertyFlags::None, {prop.array_dim}, "
        f"static_cast<Durin::uint16>(STRUCT_OFFSET({class_info.qualified_name}, {prop.name})), "
        f"Durin::DurinCodeGen::EPropertyGenFlags::{prop.kind}, {referenced_helper} }};\n"
    )
