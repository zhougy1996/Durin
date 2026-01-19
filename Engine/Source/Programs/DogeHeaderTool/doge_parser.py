import sys
import os
import logging
from enum import Enum

import clang.cindex
from clang.cindex import TokenKind

from doge_project import DogeProjectConfig, DogeModuleConfig, load_project_config, load_module_config
import doge_globals as g
from doge_exports import ExportedClass, HeaderExports

default_clang_args = [
    "-x",
    "c++",
    "-std=c++20",
    "-D_DHT_PARSER",
    "-DNDEBUG",
    "-D_MSC_VER=1930",
    "-w",
    "-MG",
    "-M",
    "-ferror-limit=0"
]

intrinsic_core_objects = [
    "DObject",
    "DClass",
    "DStruct",
    "DEnum"
]

property_param_dict = {
    "int8": "FInt8PropertyParams",
    "int16": "FInt16PropertyParams",
    "int32": "FInt32PropertyParams",
    "int64": "FInt64PropertyParams",
    "uint8": "FUInt8PropertyParams",
    "uint16": "FUInt16PropertyParams",
    "uint32": "FUInt32PropertyParams",
    "uint64": "FUInt64PropertyParams",

    "float": "FFloatPropertyParams",
    "double": "FDoublePropertyParams",
    "bool": "FBoolPropertyParams",
    "FString": "FStringPropertyParams",
}

# Forward declaration header for clang parser
_virtual_fwd_header_path = "dht_virtual_fwd.h"

# Content of the forward declaration header
_virtual_fwd_header_content = """
class FObjectInitializer;
class DObject;
class DClass;
class DStruct;
class DEnum;
"""

_module_info: DogeModuleConfig = None

def init(module_info: DogeModuleConfig):
    global _module_info
    _module_info = module_info

    # clang_args = default_clang_args.copy()

    # clang_args.append(f"-D{module_info.api_macro}=")
    # # DHT_GENERATED_BODY() will be identified as a function declaration
    # clang_args.append("-DGENERATED_BODY(...)=void DHT_GENERATED_BODY();")
    # clang_args.append("-DDHT_DEBUG_BEGIN(...)=static void DHT_DEBUG_BEGIN();")
    # clang_args.append("-DDHT_DEBUG_END(...)=static void DHT_DEBUG_END();")
    # # DHT_CLASS() will be identified as a function declaration
    # clang_args.append("-DDCLASS(...)=__attribute__((annotate(\"DCLASS,\" #__VA_ARGS__))) void DHT_CLASS();") 
    # clang_args.append("-DDPROPERTY(...)=__attribute__((annotate(\"DPROPERTY,\" #__VA_ARGS__)))")
    # clang_args.append("-DDFUNCTION(...)=__attribute__((annotate(\"DFUNCTION,\" #__VA_ARGS__)))")
    # clang_args.append("-DDFUNCTION(...)=__attribute__((annotate(\"DFUNCTION,\" #__VA_ARGS__)))")
    # clang_args.append(f'-include{_virtual_fwd_header_path}')


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

def append_include(builder, include_file):
    builder.append(f'#include "{include_file}"\n')

macro_newline = " \\\n"

def append_macro_line(builder, content, indent_level=0, is_last_line=False):
    indent = "\t" * indent_level
    builder.append(indent + content + ("\n" if is_last_line else macro_newline))

def append_comment_segmentation(builder, comment):
    comment_size = len(comment)
    star_num = 0 if comment_size > 50 else 50 - comment_size
    builder.append(f"// ********* {comment} {star_num * '*'} \n")

def is_object_initializer_constructor(constructor_cursor) -> bool:
    if constructor_cursor.kind != clang.cindex.CursorKind.CONSTRUCTOR:
        return False

    params = list(constructor_cursor.get_children())
    if len(params) != 1:
        return False

    first_param = params[0]
    if first_param.kind != clang.cindex.CursorKind.PARM_DECL:
        return False

    param_type = first_param.type.spelling
    if param_type.startswith("const FObjectInitializer &") or param_type.startswith("FObjectInitializer &"):
        return True

    return False

def is_default_constructor(constructor_cursor) -> bool:
    if constructor_cursor.kind != clang.cindex.CursorKind.CONSTRUCTOR:
        return False

    params = list(constructor_cursor.get_children())
    if len(params) != 0:
        return False

    return True

class DHTConstructorType(Enum):
    DEFAULT = 1
    OBJECT_INITIALIZER = 2

