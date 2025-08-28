import sys
import os

import clang.cindex

current_file_path = os.path.abspath(__file__)
current_dir = os.path.dirname(current_file_path)
doge_source_dir = os.path.abspath(os.path.join(current_dir, "../.."))
doge_root_dir = os.path.abspath(os.path.join(doge_source_dir, "../.."))
clang_lib_dir = os.path.join(doge_source_dir, "ThirdParty/clang/bin")


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

def parse_header(header_path):
    index = clang.cindex.Index.create()
    print("Parsing header:", header_path)
    translation_unit = index.parse(header_path, args=[
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
    ])
    for node in translation_unit.cursor.get_children():
        print("Node:", node.spelling, "Type:", node.kind)
    return translation_unit

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

