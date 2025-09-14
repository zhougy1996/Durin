import sys
import os
import logging
import json

import clang.cindex
from clang.cindex import TokenKind

# Constants
current_file_path = os.path.abspath(__file__)
current_dir = os.path.dirname(current_file_path)
doge_source_dir = os.path.abspath(os.path.join(current_dir, "../.."))
doge_root_dir = os.path.abspath(os.path.join(doge_source_dir, "../.."))
clang_lib_dir = os.path.join(doge_source_dir, "ThirdParty/clang/bin")
clang_args = [
    "-x",
    "c++",
    "-std=c++20",
    "-D_DHT_PARSER",
    "-DNDEBUG",
    "-D_MSC_VER=1930",
    "-w",
    "-MG",
    "-M",
    "-ferror-limit=0",
    "-o clangLog.txt"
]
intrinsic_core_objects = [
    "DObject",
    "DClass",
    "DStruct",
    "DEnum"
]
macro_newline = " \\\n"

module_meta = None
header_meta = None
header_source = None
translation_unit = None

def init_clang():
    if module_meta.api_macro:
        clang_args.append(f"-D{module_meta.api_macro}=")
        # DHT_GENERATED_BODY() will be identified as a function declaration
        clang_args.append("-DGENERATED_BODY(...)=void DHT_GENERATED_BODY();")
        # DHT_CLASS() will be identified as a function declaration
        clang_args.append("-DDCLASS(...)=__attribute__((annotate(\"DCLASS,\" #__VA_ARGS__))) void DHT_CLASS();") 
        clang_args.append("-DDPROPERTY(...)=__attribute__((annotate(\"DPROPERTY,\" #__VA_ARGS__)))")
        clang_args.append("-DDFUNCTION(...)=__attribute__((annotate(\"DFUNCTION,\" #__VA_ARGS__)))")

    clang.cindex.Config.set_library_path(clang_lib_dir)

def init_logging():
    logging.getLogger().setLevel(logging.INFO)
    logging.basicConfig(format='[%(levelname)s] %(message)s')

def parse_annotation(annotation_str) -> dict:
    subsections = [subsection.strip() for subsection in annotation_str.split(',') if subsection.strip()]  # split by comma
    annotation_dict = {}
    for subsection in subsections:
        if '=' in subsection:
            key, value = subsection.split('=', 1)
            annotation_dict[key.strip()] = value.strip().strip('"')
        else:
            annotation_dict[subsection] = True
    return annotation_dict

def extract_annotations(cursor) -> dict:
    for child_cursor in cursor.get_children():
        if child_cursor.kind == clang.cindex.CursorKind.ANNOTATE_ATTR:
            annotation_str = child_cursor.spelling
            return parse_annotation(annotation_str)
    return {}

def strip_macro_paren_prefix(tokens):
    result = []
    paren_num = 0
    for i in range(len(tokens)):
        if tokens[i].spelling == "(":
            paren_num += 1
        elif tokens[i].spelling == ")":
            paren_num -= 1
            if paren_num == 0:
                result = tokens[i+1:]
                break
    return result

def write_include(file, include_file):
    file.write(f"#include \"{include_file}\"\n")

def write_macro(file, macro_lines):
    if len(macro_lines) == 0:
        return
    
    i = 0
    while i < len(macro_lines) - 1:
        file.write(macro_lines[i] + macro_newline)
        i += 1
    file.write(macro_lines[i] + "\n")

def write_comment_segmentation(file, comment):
    comment_size = len(comment)
    star_num = 0 if comment_size > 50 else 50 - comment_size
    file.write(f"// ********* {comment} {star_num * '*'} \n")


class DHTModule:
    name: str
    source_dir: str
    module_info_filepath: str
    api_macro: str
    dht_output_dir: str
    classes: list

    def __init__(self, source_dir, dht_output_dir):
        self.dht_output_dir = dht_output_dir
        self.source_dir = source_dir
        self.name = os.path.basename(self.source_dir)
        self.module_info_filepath = os.path.join(self.dht_output_dir, f"ModuleInfo.json")
        self.api_macro = self.name.upper() + "_API"

        with open(self.module_info_filepath, "r") as f:
            module_info = json.load(f)
            self.classes = module_info.get("DClasses", [])
            logging.debug("Registered DClasses for module '%s': %s", self.name, self.classes)

    def get_api_macro(self, classname):
        if classname in intrinsic_core_objects:
            return "CORE_API"
        elif classname in self.classes:
            return self.api_macro
        else:
            return "DLLIMPORT"

