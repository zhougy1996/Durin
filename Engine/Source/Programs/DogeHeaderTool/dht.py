import sys
import os
import logging
import json

import clang.cindex

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

def init_clang():
    if module_meta.export_api:
        clang_args.append(f"-D{module_meta.export_api}=")
        clang_args.append("-DDCLASS(...)=__attribute__((annotate(\"DCLASS,\" #__VA_ARGS__))) DHT_CLASS();")
        clang_args.append("-DDPROPERTY(...)=__attribute__((annotate(\"DPROPERTY,\" #__VA_ARGS__)))")
        clang_args.append("-DDFUNCTION(...)=__attribute__((annotate(\"DFUNCTION,\" #__VA_ARGS__)))")

    clang.cindex.Config.set_library_path(clang_lib_dir)

def init_logging():
    logging.getLogger().setLevel(logging.INFO)
    logging.basicConfig(format='[%(levelname)s] %(message)s')

def extract_tokens(cursor) -> list:
    return [token.spelling for token in cursor.get_tokens()]

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

class DHTModule:
    name: str
    source_dir: str
    module_info_filepath: str
    export_api: str
    dht_output_dir: str
    registered_dclasses: list

    def __init__(self, source_dir, dht_output_dir):
        self.dht_output_dir = dht_output_dir
        self.source_dir = source_dir
        self.name = os.path.basename(self.source_dir)
        self.module_info_filepath = os.path.join(self.dht_output_dir, f"ModuleInfo.json")
        self.export_api = self.name.upper() + "_API"

        with open(self.module_info_filepath, "r") as f:
            module_info = json.load(f)
            self.registered_dclasses = module_info.get("DClasses", [])
            logging.debug("Registered DClasses for module '%s': %s", self.name, self.registered_dclasses)

module_meta = None

# Property meta info, annotated with DPROPERTY()
class DHTProperty:
    name: str
    type: str
    annotations: dict

    def __init__(self, name: str, type: str, annotations: dict):
        self.name = name
        self.type = type
        self.annotations = annotations

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
    api: str
    superclass: str
    annotations: dict
    generate_body_line: int
    properties: list
    functions: list


    def __init__(self):
        self.generate_body_line = 0

    def construct(self, class_cursor, annotations):
        self.name = class_cursor.spelling
        self.properties = []
        self.functions = []
        self.annotations = annotations

        self.construct_class_declaration(class_cursor)
        self.construct_members(class_cursor)

    def construct_members(self, class_cursor):
        for child_cursor in class_cursor.get_children():
            if child_cursor.kind == clang.cindex.CursorKind.FIELD_DECL:
                self.add_property(child_cursor)
            elif child_cursor.kind == clang.cindex.CursorKind.FUNCTION_DECL:
                self.add_function(child_cursor)

    def construct_class_declaration(self, class_cursor):
        class_tokens = [token.spelling for token in class_cursor.get_tokens()]
        declaration_token_end = class_tokens.index("{") if "{" in class_tokens else len(class_tokens)
        declaration_tokens = class_tokens[:declaration_token_end]

        # Extract export API information
        if module_meta.export_api in declaration_tokens:
            self.api = module_meta.export_api

        # Extract the first superclass
        if ":" in declaration_tokens:
            colon_index = declaration_tokens.index(":")
            if declaration_tokens[colon_index + 1] in ["public", "protected", "private"]:
                assert colon_index + 2 < len(declaration_tokens)
                self.superclass = declaration_tokens[colon_index + 2]
            else:
                assert colon_index + 1 < len(declaration_tokens)
                self.superclass = declaration_tokens[colon_index + 1]

    def add_property(self, property_cursor):
        if property_cursor.kind == clang.cindex.CursorKind.FIELD_DECL:
            annotations = extract_annotations(property_cursor)
            if annotations and "DPROPERTY" in annotations:
                property_meta = DHTProperty(property_cursor.spelling, property_cursor.type.spelling, annotations)
                self.properties.append(property_meta)

    def add_function(self, function_cursor):
        pass

    def generate_body_code(self, file, fid):
        if self.generate_body_line == 0:
            logging.warning("No generated body line found for class: %s", self.name)
            return

        file.write("#define " + fid + "_" + self.generate_body_line + "_GENERATED_BODY\n")


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
        tu = clang_index.parse(header_path, clang_args)

        if not tu:
            logging.error("Unable to load translation unit from %s", header_path)
            return None

        header_meta = DHTHeader(input_header)
        header_meta.construct(tu.cursor)
        return header_meta

class DHTCodeGenerator:
    def __init__(self):
        pass

    def generate(self, header_meta: DHTHeader) -> None:
        filename, _ = os.path.splitext(os.path.basename(header_meta.filepath))
        output_dir = module_meta.dht_output_dir
        if output_dir is None:
            raise ValueError("Output directory is not set.")
        os.makedirs(output_dir, exist_ok=True)
        self.generate_header_file(os.path.join(output_dir, f"{filename}.gen.h"), header_meta)
        self.generate_cpp_file(os.path.join(output_dir, f"{filename}.gen.cpp"), header_meta)

    # Generate the header file
    def generate_header_file(self, filepath, header_meta: DHTHeader) -> None:
        with open(filepath, 'w') as file:
            file.write("#pragma once\n")
            file.write("#undef CURRENT_FILE_ID\n")
            file.write("#define CURRENT_FILE_ID " + header_meta.fid + "\n")
            for class_meta in header_meta.classes:
                class_meta.generate_body_code(file, header_meta.fid)

    # Generate the cpp file
    def generate_cpp_file(self, filepath, header_meta: DHTHeader) -> None:
        with open(filepath, 'w') as file:
            file.write("#include \"" + header_meta.include_path + "\"\n")


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

    dht_code_generator.generate(header_meta)

