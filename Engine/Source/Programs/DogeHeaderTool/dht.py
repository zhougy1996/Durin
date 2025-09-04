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

class DHTModule:
    name: str
    source_dir: str
    module_info_filepath: str
    export_api: str
    dht_output_dir: str

    def __init__(self):
        self.name = ""
        self.source_dir = ""
        self.module_info_filepath = ""
        self.export_api = ""
        self.dht_output_dir = ""

module_meta = DHTModule()

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
    clang.cindex.Config.set_library_path(clang_lib_dir)

def init_logging():
    logging.getLogger().setLevel(logging.INFO)
    logging.basicConfig(format='[%(levelname)s] %(message)s')

def parse_annotation(annotation_cursor) -> dict:
    tokens = [token.spelling for token in annotation_cursor.get_tokens()]
    tokens = tokens[2:-1]  # Remove the macro name and parentheses
    tokens = [token.strip() for token in ' '.join(tokens).split(',') if token.strip()] # split by comma
    annotations = {}
    for token in tokens:
        if '=' in token:
            key, value = token.split('=', 1)
            annotations[key.strip()] = value.strip().strip('"')
        else:
            annotations[token] = True
    return annotations

# Property meta info, annotated with DPROPERTY()
class DHTProperty:
    name: str
    type: str

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
    properties: list
    functions: list

    def __init__(self):
        pass

    def construct(self, class_cursor, annotation_cursor):
        self.name = class_cursor.spelling
        self.properties = []
        self.functions = []

        annotations = parse_annotation(annotation_cursor)
        logging.debug("Found class: %s with annotations: %s", self.name, annotations)

        self.construct_class_declaration(class_cursor)
        self.construct_members(class_cursor)

    def construct_members(self, class_cursor):
        members = list(class_cursor.get_children())
        i = 0
        while i < (len(members) - 1):
            cursor = members[i]
            if cursor.kind == clang.cindex.CursorKind.FUNCTION_DECL:
                member_added = False
                if cursor.spelling == "DPROPERTY":
                    member_added = self.add_property(members[i+1], cursor)
                if member_added:
                    i += 1
            i += 1

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

    def add_property(self, property_cursor, annotation_cursor) -> bool:
        if property_cursor.kind == clang.cindex.CursorKind.FIELD_DECL:
            property_meta = DHTProperty(property_cursor.spelling, property_cursor.type.spelling)
            self.properties.append(property_meta)
            return True
        return False

    def add_function(self, function_cursor, annotation_cursor):
        pass


# Header meta info, contains multiple classes
class DHTHeader:
    name: str
    filepath: str
    relative_path: str
    classes: list

    def __init__(self):
        pass

    def construct(self, header_path, header_cursor):
        header_name, _ = os.path.splitext(os.path.basename(header_path))

        self.name = header_name
        self.filepath = header_path
        self.classes = []

        self.construct_children(header_cursor)
        return True
    
    def construct_children(self, header_cursor):
        cursors = list(header_cursor.get_children())
        i = 0
        while i < (len(cursors) - 1):
            cursor = cursors[i]
            if cursor.kind == clang.cindex.CursorKind.FUNCTION_DECL:
                added = False
                if cursor.spelling == "DCLASS":
                    added = self.add_class(cursors[i+1], cursor)
                if added:
                    i += 1
            i += 1

    def add_class(self, class_cursor, annotation_cursor) -> bool:
        if class_cursor.kind == clang.cindex.CursorKind.CLASS_DECL:
            class_meta = DHTClass()
            class_meta.construct(class_cursor, annotation_cursor)
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

        header_meta = DHTHeader()
        header_meta.construct(header_path, tu.cursor)
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

    # Generate the cpp file
    def generate_cpp_file(self, filepath, header_meta: DHTHeader) -> None:
        with open(filepath, 'w') as file:
            file.write("#include \"" + header_meta.relative_path + "\"\n")

dht_parser = DHTParser()
dht_code_generator = DHTCodeGenerator()

def get_relative_path(input_header, module_source_dir):
    if not input_header.startswith(module_source_dir):
        logging.error("Input header: %s", input_header)
        logging.error("Module source dir: %s", module_source_dir)
        sys.exit(1)

    relative_path = os.path.relpath(input_header, module_source_dir).replace("\\", "/")
    if relative_path.startswith("Public/"):
        relative_path = relative_path[len("Public/"):]
    elif relative_path.startswith("Private/"):
        relative_path = relative_path[len("Private/"):]
    return relative_path

if __name__ == "__main__":
    # Example usage: python dht.py input_header.h target_directory module_source_dir
    # Parsing command line arguments
    if len(sys.argv) != 4:
        print("Usage: python dht.py <input_header> <target_directory> <module_source_dir>")
        sys.exit(1)

    input_header = sys.argv[1]

    # Initialize module metadata
    module_meta.dht_output_dir = sys.argv[2]
    module_meta.source_dir = sys.argv[3]
    module_meta.name = os.path.basename(module_meta.source_dir)
    module_meta.module_info_filepath = os.path.join(module_meta.dht_output_dir, f"ModuleInfo.json")
    module_meta.export_api = module_meta.name.upper() + "_API"

    init_logging()
    init_clang()

    # relative path from module source dir to input header
    relative_path = get_relative_path(input_header, module_meta.source_dir)

    header_meta = dht_parser.parse_header(input_header)
    if header_meta is None:
        logging.error("Failed to parse header.")
        sys.exit(1)

    # Set the relative path for the header meta, used for includes in generated files
    header_meta.relative_path = relative_path
    dht_code_generator.generate(header_meta)