# Property meta info, annotated with DPROPERTY()
class DHTProperty:
    name: str
    type: str
    annotations: dict
    cursor: clang.cindex.Cursor
    tokens: list

    def __init__(self):
        self.name = ""
        self.type = ""
        self.annotations = {}
        self.tokens = []

    def construct(self, property_cursor, annotations: dict, tokens: list):
        assert property_cursor.kind == clang.cindex.CursorKind.FIELD_DECL
        self.name = property_cursor.spelling
        self.annotations = annotations
        self.tokens = tokens
        self.cursor = property_cursor
        self.type = self.extract_property_type(tokens)

    def extract_property_type(self, tokens) -> str:
        result = []
        for i, token in enumerate(tokens[:-2]):
            spelling = token.spelling
            if token.kind == TokenKind.KEYWORD:
                result.append(spelling)
                result.append(" ")
            elif token.kind == TokenKind.IDENTIFIER:
                result.append(spelling)
                if i + 1 < len(tokens) - 2 and tokens[i+1].kind != TokenKind.PUNCTUATION:
                    result.append(" ")
            else:
                result.append(spelling)
        return "".join(result).strip()

# Function meta info, annotated with DFUNCTION()
class DHTFunction:
    name: str
    return_type: str
    parameters: list

    def __init__(self, name: str, return_type: str):
        self.name = name
        self.return_type = return_type
        self.parameters = []



# Class meta info, annotated with DCLASS()
# DCLASS() should not be nested in other classes or namespaces
class DHTClass:
    name: str
    cursor: clang.cindex.Cursor
    clang_tokens: list
    api: str
    superclass: str
    annotations: dict
    generate_body_line: int
    properties: list
    functions: list
    registration_info_name: str
    construct_func_name: str
    construct_noregister_func_name: str
    construct_statics: str

    def __init__(self):
        self.generate_body_line = 0

    def construct(self, class_cursor, annotations):
        self.name = class_cursor.spelling
        self.properties = []
        self.functions = []
        self.annotations = annotations
        self.cursor = class_cursor
        self.clang_tokens = list(class_cursor.get_tokens())
        self.registration_info_name = f"Z_Registration_Info_DClass_{self.name}"
        self.construct_func_name =  f"Z_Construct_DClass_{self.name}"
        self.construct_noregister_func_name =  f"{self.construct_func_name}_NoRegister"
        self.construct_statics = f"{self.construct_func_name}_Statics"

        self.construct_class_declaration()
        self.construct_members()

    def construct_members(self):
        for child_cursor in self.cursor.get_children():
            if child_cursor.kind == clang.cindex.CursorKind.FIELD_DECL:
                self.add_property(child_cursor)
            elif child_cursor.kind == clang.cindex.CursorKind.CXX_METHOD:
                if child_cursor.spelling == "DHT_GENERATED_BODY":
                    self.generate_body_line = child_cursor.location.line
                else:
                    self.add_function(child_cursor)

    def construct_class_declaration(self):
        class_tokens = [token.spelling for token in self.clang_tokens]
        declaration_token_end = class_tokens.index("{") if "{" in class_tokens else len(class_tokens)
        declaration_tokens = class_tokens[:declaration_token_end]

        # Extract export API information
        self.api = module_meta.api_macro

        # Extract the first superclass, the super class must be a DObject class
        superclass_begin = 0
        superclass_end = len(declaration_tokens)
        if ":" in declaration_tokens:
            colon_index = declaration_tokens.index(":")
            if declaration_tokens[colon_index + 1] in ["public", "protected", "private"]:
                assert colon_index + 2 < len(declaration_tokens)
                superclass_begin = colon_index + 2
            else:
                assert colon_index + 1 < len(declaration_tokens)
                superclass_begin = colon_index + 1

            # end when find the first "," after superclass_begin
            for i in range(superclass_begin, len(declaration_tokens)):
                if declaration_tokens[i] == ",":
                    superclass_end = i
                    break

            self.superclass = "".join(declaration_tokens[superclass_begin:superclass_end])

    def extract_subtokens(self, extent) -> list:
        subtokens = []
        started = False
        for token in self.clang_tokens:
            token_start = token.extent.start.offset
            token_end = token.extent.end.offset

            if not started:
                if token_end >= extent.start.offset:
                    started = True
                else:
                    continue

            if token_start > extent.end.offset:
                break

            subtokens.append(token)

        return subtokens

    def extract_subtokens_without_macro(self, cursor):
        tokens = self.extract_subtokens(cursor.extent)
        tokens_without_macro = strip_macro_paren_prefix(tokens)
        return tokens_without_macro

    def add_property(self, property_cursor):
        if property_cursor.kind == clang.cindex.CursorKind.FIELD_DECL:
            annotations = extract_annotations(property_cursor)
            if annotations and "DPROPERTY" in annotations:
                tokens = self.extract_subtokens_without_macro(property_cursor)
                property_meta = DHTProperty()
                property_meta.construct(property_cursor, annotations, tokens)
                self.properties.append(property_meta)

    def add_function(self, function_cursor):
        if function_cursor.kind == clang.cindex.CursorKind.CXX_METHOD:
            annotations = extract_annotations(function_cursor)
            if annotations and "DFUNCTION" in annotations:
                pass
                # tokens_without_macro = self.strip_macro_paren_prefix(tokens)

