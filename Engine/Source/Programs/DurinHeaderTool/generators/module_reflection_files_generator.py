from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed
import logging
import os
import time

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
    dep_modules = configs.collect_all_dependent_module_with_export_file(module_name)
    logging.info("[DHT] Reflection %s: loading exports from %d modules", module_name, len(dep_modules))
    for dep_module in dep_modules:
        export_file_path = utils.get_module_export_file_path(dep_module)
        if not export_file_path.exists():
            raise FileNotFoundError(f"Export file for module '{dep_module}' not found at expected path: {export_file_path}")
        export_info = load_module_export_file(export_file_path)
        symbols.update(export_info.symbols)
    logging.info("[DHT] Reflection %s: loaded %d reflected symbols", module_name, len(symbols))
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


def _manifest_contract_changed(old_manifest: ModuleManifest, new_manifest: ModuleManifest) -> bool:
    return (
        old_manifest.schema_version != new_manifest.schema_version
        or old_manifest.tool_version != new_manifest.tool_version
        or old_manifest.symbol_name_scheme != new_manifest.symbol_name_scheme
        or old_manifest.profile != new_manifest.profile
        or old_manifest.platform != new_manifest.platform
        or old_manifest.generator_options_hash != new_manifest.generator_options_hash
    )


def _dependency_exports_changed(old_manifest: ModuleManifest, new_manifest: ModuleManifest) -> bool:
    if set(old_manifest.dep_module_exports.keys()) != set(new_manifest.dep_module_exports.keys()):
        return True
    for dep_module, new_fingerprint in new_manifest.dep_module_exports.items():
        old_fingerprint = old_manifest.dep_module_exports.get(dep_module)
        if old_fingerprint != new_fingerprint:
            return True
    return False


def _generated_output_paths(module_name: str, header: str) -> list[Path]:
    output_dir = utils.get_module_dht_output_dir(module_name)
    header_filename = Path(header).stem
    return [
        output_dir / f"{header_filename}.gen.h",
        output_dir / f"{header_filename}.gen.cpp",
    ]


def _generated_outputs_missing(module_name: str, header: str) -> bool:
    return any(not output_path.exists() for output_path in _generated_output_paths(module_name, header))


def _symbol_dependency_snapshot(symbol: object) -> dict[str, str]:
    return {
        "generatedHelperName": getattr(symbol, "generatedHelperName", ""),
        "api": getattr(symbol, "api", ""),
        "baseQualifiedName": getattr(symbol, "baseQualifiedName", ""),
    }


def _resolved_symbol_dependencies_for_header(header_info: ReflectedHeaderInfo, symbols: dict[str, object]) -> dict[str, dict[str, str]]:
    dependencies: dict[str, dict[str, str]] = {}
    for class_info in header_info.classes:
        if class_info.base_qualified_name in symbols:
            dependencies[class_info.base_qualified_name] = _symbol_dependency_snapshot(symbols[class_info.base_qualified_name])
        for prop in class_info.properties:
            if prop.referenced_type in symbols:
                dependencies[prop.referenced_type] = _symbol_dependency_snapshot(symbols[prop.referenced_type])
    return dependencies


def _header_symbol_dependencies_changed(header: str, old_manifest: ModuleManifest, symbols: dict[str, object]) -> bool:
    if header not in old_manifest.resolved_symbol_dependencies:
        return True
    for symbol_name, old_snapshot in old_manifest.resolved_symbol_dependencies[header].items():
        current_symbol = symbols.get(symbol_name)
        if current_symbol is None:
            return True
        if _symbol_dependency_snapshot(current_symbol) != old_snapshot:
            return True
    return False


def get_reflection_headers_requiring_regeneration(
    module_name: str,
    old_manifest: ModuleManifest,
    new_manifest: ModuleManifest,
    symbols: dict[str, object] | None = None,
) -> list[str]:
    if old_manifest is None:
        return list(new_manifest.reflect_headers.keys())

    if _manifest_contract_changed(old_manifest, new_manifest):
        return list(new_manifest.reflect_headers.keys())

    headers_requiring_regeneration = []
    dependency_exports_changed = _dependency_exports_changed(old_manifest, new_manifest)
    if dependency_exports_changed and symbols is None:
        return list(new_manifest.reflect_headers.keys())

    for header, new_fingerprint in new_manifest.reflect_headers.items():
        old_fingerprint = old_manifest.reflect_headers.get(header)
        if (
            old_fingerprint != new_fingerprint
            or _generated_outputs_missing(module_name, header)
            or header not in old_manifest.resolved_symbol_dependencies
            or (dependency_exports_changed and _header_symbol_dependencies_changed(header, old_manifest, symbols))
        ):
            headers_requiring_regeneration.append(header)
        else:
            new_manifest.resolved_symbol_dependencies[header] = old_manifest.resolved_symbol_dependencies.get(header, {})
    return headers_requiring_regeneration


