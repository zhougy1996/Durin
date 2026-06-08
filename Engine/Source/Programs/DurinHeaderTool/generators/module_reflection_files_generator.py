from pathlib import Path

import configs
import utils
from models.export_infos import load_module_export_file
from models.module_manifest import ModuleManifest, load_module_manifest_file, save_module_manifest_file
from models.reflection_info import (
    SYMBOL_NAME_SCHEME,
    TOOL_VERSION,
    ReflectedClassInfo,
    ReflectedHeaderInfo,
    ReflectedPropertyInfo,
    make_generated_helper_name,
    parse_reflection_header,
)


_PROPERTY_PARAM_BY_KIND = {
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


def _load_available_symbols(module_name: str) -> dict[str, object]:
    symbols: dict[str, object] = {}
    for dep_module in configs.collect_all_dependent_module_with_export_file(module_name):
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for module '{dep_module}' not found at expected path: {export_file_path}")
        export_info = load_module_export_file(export_file_path)
        symbols.update(export_info.symbols)
    return symbols


def _resolve_short_symbol(short_or_qualified_name: str, symbols: dict[str, object]) -> str:
    if not short_or_qualified_name or "::" in short_or_qualified_name:
        return short_or_qualified_name
    matches = [qualified_name for qualified_name, symbol in symbols.items() if getattr(symbol, "shortName", "") == short_or_qualified_name]
    return matches[0] if len(matches) == 1 else short_or_qualified_name


def _resolve_header(header: ReflectedHeaderInfo, symbols: dict[str, object]) -> None:
    for class_info in header.classes:
        class_info.base_qualified_name = _resolve_short_symbol(class_info.base_qualified_name, symbols)
        for prop in class_info.properties:
            prop.referenced_type = _resolve_short_symbol(prop.referenced_type, symbols)


def get_reflection_headers_requiring_regeneration(old_manifest: ModuleManifest, new_manifest: ModuleManifest) -> list[str]:
    if old_manifest is None:
        return list(new_manifest.reflect_headers.keys())

    if (
        old_manifest.schema_version != new_manifest.schema_version
        or old_manifest.tool_version != new_manifest.tool_version
        or old_manifest.symbol_name_scheme != new_manifest.symbol_name_scheme
        or old_manifest.profile != new_manifest.profile
        or old_manifest.platform != new_manifest.platform
        or old_manifest.generator_options_hash != new_manifest.generator_options_hash
    ):
        return list(new_manifest.reflect_headers.keys())

    for dep_module, new_fingerprint in new_manifest.dep_module_exports.items():
        old_fingerprint = old_manifest.dep_module_exports.get(dep_module)
        if old_fingerprint != new_fingerprint:
            return list(new_manifest.reflect_headers.keys())

    headers_requiring_regeneration = []
    for header, new_fingerprint in new_manifest.reflect_headers.items():
        old_fingerprint = old_manifest.reflect_headers.get(header)
        if old_fingerprint != new_fingerprint:
            headers_requiring_regeneration.append(header)
    return headers_requiring_regeneration


def make_new_module_manifest(module_name: str, old_manifest: ModuleManifest = None) -> ModuleManifest:
    manifest = ModuleManifest(
        module_name=module_name,
        profile=configs.PROFILE_NAME,
        platform=configs.ARCH,
        tool_version=TOOL_VERSION,
        symbol_name_scheme=SYMBOL_NAME_SCHEME,
        generator_options_hash="default",
    )
    dependent_modules_with_export_file = configs.collect_all_dependent_module_with_export_file(module_name)

    for dep_module in dependent_modules_with_export_file:
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for dependent module '{dep_module}' not found at expected path: {export_file_path}")
        manifest.dep_module_exports[dep_module] = utils.get_light_file_fingerprint(export_file_path)

    for header in configs.get_module_config(module_name).reflect_headers:
        header_file_path = (configs.get_module_config(module_name).module_dir / header).resolve()
        if not header_file_path.exists():
            raise FileNotFoundError(f"Reflect header file '{header}' for module '{module_name}' not found at expected path: {header_file_path}")

        old_header_fingerprint = old_manifest.reflect_headers.get(header) if old_manifest else None
        manifest.reflect_headers[header] = utils.get_file_fingerprint_with_old_cache(header_file_path, old_header_fingerprint)

        output_dir = utils.get_module_dht_output_dir(module_name)
        manifest.generated_outputs.append(str(output_dir / f"{Path(header).stem}.gen.h"))
        manifest.generated_outputs.append(str(output_dir / f"{Path(header).stem}.gen.cpp"))

    manifest.generated_outputs.append(str(utils.get_module_dht_output_dir(module_name) / f"{module_name}.module.gen.cpp"))
    return manifest


def _append_macro_line(builder: list[str], content: str, indent: int = 0, last: bool = False) -> None:
    builder.append(("\t" * indent) + content + ("\n" if last else " \\\n"))


def _constructor_mode(class_info: ReflectedClassInfo) -> str:
    if class_info.has_default_constructor:
        return "default"
    return "object_initializer"


def _base_name_for_macro(class_info: ReflectedClassInfo) -> str:
    return class_info.base_qualified_name or "Durin::DObject"


def _generate_header_content(header: ReflectedHeaderInfo) -> str:
    builder: list[str] = []
    builder.append("// Generated code exported from DurinHeaderTool.\n\n")
    builder.append("#pragma once\n\n")

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
        if _constructor_mode(class_info) == "object_initializer" and not class_info.has_object_initializer_constructor:
            _append_macro_line(builder, f"NO_API {class_info.short_name}(const Durin::FObjectInitializer& ObjectInitializer);", 1)
        if not class_info.has_destructor:
            _append_macro_line(builder, f"NO_API ~{class_info.short_name}() override = default;", 1)
        _append_macro_line(builder, f"{class_info.short_name}({class_info.short_name}&&) = delete;", 1)
        _append_macro_line(builder, f"{class_info.short_name}(const {class_info.short_name}&) = delete;", 1)
        if _constructor_mode(class_info) == "default":
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

    builder.append("#undef CURRENT_FILE_ID\n")
    builder.append(f"#define CURRENT_FILE_ID {header.file_id}\n")
    return "".join(builder)


def _property_decl(prop: ReflectedPropertyInfo) -> str:
    param_type = _PROPERTY_PARAM_BY_KIND[prop.kind]
    return f"\tstatic const Durin::DurinCodeGen::{param_type} NewProp_{prop.name};\n"


def _property_definition(class_info: ReflectedClassInfo, prop: ReflectedPropertyInfo, symbols: dict[str, object]) -> str:
    param_type = _PROPERTY_PARAM_BY_KIND[prop.kind]
    referenced_helper = "nullptr"
    if prop.referenced_type:
        referenced_symbol = symbols.get(prop.referenced_type)
        if referenced_symbol:
            referenced_helper = getattr(referenced_symbol, "generatedHelperName")
    return (
        f"const Durin::DurinCodeGen::{param_type} {class_info.generated_statics_name}::NewProp_{prop.name} = "
        f"{{ \"{prop.name}\", Durin::EPropertyFlags::None, {prop.array_dim}, "
        f"static_cast<Durin::uint16>(STRUCT_OFFSET({class_info.qualified_name}, {prop.name})), "
        f"Durin::DurinCodeGen::EPropertyGenFlags::{prop.kind}, {referenced_helper} }};\n"
    )


def _generate_cpp_content(header: ReflectedHeaderInfo, symbols: dict[str, object]) -> str:
    builder: list[str] = []
    builder.append("// Generated code exported from DurinHeaderTool.\n\n")
    builder.append('#include "DObject/GeneratedCppIncludes.h"\n')
    builder.append(f'#include "{header.include_path}"\n\n')

    referenced_helpers: dict[str, str] = {}
    for class_info in header.classes:
        if class_info.base_qualified_name and class_info.base_qualified_name != class_info.qualified_name:
            symbol = symbols.get(class_info.base_qualified_name)
            if symbol:
                referenced_helpers[getattr(symbol, "generatedHelperName")] = getattr(symbol, "api")
        for prop in class_info.properties:
            if prop.referenced_type and prop.referenced_type in symbols:
                symbol = symbols[prop.referenced_type]
                referenced_helpers[getattr(symbol, "generatedHelperName")] = getattr(symbol, "api")

    for helper, api in sorted(referenced_helpers.items()):
        builder.append(f"{api} Durin::DClass* {helper}();\n")
    if referenced_helpers:
        builder.append("\n")

    for class_info in header.classes:
        builder.append(f"Durin::FClassRegistrationInfo {class_info.registration_info_name};\n\n")

        builder.append(f"Durin::DClass* {class_info.qualified_name}::GetPrivateStaticClass()\n")
        builder.append("{\n")
        builder.append(f"\tif (!{class_info.registration_info_name}.InnerSingleton)\n")
        builder.append("\t{\n")
        builder.append("\t\tDurin::GetPrivateStaticClassBody(\n")
        builder.append("\t\t\t\"\",\n")
        builder.append(f"\t\t\t\"{class_info.qualified_name}\",\n")
        builder.append(f"\t\t\t{class_info.registration_info_name}.InnerSingleton,\n")
        builder.append("\t\t\tnullptr,\n")
        builder.append(f"\t\t\tsizeof({class_info.qualified_name}),\n")
        builder.append(f"\t\t\talignof({class_info.qualified_name}),\n")
        builder.append("\t\t\tDurin::EClassFlags::None,\n")
        builder.append(f"\t\t\t(Durin::DClass::ClassConstructorType)Durin::InternalConstructor<{class_info.qualified_name}>,\n")
        builder.append(f"\t\t\t&{_base_name_for_macro(class_info)}::StaticClass\n")
        builder.append("\t\t);\n")
        builder.append("\t}\n")
        builder.append(f"\treturn {class_info.registration_info_name}.InnerSingleton;\n")
        builder.append("}\n\n")

        builder.append(f"Durin::DClass* {class_info.generated_helper_no_register_name}()\n")
        builder.append("{\n")
        builder.append(f"\treturn {class_info.qualified_name}::GetPrivateStaticClass();\n")
        builder.append("}\n\n")

        builder.append(f"struct {class_info.generated_statics_name}\n")
        builder.append("{\n")
        builder.append("\tstatic const Durin::DurinCodeGen::FClassParams ClassParams;\n")
        for prop in class_info.properties:
            builder.append(_property_decl(prop))
        if class_info.properties:
            builder.append("\tstatic const Durin::DurinCodeGen::FPropertyParamsBase* const PropertyParams[];\n")
        builder.append("};\n\n")

        for prop in class_info.properties:
            builder.append(_property_definition(class_info, prop, symbols))
        if class_info.properties:
            builder.append(f"const Durin::DurinCodeGen::FPropertyParamsBase* const {class_info.generated_statics_name}::PropertyParams[] = {{\n")
            for prop in class_info.properties:
                builder.append(f"\t&{class_info.generated_statics_name}::NewProp_{prop.name},\n")
            builder.append("};\n\n")

        property_params = f"{class_info.generated_statics_name}::PropertyParams" if class_info.properties else "nullptr"
        property_count = len(class_info.properties)
        builder.append(f"const Durin::DurinCodeGen::FClassParams {class_info.generated_statics_name}::ClassParams = {{\n")
        builder.append(f"\t{class_info.generated_helper_no_register_name},\n")
        builder.append(f"\t\"{class_info.qualified_name}\",\n")
        builder.append(f"\t\"{class_info.short_name}\",\n")
        builder.append(f"\t{property_params},\n")
        builder.append(f"\t{property_count}\n")
        builder.append("};\n\n")

        builder.append(f"Durin::DClass* {class_info.generated_helper_name}()\n")
        builder.append("{\n")
        builder.append(f"\tDurin::DClass*& Singleton = {class_info.registration_info_name}.OuterSingleton;\n")
        builder.append("\tif (!Singleton)\n")
        builder.append("\t{\n")
        builder.append(f"\t\tSingleton = Durin::DurinCodeGen::ConstructDClass({class_info.generated_statics_name}::ClassParams);\n")
        builder.append("\t}\n")
        builder.append("\treturn Singleton;\n")
        builder.append("}\n\n")

        if _constructor_mode(class_info) == "object_initializer" and not class_info.has_object_initializer_constructor:
            builder.append(f"{class_info.qualified_name}::{class_info.short_name}(const Durin::FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {{}}\n\n")

    if header.classes:
        defer_statics = f"Z_CompiledInDeferFile_{header.file_id}_Statics"
        builder.append(f"struct {defer_statics}\n")
        builder.append("{\n")
        builder.append("\tstatic constexpr Durin::FClassRegisterCompiledInInfo ClassInfo[] = {\n")
        for class_info in header.classes:
            builder.append(
                f"\t\t{{ {class_info.generated_helper_name}, {class_info.qualified_name}::StaticClass, "
                f"\"{class_info.qualified_name}\", &{class_info.registration_info_name} }},\n"
            )
        builder.append("\t};\n")
        builder.append("};\n\n")
        builder.append(f"static Durin::FRegisterCompiledInInfo Z_CompiledInDeferFile_{header.file_id}(\n")
        builder.append(f"\t{defer_statics}::ClassInfo,\n")
        builder.append(f"\t{len(header.classes)}\n")
        builder.append(");\n")

    return "".join(builder)


def _write_reflection_files(module_name: str, headers_to_regenerate: list[str], symbols: dict[str, object]) -> None:
    output_dir = utils.get_module_dht_output_dir(module_name)
    output_dir.mkdir(parents=True, exist_ok=True)

    for header in headers_to_regenerate:
        header_info = parse_reflection_header(module_name, header, exported_symbols=symbols)
        _resolve_header(header_info, symbols)

        header_filename = Path(header).stem
        utils.generate_file(output_dir / f"{header_filename}.gen.h", _generate_header_content(header_info))
        utils.generate_file(output_dir / f"{header_filename}.gen.cpp", _generate_cpp_content(header_info, symbols))

    utils.generate_file(output_dir / f"{module_name}.module.gen.cpp", "// Generated module reflection source.\n")


def generate_reflection_files(module_name: str) -> None:
    manifest_file_path = utils.get_module_manifest_file_path(module_name)
    old_manifest: ModuleManifest = load_module_manifest_file(module_name) if manifest_file_path.exists() else None
    new_manifest = make_new_module_manifest(module_name, old_manifest)
    headers_to_regenerate = get_reflection_headers_requiring_regeneration(old_manifest, new_manifest)
    symbols = _load_available_symbols(module_name)
    _write_reflection_files(module_name, headers_to_regenerate, symbols)
    save_module_manifest_file(new_manifest)