# Header meta info, contains multiple classes
class DHTHeader:
    name: str
    filepath: str
    relative_path: str
    fid: str
    include_path: str
    classes: list

    # Initialize the global module_meta first before create DHTHeader
    def __init__(self, filepath):
        self.filepath = filepath
        self.name, _ = os.path.splitext(os.path.basename(filepath))

        source_dir = module_meta.source_dir
        if not source_dir:
            logging.error("Source dir is empty when create DHTHeader.")
            sys.exit(1)

        if not input_header.startswith(source_dir):
            logging.error("Input header: %s", input_header)
            logging.error("Module source dir: %s", source_dir)
            sys.exit(1)

        self.relative_path = os.path.relpath(input_header, source_dir).replace("\\", "/")
        if self.relative_path.startswith("Public/"):
            self.include_path = self.relative_path[len("Public/"):]
        elif self.relative_path.startswith("Private/"):
            self.include_path = self.relative_path[len("Private/"):]
        else:
            self.include_path = self.relative_path

        self.fid = "FID_DOGE_" + module_meta.name + "_" + self.relative_path.replace("/", "_").replace(".", "_")

        self.classes = []

    def construct(self, header_cursor):
        self.construct_children(header_cursor)
        return True
    
    def construct_children(self, header_cursor):
        cursors = list(header_cursor.get_children())
        i = 0
        while i < (len(cursors) - 1):
            cursor = cursors[i]
            if cursor.kind == clang.cindex.CursorKind.FUNCTION_DECL:
                added = False
                if cursor.spelling == "DHT_CLASS":
                    added = self.add_class(cursors[i+1], extract_annotations(cursor))
                if added:
                    i += 1
            i += 1

    def add_class(self, class_cursor, annotations) -> bool:
        if "DCLASS" in annotations and class_cursor.kind == clang.cindex.CursorKind.CLASS_DECL:
            class_meta = DHTClass()
            class_meta.construct(class_cursor, annotations)
            self.classes.append(class_meta)
            return True
        return False
                

class DHTParser:
    def __init__(self):
        pass

    def parse_header(self, header_path) -> DHTHeader:
        clang_index = clang.cindex.Index.create()
        logging.debug("Parsing header: %s", header_path)

        global translation_unit
        translation_unit = clang_index.parse(header_path, clang_args)

        if not translation_unit:
            logging.error("Unable to load translation unit from %s", header_path)
            return None
        
        # if translation_unit.diagnostics:
        #     for diag in translation_unit.diagnostics:
        #         logging.debug(f"Diagnostic: {diag.spelling}")

        with open(header_path, 'r') as f:
            global header_source
            header_source = f.read()
            if not header_source:
                logging.error("Failed to read header source from: %s", header_path)
                sys.exit(1)

        header_meta = DHTHeader(header_path)
        header_meta.construct(translation_unit.cursor)

        return header_meta

