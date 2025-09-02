import sys
import os

import clang.cindex

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
    clang.cindex.Config.set_library_path(clang_lib_dir)
    print("Clang Library Directory:", clang_lib_dir)


def write_empty_file(file_path):
    with open(file_path, 'w') as file:
        file.write("")

def write_empty_header_file(file_path):
    with open(file_path, 'w') as file:
        file.write("#pragma once\n")

def get_file_name_without_extension(file_path):
    base_name = os.path.basename(file_path)
    name, _ = os.path.splitext(base_name)
    return name

def parse_annotation(node):
    tokens = node.get_tokens()
    if not tokens:
        return
    token_strings = [token.spelling for token in tokens]
    annotation_name = token_strings[0] if token_strings else ""

    print("DCLASS Macro token strings:", token_strings)

def extract_class_meta(class_node) -> dict:
    class_meta = {
        "name": class_node.spelling,
        "methods": [],
        "members": []
    }
    for child in class_node.get_children():
        if child.kind == clang.cindex.CursorKind.CXX_METHOD:
            class_meta["methods"].append(child.spelling)
        elif child.kind == clang.cindex.CursorKind.FIELD_DECL:
            class_meta["members"].append(child.spelling)
    return class_meta

def parse_dclass(annotation_node, class_node) -> bool:
    is_valid_node = False
    if class_node.kind == clang.cindex.CursorKind.CLASS_DECL:
        print("Found annotated class:", class_node.spelling)
        parse_annotation(annotation_node)
        is_valid_node = True
    return is_valid_node

def parse_header(header_path):
    index = clang.cindex.Index.create()
    print("Parsing header:", header_path)
    tu = index.parse(header_path, clang_args)

    childrens = list(tu.cursor.get_children())

    # Find and parse all annotated classes
    i = 0
    while i < (len(childrens) - 1):
        node = childrens[i]
        if node.kind == clang.cindex.CursorKind.FUNCTION_DECL and node.spelling == "DCLASS":
            found_valid_node = parse_dclass(node, childrens[i+1])
            if found_valid_node:
                i += 1
        i += 1
    
    # check the last node
    if childrens and childrens[-1].kind == clang.cindex.CursorKind.FUNCTION_DECL and childrens[-1].spelling == "DCLASS":
        print("DCLASS macro must be followed by a class declaration and cannot be the last child")

    return tu

if __name__ == "__main__":
    input_headers = sys.argv[1]
    input_header_list = input_headers.split(";")

    target_directory = sys.argv[2]
    os.makedirs(target_directory, exist_ok=True)

    init_clang()

    for header in input_header_list:
        translation_unit = parse_header(header)

        header_base = get_file_name_without_extension(header)
        gen_cpp_file_name = f"{header_base}.gen.cpp"
        gen_cpp_file_path = os.path.join(target_directory, gen_cpp_file_name)
        write_empty_file(gen_cpp_file_path)

        gen_h_file_name = f"{header_base}.gen.h"
        gen_h_file_path = os.path.join(target_directory, gen_h_file_name)
        write_empty_header_file(gen_h_file_path)

