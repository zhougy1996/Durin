import sys
import os

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

if __name__ == "__main__":
    input_headers = sys.argv[1]
    input_header_list = input_headers.split(";")

    target_directory = sys.argv[2]
    os.makedirs(target_directory, exist_ok=True)

    for header in input_header_list:
        header_base = get_file_name_without_extension(header)
        gen_cpp_file_name = f"{header_base}.gen.cpp"
        gen_cpp_file_path = os.path.join(target_directory, gen_cpp_file_name)
        write_empty_file(gen_cpp_file_path)

        gen_h_file_name = f"{header_base}.gen.h"
        gen_h_file_path = os.path.join(target_directory, gen_h_file_name)
        write_empty_header_file(gen_h_file_path)