class DHTCodeGen_H:
    @staticmethod
    def generate(filepath) -> None:
        with open(filepath, 'w') as file:
            file.write("// Generated code exported from DogeHeaderTool.\n\n")
            file.write("#pragma once\n\n")

            for class_meta in header_meta.classes:
                DHTCodeGen_H.write_class(file, class_meta, header_meta.fid)

            DHTCodeGen_H.write_fid_definition(file, header_meta.fid)

    @staticmethod
    def write_class(file, class_meta, fid):
        write_comment_segmentation(file, f"Begin Class {class_meta.name}")
        file.write(f"{class_meta.api} auto {class_meta.construct_noregister_func_name}() -> DClass*;\n")
        DHTCodeGen_H.write_generate_body_code(file, class_meta, fid)
        write_comment_segmentation(file, f"End Class {class_meta.name}")
        file.write("\n")


    @staticmethod
    def write_generate_body_code(file, class_meta, fid) -> None:
        if class_meta.generate_body_line == 0:
            logging.warning("No generated body line found for class: %s", class_meta.name)
            return

        generated_body_id = fid + "_" + str(class_meta.generate_body_line)
        generated_body_macro_name = generated_body_id + "_GENERATED_BODY"

        no_pure_decls_macro_name = generated_body_id + "_INCLASS_NO_PURE_DECLS"
        enhanced_constructors_macro_name = generated_body_id + "_ENHANCED_CONSTRUCTORS"

        DHTCodeGen_H.write_inclass_no_pure_decls(file, no_pure_decls_macro_name, class_meta)
        DHTCodeGen_H.write_code_enhanced_constructors(file, enhanced_constructors_macro_name, class_meta)

        lines = []
        lines.append(f"#define {generated_body_macro_name}")
        lines.append("public:")
        lines.append(f"\t{no_pure_decls_macro_name}")
        lines.append(f"\t{enhanced_constructors_macro_name}")
        lines.append("private:")

        write_macro(file, lines)
        file.write("\n")

    @staticmethod
    def write_inclass_no_pure_decls(file, macro_name, class_meta) -> str:
        lines = []
        lines.append(f"#define {macro_name}")
        lines.append("private:")
        lines.append(f"\tfriend struct {class_meta.construct_statics};")
        lines.append("\tstatic DClass* GetPrivateStaticClass();")
        lines.append(f"\tfriend {class_meta.api} auto {class_meta.construct_noregister_func_name}() -> DClass*;")
        lines.append("public:")
        lines.append(f"\tDECLARE_CLASS({class_meta.name}, {class_meta.superclass}, {class_meta.construct_noregister_func_name})")
        write_macro(file, lines)
        file.write("\n")

    @staticmethod
    def write_code_enhanced_constructors(file, macro_name, class_meta) -> str:
        lines = []
        lines.append(f"#define {macro_name}")
        lines.append(f"/** Deleted move- and copy-constructors, should never be used */")
        classname = class_meta.name
        lines.append(f"{classname}({classname}&&) = delete;")
        lines.append(f"{classname}(const {classname}&) = delete;")
        write_macro(file, lines)
        file.write("\n")

    @staticmethod
    def write_fid_definition(file, fid) -> None:
        file.write("#undef CURRENT_FILE_ID\n")
        file.write("#define CURRENT_FILE_ID " + fid + "\n")


