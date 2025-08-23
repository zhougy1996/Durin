import sys
import os

def write_empty_file(file_path):
    with open(file_path, 'w') as file:
        file.write("")

if __name__ == "__main__":
    input_headers = sys.argv[1]
    input_header_list = input_headers.split(";")
    print("Input Headers:", input_header_list)

    target_directory = sys.argv[2]
    os.makedirs(target_directory, exist_ok=True)
    print("Target Directory:", target_directory)

    for header in input_header_list:
        print("Processing header:", header)
        # You can add your processing logic here
        header_base = os.path.basename(header)
        gen_cpp_file_name = f"{header_base}.gen.cpp"
        gen_cpp_file_path = os.path.join(target_directory, gen_cpp_file_name)
        write_empty_file(gen_cpp_file_path)

        gen_h_file_name = f"{header_base}.gen.h"
        gen_h_file_path = os.path.join(target_directory, gen_h_file_name)
        write_empty_file(gen_h_file_path)

