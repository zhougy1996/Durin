# This file is used to test the functionality of the DurinHeaderTool.
from pathlib import Path
import sys

from cli.main import main
import utils
from models import ModuleManifest, load_module_manifest_file, save_module_manifest_file

test_cases = {
    "test_empty": ["main.py", "empty"],
    "test_generate_project_cmake_file": ["main.py", "generate_project_cmake_file", "-p", "Engine"],
    "test_generate_module_cmake_file": ["main.py", "generate_module_cmake_file", "-m", "Engine"],
    "test_generate_module_manifest_file": ["main.py", "generate_module_manifest_file", "-m", "Engine"],
    "test_generate_reflection_files": ["main.py", "generate_reflection_files", "-m", "Engine"],
}

sys.argv = test_cases["test_empty"]

if __name__ == "__main__":
    main()
    manifest = load_module_manifest_file("Engine")
    print(manifest)