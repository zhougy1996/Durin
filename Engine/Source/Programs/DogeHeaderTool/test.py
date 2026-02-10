import sys

import main

# Command lines to test:
# generate_module_cmake_data
# sys.argv = ["main.py", "generate_module_cmake_data", "-m", "Core"]

# generate_module_manifest_file
# sys.argv = ["main.py", "generate_module_manifest_file", "-m", "Core"]
    
# generate_reflection_files
sys.argv = ["main.py", "generate_reflection_files", "-m", "Core", "-a", "Win64", "-b", "Editor"]

if __name__ == "__main__":
    main.main()