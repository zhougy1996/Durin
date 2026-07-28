from durin_header_tool.model.reflection_info import (
    ReflectedClassInfo,
    ReflectedEnumInfo,
    ReflectedHeaderInfo,
    ReflectedPropertyInfo,
    ReflectedStructInfo,
    _cpp_type_spelling,
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
    "Name": "FNamePropertyParams",
    "Guid": "FGuidPropertyParams",
    "Enum": "FEnumPropertyParams",
    "Object": "FObjectPropertyParams",
    "Array": "FArrayPropertyParams",
    "Map": "FMapPropertyParams",
    "Struct": "FStructPropertyParams",
}

TAB = "\t"


def _cpp_string_literal(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    return f'"{escaped}"'


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
        if not class_info.is_abstract and constructor_mode == "object_initializer" and not class_info.has_object_initializer_constructor:
            _append_macro_line(builder, f"NO_API {class_info.short_name}(const Durin::FObjectInitializer& ObjectInitializer);", 1)
        if not class_info.has_destructor:
            _append_macro_line(builder, f"NO_API ~{class_info.short_name}() override = default;", 1)
        _append_macro_line(builder, f"{class_info.short_name}({class_info.short_name}&&) = delete;", 1)
        _append_macro_line(builder, f"{class_info.short_name}(const {class_info.short_name}&) = delete;", 1)
        if not class_info.is_abstract:
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

    for struct_info in header.structs:
        builder.append(f"struct {struct_info.generated_statics_name};\n")
        builder.append(f"{struct_info.api} Durin::DStruct* {struct_info.generated_helper_name}();\n")
        builder.append(f"{struct_info.api} Durin::DStruct* {struct_info.generated_helper_no_register_name}();\n\n")
        if struct_info.generated_body_line == 0:
            continue
        generated_body_id = f"{header.file_id}_{struct_info.generated_body_line}"
        generated_body = f"{generated_body_id}_GENERATED_BODY"
        _append_macro_line(builder, f"#define {generated_body}")
        _append_macro_line(builder, "private:", 1)
        _append_macro_line(builder, f"friend struct ::{struct_info.generated_statics_name};", 2)
        _append_macro_line(builder, f"friend {struct_info.api} Durin::DStruct* ::{struct_info.generated_helper_name}();", 2)
        _append_macro_line(builder, "public:", 1)
        _append_macro_line(builder, f"static Durin::DStruct* StaticStruct() {{ return ::{struct_info.generated_helper_name}(); }}", 2, True)
        builder.append("\n")

    for enum_info in header.enums:
        builder.append(f"struct {enum_info.generated_statics_name};\n")
        builder.append(f"{enum_info.api} Durin::DEnum* {enum_info.generated_helper_name}();\n")
        builder.append(f"{enum_info.api} Durin::DEnum* {enum_info.generated_helper_no_register_name}();\n\n")

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
    # All reflected types in a module share its compiled-in C++ package.
    package_path = f"/Cpp/{header.module_name}"

    referenced_class_helpers: dict[str, str] = {}
    referenced_enum_helpers: dict[str, str] = {}
    referenced_struct_helpers: dict[str, str] = {}
    for class_info in header.classes:
        if class_info.base_qualified_name and class_info.base_qualified_name != class_info.qualified_name:
            symbol = symbols.get(class_info.base_qualified_name)
            if symbol:
                referenced_class_helpers[getattr(symbol, "GeneratedHelperName")] = getattr(symbol, "API")
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
            (f"Durin::EClassFlags::{'Abstract' if class_info.is_abstract else 'None'},", 3),
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
        _append_lines(
            builder,
            (f"const Durin::DurinCodeGen::FClassParams {generated_statics_name}::ClassParams = {{", 0),
            (f"{class_info.generated_helper_no_register_name},", 1),
            (f"\"{class_info.qualified_name}\",", 1),
            (f"\"{class_info.short_name}\",", 1),
            (f"{property_params},", 1),
            (f"{property_count},", 1),
            (f"{display_name},", 1),
            (default_object_name, 1),
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


def _bool_literal(value: bool) -> str:
    return "true" if value else "false"


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
        (str(len(enum_info.values)), 1),
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


def _struct_definitions(struct_info: ReflectedStructInfo, symbols: dict[str, object], package_path: str) -> str:
    builder: list[str] = []
    statics = struct_info.generated_statics_name
    properties = struct_info.properties
    builder.append(f"struct {statics}\n{{\n")
    builder.append("\tstatic const Durin::DurinCodeGen::FStructParams StructParams;\n")
    builder.append("\tstatic void Initialize(void* Memory);\n")
    builder.append("\tstatic void Destroy(void* Memory);\n")
    builder.append("\tstatic void Copy(void* Destination, const void* Source);\n")
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
    builder.append(f"void {statics}::Initialize(void* Memory) {{ new (Memory) {struct_info.qualified_name}(); }}\n")
    builder.append(f"void {statics}::Destroy(void* Memory) {{ static_cast<{struct_info.qualified_name}*>(Memory)->~{struct_info.short_name}(); }}\n")
    builder.append(f"void {statics}::Copy(void* Destination, const void* Source) {{ new (Destination) {struct_info.qualified_name}(*static_cast<const {struct_info.qualified_name}*>(Source)); }}\n\n")
    builder.append(
        f"const Durin::DurinCodeGen::FStructParams {statics}::StructParams = {{ "
        f"{struct_info.generated_helper_no_register_name}, \"{struct_info.qualified_name}\", \"{struct_info.short_name}\", "
        f"sizeof({struct_info.qualified_name}), alignof({struct_info.qualified_name}), {prop_ref}, {len(properties)}, &{statics}::Initialize, &{statics}::Destroy, &{statics}::Copy }};\n\n"
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


def _property_decls(prop: ReflectedPropertyInfo) -> list[str]:
    decls: list[str] = []
    if prop.inner:
        decls.extend(_property_decls(prop.inner))
    if prop.key:
        decls.extend(_property_decls(prop.key))
    if prop.value:
        decls.extend(_property_decls(prop.value))
    if prop.kind == "Array":
        decls.extend(
            [
                _line(f"static Durin::uint64 NewProp_{prop.name}_ArrayNum(const void* Container);", 1),
                _line(f"static const void* NewProp_{prop.name}_ArrayGetElement(const void* Container, Durin::uint64 Index);", 1),
                _line(f"static void* NewProp_{prop.name}_ArrayGetMutableElement(void* Container, Durin::uint64 Index);", 1),
                _line(f"static void NewProp_{prop.name}_ArrayResize(void* Container, Durin::uint64 Num);", 1),
                _line(f"static const Durin::DurinCodeGen::FArrayPropertyHelper NewProp_{prop.name}_ArrayHelper;", 1),
            ]
        )
    if prop.kind == "Map":
        decls.extend(
            [
                _line(f"static Durin::uint64 NewProp_{prop.name}_MapNum(const void* Container);", 1),
                _line(f"static const void* NewProp_{prop.name}_MapGetKey(const void* Container, Durin::uint64 Index);", 1),
                _line(f"static const void* NewProp_{prop.name}_MapGetValue(const void* Container, Durin::uint64 Index);", 1),
                _line(f"static void* NewProp_{prop.name}_MapGetMutableValue(void* Container, Durin::uint64 Index);", 1),
                _line(f"static void NewProp_{prop.name}_MapClear(void* Container);", 1),
                _line(f"static void* NewProp_{prop.name}_MapCreateKey();", 1),
                _line(f"static void* NewProp_{prop.name}_MapCreateKeyCopy(const void* Key);", 1),
                _line(f"static void NewProp_{prop.name}_MapDestroyKey(void* Key);", 1),
                _line(f"static void* NewProp_{prop.name}_MapCreateValue();", 1),
                _line(f"static void NewProp_{prop.name}_MapDestroyValue(void* Value);", 1),
                _line(f"static void NewProp_{prop.name}_MapInsert(void* Container, const void* Key, const void* Value);", 1),
                _line(f"static bool NewProp_{prop.name}_MapContains(const void* Container, const void* Key);", 1),
                _line(f"static bool NewProp_{prop.name}_MapRenameKey(void* Container, const void* OldKey, const void* NewKey);", 1),
                _line(f"static bool NewProp_{prop.name}_MapRemove(void* Container, const void* Key);", 1),
                _line(f"static const Durin::DurinCodeGen::FMapPropertyHelper NewProp_{prop.name}_MapHelper;", 1),
            ]
        )
    if prop.metadata:
        decls.append(_line(f"static const Durin::DurinCodeGen::FMetaDataPair NewProp_{prop.name}_MetaData[];", 1))
    param_type = PROPERTY_PARAM_BY_KIND[prop.kind]
    decls.append(_line(f"static const Durin::DurinCodeGen::{param_type} NewProp_{prop.name};", 1))
    return decls


def _property_definitions(class_info: ReflectedClassInfo, prop: ReflectedPropertyInfo, symbols: dict[str, object], nested: bool = False) -> str:
    content = []
    if prop.inner:
        content.append(_property_definitions(class_info, prop.inner, symbols, True))
    if prop.key:
        content.append(_property_definitions(class_info, prop.key, symbols, True))
    if prop.value:
        content.append(_property_definitions(class_info, prop.value, symbols, True))
    content.append(_property_definition(class_info, prop, symbols, nested))
    return "".join(content)


def _property_definition(class_info: ReflectedClassInfo, prop: ReflectedPropertyInfo, symbols: dict[str, object], nested: bool) -> str:
    content = ""
    if prop.kind == "Array":
        content += _array_helper_definition(class_info, prop, symbols)
    if prop.kind == "Map":
        content += _map_helper_definition(class_info, prop, symbols)
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
    param_type = PROPERTY_PARAM_BY_KIND[prop.kind]
    referenced_class_helper = "nullptr"
    if prop.referenced_type:
        referenced_symbol = symbols.get(prop.referenced_type)
        if referenced_symbol:
            referenced_class_helper = getattr(referenced_symbol, "GeneratedHelperName")
    referenced_enum_helper = "nullptr"
    if prop.referenced_enum_type:
        referenced_symbol = symbols.get(prop.referenced_enum_type)
        if referenced_symbol:
            referenced_enum_helper = getattr(referenced_symbol, "GeneratedHelperName")
    referenced_struct_helper = "nullptr"
    if prop.referenced_struct_type:
        referenced_symbol = symbols.get(prop.referenced_struct_type)
        if referenced_symbol:
            referenced_struct_helper = getattr(referenced_symbol, "GeneratedHelperName")
    property_flags = prop.flags
    if property_flags == "None":
        property_flags = "Durin::EPropertyFlags::None"
    inner = f"&{class_info.generated_statics_name}::NewProp_{prop.inner.name}" if prop.inner else "nullptr"
    key = f"&{class_info.generated_statics_name}::NewProp_{prop.key.name}" if prop.key else "nullptr"
    value = f"&{class_info.generated_statics_name}::NewProp_{prop.value.name}" if prop.value else "nullptr"
    offset = "0" if nested else f"static_cast<Durin::uint16>(STRUCT_OFFSET({class_info.qualified_name}, {prop.name}))"
    element_size = f"sizeof(decltype((({class_info.qualified_name}*)0)->{prop.name}))" if prop.element_size == "sizeof_self" else prop.element_size
    value_type = _cpp_type_spelling(prop.type_name, symbols) if nested else f"std::remove_extent_t<decltype((({class_info.qualified_name}*)0)->{prop.name})>"
    array_helper = f"&{class_info.generated_statics_name}::NewProp_{prop.name}_ArrayHelper" if prop.kind == "Array" else "nullptr"
    map_helper = f"&{class_info.generated_statics_name}::NewProp_{prop.name}_MapHelper" if prop.kind == "Map" else "nullptr"
    content += (
        f"const Durin::DurinCodeGen::{param_type} {class_info.generated_statics_name}::NewProp_{prop.name} = "
        f"{{ \"{prop.name}\", {property_flags}, {prop.array_dim}, "
        f"{offset}, "
        f"static_cast<Durin::uint16>({element_size}), "
        f"Durin::DurinCodeGen::EPropertyGenFlags::{prop.kind}, {referenced_class_helper}, {referenced_enum_helper}, {inner}, {key}, {value}, "
        f"{_bool_literal(prop.is_object_ptr_wrapper)}, {array_helper}, {map_helper}, {referenced_struct_helper}, nullptr, nullptr, "
        f"{metadata_ref}, {metadata_count}, sizeof({value_type}), alignof({value_type}), "
        f"&Durin::DurinCodeGen::InitializePropertyValue<{value_type}>, "
        f"&Durin::DurinCodeGen::DestroyPropertyValue<{value_type}> }};\n"
    )
    return content


def _array_helper_definition(class_info: ReflectedClassInfo, prop: ReflectedPropertyInfo, symbols: dict[str, object]) -> str:
    vector_type = _cpp_type_spelling(prop.type_name, symbols)
    statics = class_info.generated_statics_name
    name = f"NewProp_{prop.name}"
    return (
        f"Durin::uint64 {statics}::{name}_ArrayNum(const void* Container)\n"
        "{\n"
        f"\tconst auto* Value = static_cast<const {vector_type}*>(Container);\n"
        "\treturn static_cast<Durin::uint64>(Value->size());\n"
        "}\n\n"
        f"const void* {statics}::{name}_ArrayGetElement(const void* Container, Durin::uint64 Index)\n"
        "{\n"
        f"\tconst auto* Value = static_cast<const {vector_type}*>(Container);\n"
        "\treturn &(*Value)[static_cast<size_t>(Index)];\n"
        "}\n\n"
        f"void* {statics}::{name}_ArrayGetMutableElement(void* Container, Durin::uint64 Index)\n"
        "{\n"
        f"\tauto* Value = static_cast<{vector_type}*>(Container);\n"
        "\treturn &(*Value)[static_cast<size_t>(Index)];\n"
        "}\n\n"
        f"void {statics}::{name}_ArrayResize(void* Container, Durin::uint64 Num)\n"
        "{\n"
        f"\tauto* Value = static_cast<{vector_type}*>(Container);\n"
        "\tValue->resize(static_cast<size_t>(Num));\n"
        "}\n\n"
        f"const Durin::DurinCodeGen::FArrayPropertyHelper {statics}::{name}_ArrayHelper = {{\n"
        f"\t&{statics}::{name}_ArrayNum,\n"
        f"\t&{statics}::{name}_ArrayGetElement,\n"
        f"\t&{statics}::{name}_ArrayGetMutableElement,\n"
        f"\t&{statics}::{name}_ArrayResize\n"
        "};\n"
    )


def _map_helper_definition(class_info: ReflectedClassInfo, prop: ReflectedPropertyInfo, symbols: dict[str, object]) -> str:
    map_type = _cpp_type_spelling(prop.type_name, symbols)
    statics = class_info.generated_statics_name
    name = f"NewProp_{prop.name}"
    return (
        f"Durin::uint64 {statics}::{name}_MapNum(const void* Container)\n"
        "{\n"
        f"\treturn static_cast<Durin::uint64>(static_cast<const {map_type}*>(Container)->size());\n"
        "}\n\n"
        f"const void* {statics}::{name}_MapGetKey(const void* Container, Durin::uint64 Index)\n"
        "{\n"
        f"\tconst auto* Value = static_cast<const {map_type}*>(Container);\n"
        "\tauto It = Value->begin();\n"
        "\tstd::advance(It, static_cast<size_t>(Index));\n"
        "\treturn &It->first;\n"
        "}\n\n"
        f"const void* {statics}::{name}_MapGetValue(const void* Container, Durin::uint64 Index)\n"
        "{\n"
        f"\tconst auto* Value = static_cast<const {map_type}*>(Container);\n"
        "\tauto It = Value->begin();\n"
        "\tstd::advance(It, static_cast<size_t>(Index));\n"
        "\treturn &It->second;\n"
        "}\n\n"
        f"void* {statics}::{name}_MapGetMutableValue(void* Container, Durin::uint64 Index)\n"
        "{\n"
        f"\tauto* Value = static_cast<{map_type}*>(Container);\n"
        "\tauto It = Value->begin();\n"
        "\tstd::advance(It, static_cast<size_t>(Index));\n"
        "\treturn &It->second;\n"
        "}\n\n"
        f"void {statics}::{name}_MapClear(void* Container)\n"
        "{\n"
        f"\tstatic_cast<{map_type}*>(Container)->clear();\n"
        "}\n\n"
        f"void* {statics}::{name}_MapCreateKey() {{ using FType = {map_type}::key_type; return new FType(); }}\n"
        f"void* {statics}::{name}_MapCreateKeyCopy(const void* Key) {{ using FType = {map_type}::key_type; return new FType(*static_cast<const FType*>(Key)); }}\n"
        f"void {statics}::{name}_MapDestroyKey(void* Key) {{ using FType = {map_type}::key_type; delete static_cast<FType*>(Key); }}\n"
        f"void* {statics}::{name}_MapCreateValue() {{ using FType = {map_type}::mapped_type; return new FType(); }}\n"
        f"void {statics}::{name}_MapDestroyValue(void* Value) {{ using FType = {map_type}::mapped_type; delete static_cast<FType*>(Value); }}\n"
        f"void {statics}::{name}_MapInsert(void* Container, const void* Key, const void* Value)\n"
        "{\n"
        f"\tusing FMapType = {map_type};\n"
        "\tusing FKeyType = FMapType::key_type;\n"
        "\tusing FValueType = FMapType::mapped_type;\n"
        "\tstatic_cast<FMapType*>(Container)->insert_or_assign(*static_cast<const FKeyType*>(Key), *static_cast<const FValueType*>(Value));\n"
        "}\n\n"
        f"bool {statics}::{name}_MapContains(const void* Container, const void* Key)\n"
        "{\n"
        f"\tusing FMapType = {map_type};\n"
        "\tusing FKeyType = FMapType::key_type;\n"
        "\treturn static_cast<const FMapType*>(Container)->contains(*static_cast<const FKeyType*>(Key));\n"
        "}\n\n"
        f"bool {statics}::{name}_MapRenameKey(void* Container, const void* OldKey, const void* NewKey)\n"
        "{\n"
        f"\tusing FMapType = {map_type};\n"
        "\tusing FKeyType = FMapType::key_type;\n"
        "\tauto* Value = static_cast<FMapType*>(Container);\n"
        "\tconst FKeyType OldKeyCopy = *static_cast<const FKeyType*>(OldKey);\n"
        "\tconst FKeyType NewKeyCopy = *static_cast<const FKeyType*>(NewKey);\n"
        "\tif (OldKeyCopy == NewKeyCopy) return false;\n"
        "\tif (Value->contains(NewKeyCopy)) return false;\n"
        "\tauto Node = Value->extract(OldKeyCopy);\n"
        "\tif (Node.empty()) return false;\n"
        "\tNode.key() = NewKeyCopy;\n"
        "\tValue->insert(std::move(Node));\n"
        "\treturn true;\n"
        "}\n\n"
        f"bool {statics}::{name}_MapRemove(void* Container, const void* Key)\n"
        "{\n"
        f"\tusing FMapType = {map_type};\n"
        "\tusing FKeyType = FMapType::key_type;\n"
        "\treturn static_cast<FMapType*>(Container)->erase(*static_cast<const FKeyType*>(Key)) != 0;\n"
        "}\n\n"
        f"const Durin::DurinCodeGen::FMapPropertyHelper {statics}::{name}_MapHelper = {{\n"
        f"\t&{statics}::{name}_MapNum, &{statics}::{name}_MapGetKey, &{statics}::{name}_MapGetValue, &{statics}::{name}_MapGetMutableValue,\n"
        f"\t&{statics}::{name}_MapClear, &{statics}::{name}_MapCreateKey, &{statics}::{name}_MapCreateKeyCopy, &{statics}::{name}_MapDestroyKey,\n"
        f"\t&{statics}::{name}_MapCreateValue, &{statics}::{name}_MapDestroyValue, &{statics}::{name}_MapInsert,\n"
        f"\t&{statics}::{name}_MapContains, &{statics}::{name}_MapRenameKey, &{statics}::{name}_MapRemove\n"
        "};\n"
    )


def _collect_referenced_helpers(
    prop: ReflectedPropertyInfo,
    symbols: dict[str, object],
    referenced_class_helpers: dict[str, str],
    referenced_enum_helpers: dict[str, str],
    referenced_struct_helpers: dict[str, str],
) -> None:
    if prop.referenced_type:
        symbol = symbols.get(prop.referenced_type)
        if symbol:
            referenced_class_helpers[getattr(symbol, "GeneratedHelperName")] = getattr(symbol, "API")
    if prop.referenced_enum_type:
        symbol = symbols.get(prop.referenced_enum_type)
        if symbol:
            referenced_enum_helpers[getattr(symbol, "GeneratedHelperName")] = getattr(symbol, "API")
    if prop.referenced_struct_type:
        symbol = symbols.get(prop.referenced_struct_type)
        if symbol:
            referenced_struct_helpers[getattr(symbol, "GeneratedHelperName")] = getattr(symbol, "API")
    if prop.inner:
        _collect_referenced_helpers(prop.inner, symbols, referenced_class_helpers, referenced_enum_helpers, referenced_struct_helpers)
    if prop.key:
        _collect_referenced_helpers(prop.key, symbols, referenced_class_helpers, referenced_enum_helpers, referenced_struct_helpers)
    if prop.value:
        _collect_referenced_helpers(prop.value, symbols, referenced_class_helpers, referenced_enum_helpers, referenced_struct_helpers)
