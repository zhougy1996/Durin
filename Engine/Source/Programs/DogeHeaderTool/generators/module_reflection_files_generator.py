import os

import configs
import utils

# Empty Implementation for generating reflection files for a module. This is used to create empty reflection files for modules to make sure the build system can find them, and to test the integration of the reflection file generation into the build process. The actual implementation of parsing headers and generating reflection files will be added later.
def _generate_empty_reflection_files(module_name):
    module_config = configs.get_module_config(module_name)

    empty_cpp_content = "// This is an empty generated reflection source file.\n"
    empty_h_content = "// This is an empty generated reflection header file.\n"

    for header in module_config.reflect_headers:
        header_filename = os.path.splitext(os.path.basename(header))[0]
        output_dir = utils.get_module_dht_output_dir(module_name)
        reflection_header_file = output_dir / f"{header_filename}.gen.h"
        reflection_source_file = output_dir / f"{header_filename}.gen.cpp"
        utils.generate_file(reflection_header_file, empty_h_content)
        utils.generate_file(reflection_source_file, empty_cpp_content)

def generate_reflection_files(module_name):
    _generate_empty_reflection_files(module_name)