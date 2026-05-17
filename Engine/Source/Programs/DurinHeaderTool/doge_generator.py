from dataclasses import asdict
import sys
import os
import logging
import json
from enum import Enum

from durin_parser import DHTConstructorType, intrinsic_core_objects
from durin_exports import *

def append_include(builder, include_file):
    builder.append(f'#include "{include_file}"\n')

MACRO_NEWLINE = " \\\n"

def append_macro_line(builder, content, indent_level=0, is_last_line=False):
    indent = "\t" * indent_level
    builder.append(indent + content + ("\n" if is_last_line else MACRO_NEWLINE))

def append_comment_segmentation(builder, comment):
    comment_size = len(comment)
    star_num = 0 if comment_size > 50 else 50 - comment_size
    builder.append(f"// ********* {comment} {star_num * '*'} \n")

class DHTCodeGen_H:
    @staticmethod
    def generate(header_meta, filepath) -> None:
        with open(filepath, 'w') as file:
            builder = []
            builder.append("// Generated code exported from DurinHeaderTool.\n\n")
            builder.append("#pragma once\n\n")

            for class_meta in header_meta.classes:
                DHTCodeGen_H.append_class(builder, class_meta, header_meta.fid)

            DHTCodeGen_H.append_fid_definition(builder, header_meta.fid)
            file.writelines(builder)

    @staticmethod
    def append_class(builder, class_meta, fid):
        append_comment_segmentation(builder, f"Begin Class {class_meta.name}")
        builder.append(f"{class_meta.api} auto {class_meta.construct_noregister_func_name}() -> DClass*;\n")
        DHTCodeGen_H.append_generate_body_code(builder, class_meta, fid)
        append_comment_segmentation(builder, f"End Class {class_meta.name}")
        builder.append("\n")

    @staticmethod
    def append_generate_body_code(builder, class_meta, fid) -> None:
        if class_meta.generate_body_line == 0:
            logging.warning("No generated body line found for class: %s", class_meta.name)
            return

        generated_body_id = fid + "_" + str(class_meta.generate_body_line)
        generated_body_macro_name = generated_body_id + "_GENERATED_BODY"

        no_pure_decls_macro_name = generated_body_id + "_INCLASS_NO_PURE_DECLS"
        enhanced_constructors_macro_name = generated_body_id + "_ENHANCED_CONSTRUCTORS"

        DHTCodeGen_H.append_inclass_no_pure_decls(builder, no_pure_decls_macro_name, class_meta)
        DHTCodeGen_H.append_enhanced_constructors(builder, enhanced_constructors_macro_name, class_meta)

        append_macro_line(builder, f"#define {generated_body_macro_name}", 0)
        append_macro_line(builder, "public:", 1)
        append_macro_line(builder, f"{no_pure_decls_macro_name}", 2)
        append_macro_line(builder, f"{enhanced_constructors_macro_name}", 2)
        append_macro_line(builder, "private:", 1, is_last_line=True)

        builder.append("\n")
    
    @staticmethod
    def append_inclass_no_pure_decls(builder, macro_name, class_meta) -> None:
        append_macro_line(builder, f"#define {macro_name}", 0)

        append_macro_line(builder, "private:", 1)
        append_macro_line(builder, f"friend struct {class_meta.construct_statics};", 2)
        append_macro_line(builder, f"static DClass* GetPrivateStaticClass();", 2)
        append_macro_line(builder, f"friend {class_meta.api} auto {class_meta.construct_noregister_func_name}() -> DClass*;", 2)
        append_macro_line(builder, "public:", 1)
        append_macro_line(builder, f"DECLARE_CLASS({class_meta.name}, {class_meta.superclass}, {class_meta.construct_noregister_func_name})", 2, is_last_line=True)

        builder.append("\n")

    @staticmethod
    def append_enhanced_constructors(builder, macro_name, class_meta) -> None:
        append_macro_line(builder, f"#define {macro_name}", 0)

        # Append constructor if not declared by original header file
        if class_meta.constructor_type is DHTConstructorType.OBJECT_INITIALIZER and not class_meta.has_object_initializer_constructor:
            append_macro_line(builder, f"/** Default Object Initializer Constructor **/", 1)
            append_macro_line(builder, f"NO_API {class_meta.name}(const FObjectInitializer& ObjectInitializer);", 1)

        # Append destructor if not declared by original header file
        if not class_meta.has_destructor:
            append_macro_line(builder, f"/** Default Destructor **/", 1)
            append_macro_line(builder, f"NO_API ~{class_meta.name}() override = default;", 1)

        # Append deleted move- and copy-constructors
        append_macro_line(builder, f"/** Deleted move- and copy-constructors, should never be used */", 1)
        classname = class_meta.name
        append_macro_line(builder, f"{classname}({classname}&&) = delete;", 1)
        append_macro_line(builder, f"{classname}(const {classname}&) = delete;", 1)

        # Add corresponding constructor call function
        if class_meta.constructor_type is DHTConstructorType.OBJECT_INITIALIZER:
            append_macro_line(builder, f"DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL({classname})", 1)
        elif class_meta.constructor_type is DHTConstructorType.DEFAULT:
            append_macro_line(builder, f"DEFINE_DEFAULT_CONSTRUCTOR_CALL({classname})", 1)

        builder.append("\n")

    @staticmethod
    def append_fid_definition(builder, fid) -> None:
        builder.append("#undef CURRENT_FILE_ID\n")
        builder.append("#define CURRENT_FILE_ID " + fid + "\n")


