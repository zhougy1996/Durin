import sys

import cli.main as main

test_cases = {
    "test_generate_project_cmake_file": ["main.py", "generate_project_cmake_file", "-p", "Engine"],
    "test_generate_module_cmake_file": ["main.py", "generate_module_cmake_file", "-m", "Engine"],
    "test_generate_module_manifest_file": ["main.py", "generate_module_manifest_file", "-m", "Engine"],
    "test_generate_reflection_files": ["main.py", "generate_reflection_files", "-m", "Engine"],
}

sys.argv = test_cases["test_generate_project_cmake_file"]

if __name__ == "__main__":
    main.main()