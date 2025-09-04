import sys
import os
import logging

import clang.cindex
import json

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

def init_logging():
    logging.getLogger().setLevel(logging.DEBUG)
    logging.basicConfig(format='[%(levelname)s] %(message)s')

def find_dclasses(input_header) -> list[str]:
    # Find all D classes in the given header file

    found_dclasses = []

    index = clang.cindex.Index.create()
    translation_unit = index.parse(input_header, args=clang_args)

    cursors = list(translation_unit.cursor.get_children())
    i = 0
    while i < (len(cursors) - 1):
        cursor = cursors[i]
        if cursor.kind == clang.cindex.CursorKind.FUNCTION_DECL:
            added = False
            if cursor.spelling == "DCLASS":
                class_cursor = cursors[i + 1]
                if class_cursor.kind == clang.cindex.CursorKind.CLASS_DECL:
                    found_dclasses.append(class_cursor.spelling)
                    added = True
            if added:
                i += 1
        i += 1

    return found_dclasses

if __name__ == "__main__":
    # Example usage: python dht.py <input_headers> <target_directory> <module_source_dir>
    # Parsing command line arguments
    if len(sys.argv) != 4:
        print("Usage: python dht.py <input_headers> <target_directory> <module_source_dir>")
        sys.exit(1)

    input_headers = sys.argv[1]
    target_directory = sys.argv[2]
    module_source_dir = sys.argv[3]

    module_name = os.path.basename(module_source_dir)
    module_export_api = module_name.upper() + "_API"

    clang_args.append(f"-D{module_export_api}=")

    init_logging()
    init_clang()

    found_dclasses = []

    input_headers = input_headers.split(";")
    for input_header in input_headers:
        found_dclasses.extend(find_dclasses(input_header))

    output_file = os.path.join(target_directory, "ModuleInfo.json")

    with open(output_file, "w") as f:
        json.dump({"DClasses": found_dclasses}, f, indent=4)