class DHTCodeGen_Cpp:
    @staticmethod
    def generate(header_meta, filepath) -> None:
        global _header_meta
        _header_meta = header_meta

        with open(filepath, 'w') as file:
            builder = []
            builder.append("// Generated code exported from DurinHeaderTool.\n\n")
            DHTCodeGen_Cpp.append_includes(header_meta, builder)
            DHTCodeGen_Cpp.append_cross_module_references(header_meta, builder)
            DHTCodeGen_Cpp.append_classes(header_meta, builder)
            DHTCodeGen_Cpp.append_registration(header_meta, builder)
            file.writelines(builder)

    @staticmethod
    def append_includes(header_meta, builder) -> None:
        append_include(builder, "DObject/GeneratedCppIncludes.h")
        append_include(builder, header_meta.include_path)
        builder.append("\n")
    

    @staticmethod
    def append_cross_module_references(header_meta, builder) -> None:
        append_comment_segmentation(builder, "Begin Cross Module References")
        for class_meta in header_meta.classes:
            superclass = class_meta.superclass
            # superclass_api_macro = module_meta.get_api_macro(superclass)
            # builder.append(f"{superclass_api_macro} DClass* Z_Construct_DClass_{superclass}();\n")
            # builder.append(f"{module_meta.api_macro} DClass* {class_meta.construct_func_name}();\n")
            # builder.append(f"{module_meta.api_macro} DClass* {class_meta.construct_noregister_func_name}();\n")
        append_comment_segmentation(builder, "End Cross Module References")
        builder.append("\n")

    @staticmethod
    def append_classes(header_meta, builder) -> None:
        for class_meta in header_meta.classes:
            classname = class_meta.name
            
            append_comment_segmentation(builder, f"Begin Class {classname}")

            DHTCodeGen_Cpp.append_class_construct_noregister_function(builder, class_meta)
            DHTCodeGen_Cpp.append_class_construct_function(builder, class_meta)
            DHTCodeGen_Cpp.append_default_constructor_impl(builder, class_meta)

            append_comment_segmentation(builder, f"End Class {classname}")
            builder.append("\n")
        builder.append("\n")

    @staticmethod
    def append_class_construct_noregister_function(builder, class_meta) -> None:
        builder.append(f"IMPLEMENT_CLASS_NO_AUTO_REGISTRATION({class_meta.name});\n")
        builder.append("\n")

        builder.append(f"auto {class_meta.construct_noregister_func_name}() -> DClass*\n")
        builder.append("{\n")
        builder.append(f"\treturn {class_meta.name}::GetPrivateStaticClass();\n")
        builder.append("}\n")
        builder.append("\n")

    @staticmethod
    def append_class_construct_function(builder, class_meta) -> None:
        DHTCodeGen_Cpp.append_class_construct_statics_definition(builder, class_meta)

        builder.append(f"auto {class_meta.construct_func_name}() -> DClass*\n")
        builder.append("{\n")
        builder.append(f"\tDClass*& Singleton = {class_meta.registration_info_name}.OuterSingleton;\n")
        builder.append(f"\tif (!Singleton)\n")
        builder.append("\t{\n")
        builder.append(f"\t\tSingleton = DurinCodeGen::ConstructDClass({class_meta.construct_func_name}_Statics::ClassParams);\n")
        builder.append("\t}\n")
        builder.append("\treturn Singleton;\n")
        builder.append("}\n")
        builder.append("\n")

    @staticmethod
    def append_class_construct_statics_definition(builder, class_meta) -> None:
        construct_statics = class_meta.construct_statics
        # Append struct definition
        builder.append(f"struct {construct_statics}\n")
        builder.append("{\n")
        builder.append("\tstatic const DurinCodeGen::FClassParams ClassParams;\n")
        prop_param_list = []
        
        if len(class_meta.properties) > 0:
            for property_meta in class_meta.properties:
                newparams = property_meta.append_param_declaration(builder)
                prop_param_list.extend(newparams)
            builder.append("\tstatic const DurinCodeGen::FPropertyParamsBase* const PropertyParams[];\n")

        builder.append("};\n")
        builder.append("\n")

        DHTCodeGen_Cpp.append_class_construct_statics_propertyparams_definition(builder, class_meta, prop_param_list)
        DHTCodeGen_Cpp.append_class_construct_statics_classparams_definition(builder, class_meta)
        builder.append("\n")

    @staticmethod
    def append_class_construct_statics_classparams_definition(builder, class_meta) -> None:
        builder.append(f"const DurinCodeGen::FClassParams {class_meta.construct_statics}::ClassParams = {{\n")
        builder.append(f"\t{class_meta.name}::StaticClass,\n")
        builder.append(f"\t\"{class_meta.name}\"\n")
        builder.append("};\n")
        builder.append("\n")

    @staticmethod
    def append_class_construct_statics_propertyparams_definition(builder, class_meta, prop_param_list) -> None:
        if len(prop_param_list) == 0:
            return

        for property_meta in class_meta.properties:
            property_meta.append_param_definition(builder, class_meta)
        
        builder.append(f"const DurinCodeGen::FPropertyParamsBase* const {class_meta.construct_statics}::PropertyParams[] = {{\n")
        for prop_param in prop_param_list:
            builder.append(f"\t&{class_meta.construct_statics}::{prop_param},\n")
        builder.append("};\n")
        builder.append("\n")

    @staticmethod
    def append_default_constructor_impl(builder, class_meta) -> None:
        if class_meta.constructor_type is DHTConstructorType.OBJECT_INITIALIZER and not class_meta.has_object_initializer_constructor:
            builder.append(f"{class_meta.name}::{class_meta.name}(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer) {{}}\n")
            builder.append("\n")

    @staticmethod
    def append_registration(header_meta, builder) -> None:
        if len(header_meta.classes) == 0:
            return
        append_comment_segmentation(builder, f"Begin Registration")

        defer_registration_statics = f"Z_CompiledInDeferFile_{header_meta.fid}_Statics"

        builder.append(f"struct {defer_registration_statics}\n")
        builder.append("{\n")
        builder.append("\tstatic constexpr FClassRegisterCompiledInInfo ClassInfo[] = {\n")
        for class_meta in header_meta.classes:
            builder.append(f"\t\t{{ {class_meta.construct_func_name}, {class_meta.name}::StaticClass, \"{class_meta.name}\", &{class_meta.registration_info_name} }},\n")
        builder.append("\t};\n")
        builder.append("};\n")
        builder.append("\n")

        builder.append(f"static FRegisterCompiledInInfo Z_CompiledInDeferFile_{header_meta.fid}" + "(\n")
        builder.append(f"\t{defer_registration_statics}::ClassInfo,\n")
        builder.append(f"\t{len(header_meta.classes)}\n")
        builder.append(");\n")
        builder.append("\n")

        append_comment_segmentation(builder, f"End Registration")

def generate_module_manifest_file(module_manifest: ModuleManifest, manifest_file: str) -> None:
    os.makedirs(os.path.dirname(manifest_file), exist_ok=True)
    output_data = module_manifest.to_json_dict()
    content = json.dumps(output_data, indent=4)
    generate_file(manifest_file, content)

def generate_file(filepath: str, content: str, compare: bool = True) -> None:
    os.makedirs(os.path.dirname(filepath), exist_ok=True)
    # Only write file if content has changed
    if compare and os.path.exists(filepath):
        with open(filepath, "r") as f:
            existing_content = f.read()
            if existing_content == content:
                logging.debug(f"No changes detected for {filepath}. Skipping write.")
                return
    # Write new content to file no matter what
    with open(filepath, "w") as f:
        f.write(content)