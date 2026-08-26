from durin_header_tool.model.reflection_info import ReflectedHeaderInfo
from durin_header_tool.writers.reflection_writer_common import (
    _append_macro_line,
    _append_lines_no_indent,
    _base_name_for_macro,
    _constructor_mode,
    _wrap_in_namespace,
)


def generate_header_content(header: ReflectedHeaderInfo) -> str:
    builder: list[str] = [
        "// Generated code exported from DurinHeaderTool.\n\n",
        "#pragma once\n\n",
    ]

    declarations_by_namespace: dict[tuple, list[str]] = {}
    all_types = [*header.classes, *header.structs, *header.enums]
    for type_info in all_types:
        declarations = declarations_by_namespace.setdefault(type_info.namespace_path, [])
        return_type = {
            "class": "DClass",
            "struct": "DStruct",
            "enum": "DEnum",
        }[type_info.generated_symbol.kind]
        declarations.append(f"struct {type_info.generated_statics_name};\n")
        declarations.append(
            f"{type_info.api} Durin::{return_type}* {type_info.generated_helper_name}();\n"
        )
        declarations.append(
            f"{type_info.api} Durin::{return_type}* {type_info.generated_helper_no_register_name}();\n"
        )
    for namespace_path in sorted(declarations_by_namespace):
        builder.append(_wrap_in_namespace("".join(declarations_by_namespace[namespace_path]), namespace_path))
        builder.append("\n")

    for class_info in header.classes:

        if class_info.generated_body_line == 0:
            continue

        generated_body_id = f"{header.file_id}_{class_info.generated_body_line}"
        no_pure_decls = f"{generated_body_id}_INCLASS_NO_PURE_DECLS"
        enhanced_constructors = f"{generated_body_id}_ENHANCED_CONSTRUCTORS"
        generated_body = f"{generated_body_id}_GENERATED_BODY"
        constructor_mode = _constructor_mode(class_info)

        _append_macro_line(builder, f"#define {no_pure_decls}")
        _append_macro_line(builder, "private:", 1)
        _append_macro_line(builder, f"friend struct {class_info.generated_statics_reference};", 2)
        _append_macro_line(builder, "static Durin::DClass* GetPrivateStaticClass();", 2)
        _append_macro_line(builder, f"friend {class_info.api} Durin::DClass* {class_info.generated_helper_no_register_reference}();", 2)
        _append_macro_line(builder, "public:", 1)
        _append_macro_line(
            builder,
            f"DECLARE_CLASS({class_info.short_name}, {_base_name_for_macro(class_info)}, {class_info.generated_helper_no_register_reference})",
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
        if struct_info.generated_body_line == 0:
            continue
        generated_body_id = f"{header.file_id}_{struct_info.generated_body_line}"
        generated_body = f"{generated_body_id}_GENERATED_BODY"
        _append_macro_line(builder, f"#define {generated_body}")
        _append_macro_line(builder, "private:", 1)
        _append_macro_line(builder, f"friend struct {struct_info.generated_statics_reference};", 2)
        _append_macro_line(builder, f"friend {struct_info.api} Durin::DStruct* {struct_info.generated_helper_reference}();", 2)
        _append_macro_line(builder, "public:", 1)
        _append_macro_line(builder, f"static Durin::DStruct* StaticStruct() {{ return {struct_info.generated_helper_reference}(); }}", 2, True)
        builder.append("\n")

    _append_lines_no_indent(
        builder,
        "#undef CURRENT_FILE_ID",
        f"#define CURRENT_FILE_ID {header.file_id}",
    )
    return "".join(builder)