def make_new_module_manifest(module_name: str, old_manifest: ModuleManifest = None) -> ModuleManifest:
    manifest = ModuleManifest(
        module_name=module_name,
        profile=configs.PROFILE_NAME,
        platform=configs.ARCH,
        tool_version=TOOL_VERSION,
        symbol_name_scheme=SYMBOL_NAME_SCHEME,
        generator_options_hash="resolved-symbol-dependencies-v1",
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


def _generate_reflection_output_impl(module_name: str, header: str, symbols: dict[str, object]) -> dict[str, object]:
    header_info = parse_reflection_header(module_name, header, exported_symbols=symbols)
    _resolve_header(header_info, symbols)
    return {
        "header": header,
        "header_content": _generate_header_content(header_info),
        "cpp_content": _generate_cpp_content(header_info, symbols),
        "class_count": len(header_info.classes),
        "property_count": sum(len(class_info.properties) for class_info in header_info.classes),
        "resolved_symbol_dependencies": _resolved_symbol_dependencies_for_header(header_info, symbols),
    }


def _generate_reflection_output_worker(args):
    module_name, header, symbols, arch, profile = args

    import configs as worker_configs
    from generators.module_reflection_files_generator import _generate_reflection_output_impl as worker_generate

    worker_configs.ARCH = arch
    worker_configs.PROFILE_NAME = profile
    worker_configs.init_configs()

    start_time = time.perf_counter()
    result = worker_generate(module_name, header, symbols)
    result["elapsed_ms"] = (time.perf_counter() - start_time) * 1000.0
    return result


def _write_reflection_output(module_name: str, result: dict[str, object], manifest: ModuleManifest) -> None:
    output_dir = utils.get_module_dht_output_dir(module_name)
    output_dir.mkdir(parents=True, exist_ok=True)

    header = result["header"]
    header_filename = Path(header).stem
    utils.generate_file(output_dir / f"{header_filename}.gen.h", result["header_content"])
    utils.generate_file(output_dir / f"{header_filename}.gen.cpp", result["cpp_content"])
    manifest.resolved_symbol_dependencies[header] = result["resolved_symbol_dependencies"]
    logging.info(
        "[DHT] Reflection %s: wrote %s.gen.* (%d classes, %d properties) in %.0f ms",
        module_name,
        header_filename,
        result["class_count"],
        result["property_count"],
        result["elapsed_ms"],
    )


def _write_reflection_files(module_name: str, headers_to_regenerate: list[str], symbols: dict[str, object], manifest: ModuleManifest) -> None:
    output_dir = utils.get_module_dht_output_dir(module_name)
    output_dir.mkdir(parents=True, exist_ok=True)

    if headers_to_regenerate:
        worker_count = min(len(headers_to_regenerate), os.cpu_count() or 1, 8)
        if worker_count > 1:
            logging.info(
                "[DHT] Reflection %s: parsing %d headers with %d workers",
                module_name,
                len(headers_to_regenerate),
                worker_count,
            )
        results: list[dict[str, object]]
        worker_args = [
            (module_name, header, symbols, configs.ARCH, configs.PROFILE_NAME)
            for header in headers_to_regenerate
        ]
        if worker_count == 1:
            results = [_generate_reflection_output_worker(args) for args in worker_args]
        else:
            try:
                with ProcessPoolExecutor(max_workers=worker_count) as executor:
                    futures = [executor.submit(_generate_reflection_output_worker, args) for args in worker_args]
                    results = [future.result() for future in as_completed(futures)]
            except Exception as error:
                logging.warning(
                    "[DHT] Reflection %s: parallel parsing failed (%s), falling back to sequential parsing",
                    module_name,
                    error,
                )
                results = [_generate_reflection_output_worker(args) for args in worker_args]

        header_order = {header: index for index, header in enumerate(configs.get_module_config(module_name).reflect_headers)}
        for result in sorted(results, key=lambda item: header_order[item["header"]]):
            _write_reflection_output(module_name, result, manifest)
    else:
        logging.info(
            "[DHT] Reflection %s: no headers require regeneration",
            module_name,
        )

    utils.generate_file(output_dir / f"{module_name}.module.gen.cpp", "// Generated module reflection source.\n")


def generate_reflection_files(module_name: str) -> None:
    start_time = time.perf_counter()
    logging.info("[DHT] Reflection %s: preparing manifest", module_name)
    manifest_file_path = utils.get_module_manifest_file_path(module_name)
    old_manifest: ModuleManifest = load_module_manifest_file(module_name) if manifest_file_path.exists() else None
    new_manifest = make_new_module_manifest(module_name, old_manifest)

    symbols = None
    dependency_exports_changed = (
        old_manifest is not None
        and not _manifest_contract_changed(old_manifest, new_manifest)
        and _dependency_exports_changed(old_manifest, new_manifest)
    )
    if dependency_exports_changed:
        symbols = _load_available_symbols(module_name)

    headers_to_regenerate = get_reflection_headers_requiring_regeneration(
        module_name,
        old_manifest,
        new_manifest,
        symbols,
    )
    if dependency_exports_changed:
        logging.info(
            "[DHT] Reflection %s: dependency exports changed, %d/%d headers affected",
            module_name,
            len(headers_to_regenerate),
            len(new_manifest.reflect_headers),
        )

    total_headers = len(new_manifest.reflect_headers)
    skipped_headers = total_headers - len(headers_to_regenerate)
    logging.info(
        "[DHT] Reflection %s: %d/%d headers require regeneration (%d skipped)",
        module_name,
        len(headers_to_regenerate),
        total_headers,
        skipped_headers,
    )
    if headers_to_regenerate and symbols is None:
        symbols = _load_available_symbols(module_name)
    elif not headers_to_regenerate and symbols is None:
        logging.info("[DHT] Reflection %s: no headers require regeneration, skipped symbols loading", module_name)

    _write_reflection_files(module_name, headers_to_regenerate, symbols or {}, new_manifest)
    save_module_manifest_file(new_manifest)
    elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    logging.info("[DHT] Reflection %s: finished in %.0f ms", module_name, elapsed_ms)