class DHTCodeGen_Cpp:
    @staticmethod
    def generate(filepath) -> None:
        with open(filepath, 'w') as file:
            DHTCodeGen_Cpp.write_includes(file)
            DHTCodeGen_Cpp.write_cross_module_references(file)
            DHTCodeGen_Cpp.write_classes(file)
            DHTCodeGen_Cpp.write_registration(file)

    @staticmethod
    def write_includes(file) -> None:
        write_include(file, "DObject/GeneratedCppIncludes.h")
        write_include(file, header_meta.include_path)
        file.write("\n")

    @staticmethod
    def write_cross_module_references(file) -> None:
        write_comment_segmentation(file, "Begin Cross Module References")
        for class_meta in header_meta.classes:
            superclass = class_meta.superclass
            superclass_api_macro = module_meta.get_api_macro(superclass)
            file.write(f"{superclass_api_macro} DClass* Z_Construct_DClass_{superclass}();\n")
            file.write(f"{module_meta.api_macro} DClass* {class_meta.construct_func_name}();\n")
            file.write(f"{module_meta.api_macro} DClass* {class_meta.construct_noregister_func_name}();\n")
        write_comment_segmentation(file, "End Cross Module References")
        file.write("\n")
    
    @staticmethod
    def write_classes(file) -> None:
        for class_meta in header_meta.classes:
            classname = class_meta.name
            
            write_comment_segmentation(file, f"Begin Class {classname}")

            file.write(f"FClassRegistrationInfo {class_meta.registration_info_name};\n")
            DHTCodeGen_Cpp.write_class_construct_noregister_function(file, class_meta)
            DHTCodeGen_Cpp.write_class_construct_function(file, class_meta)

            write_comment_segmentation(file, f"End Class {classname}")
            file.write("\n")
        file.write("\n")

    @staticmethod
    def write_class_construct_noregister_function(file, class_meta) -> None:
        file.write(f"auto {class_meta.name}::GetPrivateStaticClass() -> DClass*\n")
        file.write("{\n")
        file.write(f"\tusing TClass = {class_meta.name};\n")
        file.write(f"\tDClass*& Singleton = {class_meta.registration_info_name}.InnerSingleton;\n")
        file.write(f"\tif (!Singleton)\n")
        file.write("\t{\n")

        file.write(f"\t\tSingleton = GetPrivateStaticClassBody(\n")
        file.write(f"\t\t\t\"{class_meta.name}\"\n")
        file.write(f"\t\t);\n")

        file.write("\t}\n")
        file.write("\treturn Singleton;\n")
        file.write("}\n")
        file.write("\n")

        file.write(f"auto {class_meta.construct_noregister_func_name}() -> DClass*\n")
        file.write("{\n")
        file.write(f"\treturn {class_meta.name}::GetPrivateStaticClass();\n")
        file.write("}\n")
        file.write("\n")

    @staticmethod
    def write_class_construct_function(file, class_meta) -> None:
        DHTCodeGen_Cpp.write_class_construct_statics(file, class_meta)

        file.write(f"auto {class_meta.construct_func_name}() -> DClass*\n")
        file.write("{\n")
        file.write(f"\tDClass*& Singleton = {class_meta.registration_info_name}.OuterSingleton;\n")
        file.write(f"\tif (!Singleton)\n")
        file.write("\t{\n")
        file.write(f"\t\tSingleton = DogeCodeGen::ConstructDClass({class_meta.construct_func_name}_Statics::ClassParams);\n")
        file.write("\t}\n")
        file.write("\treturn Singleton;\n")
        file.write("}\n")
        file.write("\n")

    @staticmethod
    def write_class_construct_statics(file, class_meta) -> None:
        construct_statics = class_meta.construct_statics
        file.write(f"struct {construct_statics}\n")
        file.write("{\n")
        file.write("\tstatic const DogeCodeGen::FClassParams ClassParams;\n")
        file.write("};\n")
        file.write("\n")

        file.write(f"const DogeCodeGen::FClassParams {class_meta.construct_statics}::ClassParams = {{\n")
        file.write(f"\t{class_meta.name}::StaticClass,\n")
        file.write(f"\t\"{class_meta.name}\"\n")
        file.write("};\n")
        file.write("\n")

    def write_registration(file) -> None:
        if len(header_meta.classes) == 0:
            return
        write_comment_segmentation(file, f"Begin Registration")

        defer_registration_statics = f"Z_CompiledInDeferFile_{header_meta.fid}_Statics"

        file.write(f"struct {defer_registration_statics}\n")
        file.write("{\n")
        file.write("\tstatic constexpr FClassRegisterCompiledInInfo ClassInfo[] = {\n")
        for class_meta in header_meta.classes:
            file.write(f"\t\t{{ {class_meta.construct_func_name}, {class_meta.name}::StaticClass, \"{class_meta.name}\", &{class_meta.registration_info_name} }},\n")
        file.write("\t};\n")
        file.write("};\n")
        file.write("\n")

        file.write(f"static FRegisterCompiledInInfo Z_CompiledInDeferFile_{header_meta.fid}" + "(\n")
        file.write(f"\t{defer_registration_statics}::ClassInfo,\n") 
        file.write(f"\t{len(header_meta.classes)}\n")
        file.write(");\n")
        file.write("\n")

        write_comment_segmentation(file, f"End Registration")


class DHTCodeGenerator:
    gen_h_file: str
    gen_cpp_file: str

    def __init__(self):
        pass

    def generate(self) -> None:
        filename, _ = os.path.splitext(os.path.basename(header_meta.filepath))
        output_dir = module_meta.dht_output_dir
        if output_dir is None:
            raise ValueError("Output directory is not set.")
        os.makedirs(output_dir, exist_ok=True)

        self.gen_h_file = os.path.join(output_dir, f"{filename}.gen.h")
        self.gen_cpp_file = os.path.join(output_dir, f"{filename}.gen.cpp")

        DHTCodeGen_H.generate(self.gen_h_file)
        DHTCodeGen_Cpp.generate(self.gen_cpp_file)


if __name__ == "__main__":
    # Example usage: python dht.py input_header.h target_directory module_source_dir
    # Parsing command line arguments
    if len(sys.argv) != 4:
        print("Usage: python dht.py <input_header> <target_directory> <module_source_dir>")
        sys.exit(1)

    init_logging()

    input_header = sys.argv[1]
    module_meta = DHTModule(sys.argv[3], sys.argv[2])

    init_clang()

    dht_parser = DHTParser()
    dht_code_generator = DHTCodeGenerator()

    header_meta = dht_parser.parse_header(input_header)
    if header_meta is None:
        logging.error("Failed to parse header.")
        sys.exit(1)

    dht_code_generator.generate()