# Property meta info, annotated with DPROPERTY()
class DHTProperty:
    name: str
    type: str
    annotations: dict
    cursor: clang.cindex.Cursor
    tokens: list
    property_param: str

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
        self.init_property_param()

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
    
    def init_property_param(self):
        if self.type in property_param_dict:
            self.property_param = property_param_dict[self.type]
        else:
            self.property_param = "FUnknownPropertyParams"
            logging.warning("Property '%s' has unknown type: %s", self.name, self.type)

    def append_param_declaration(self, builder) -> list:
        builder.append(f"\tstatic const DogeCodeGen::{self.property_param} NewProp_{self.name};\n")
        return [f"NewProp_{self.name}"]

    def append_param_definition(self, builder, class_meta) -> None:
        builder.append(f"const DogeCodeGen::{self.property_param} {class_meta.construct_statics}::NewProp_{self.name} = {{ \"{self.name}\", EPropertyFlags::None, 1, STRUCT_OFFSET_UINT16({class_meta.name}, {self.name})}};\n")



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
    has_default_constructor: bool = False
    has_object_initializer_constructor: bool = False
    has_destructor: bool = False
    constructor_type: DHTConstructorType

    construct_func_name: str
    construct_noregister_func_name: str
    construct_statics: str

    def __init__(self):
        self.generate_body_line = 0

    def construct(self, class_cursor, annotations):
        # Initialize basic info
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
        self.constructor_type = DHTConstructorType.OBJECT_INITIALIZER
        self.has_default_constructor = False
        self.has_object_initializer_constructor = False
        self.has_destructor = False

        self.construct_class_declaration()
        self.construct_members()

    def construct_members(self):
        for child_cursor in self.cursor.get_children():
            match child_cursor.kind:
                case clang.cindex.CursorKind.FIELD_DECL:
                    self.add_property(child_cursor)
                    continue
                case clang.cindex.CursorKind.CONSTRUCTOR:
                    if is_default_constructor(child_cursor):
                        self.has_default_constructor = True
                        self.constructor_type = DHTConstructorType.DEFAULT
                    elif is_object_initializer_constructor(child_cursor):
                        self.has_object_initializer_constructor = True
                    continue
                case clang.cindex.CursorKind.DESTRUCTOR:
                    self.has_destructor = True
                    continue
                case clang.cindex.CursorKind.CXX_METHOD:
                    if child_cursor.spelling == "DHT_GENERATED_BODY":
                        self.generate_body_line = child_cursor.location.line
                    elif child_cursor.spelling == "DHT_DEBUG_BEGIN":
                        logging.getLogger().setLevel(logging.DEBUG)
                    elif child_cursor.spelling == "DHT_DEBUG_END":
                        logging.getLogger().setLevel(logging.INFO)
                    else:
                        self.add_function(child_cursor)
                    continue
                case _:
                    continue

    def construct_class_declaration(self):
        class_tokens = [token.spelling for token in self.clang_tokens]
        declaration_token_end = class_tokens.index("{") if "{" in class_tokens else len(class_tokens)
        declaration_tokens = class_tokens[:declaration_token_end]

        # Extract export API information
        self.api = g.module_meta.api_macro

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

    @staticmethod
    def add_function(function_cursor):
        if function_cursor.kind == clang.cindex.CursorKind.CXX_METHOD:
            annotations = extract_annotations(function_cursor)
            if annotations and "DFUNCTION" in annotations:
                pass
                # tokens_without_macro = self.strip_macro_paren_prefix(tokens)

def collect_header_exports(header_file: str) -> HeaderExports:
    exports = HeaderExports(header_file) #TODO should be relative path

    clang_args = default_clang_args.copy()
    clang_args.append(f'-DDHT_EXPORTS_PARSER=')
    clang_args.append('-o /Logs/DHT/DHTExports.log')
    
    if os.path.isfile(header_file):
        index = clang.cindex.Index.create()
        tu = index.parse(header_file, args=clang_args)
        cursors = list(tu.cursor.get_children())
        i = 0
        while i < (len(cursors) - 1):
            cursor = cursors[i]
            if cursor.kind == clang.cindex.CursorKind.FUNCTION_DECL:
                added = False
                if cursor.spelling == "DCLASS":
                    class_cursor = cursors[i + 1]
                    if class_cursor.kind == clang.cindex.CursorKind.CLASS_DECL:
                        class_symbol = ExportedClass(class_cursor.spelling)
                        exports.classes[class_symbol.name] = class_symbol
                        added = True
                if added:
                    i += 1
            i += 1

    return exports

