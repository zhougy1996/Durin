
import os
import argparse
import dproject

def process_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="A tool for the build system of Doge Engine.")
    subparsers = parser.add_subparsers(dest="function", required=True)

    parser_get_module_dirs = subparsers.add_parser("get_module_dirs", help="CMake related operations.")
    parser_get_module_dirs.add_argument("-p", "--project_file", help="Specify the Doge project file (.dproject) to load.", required=True)
    
    args = parser.parse_args()
    return args

def get_module_dirs(dproj_filepath: str) -> list[str]:
    dproj = dproject.load_dproject_config(dproj_filepath)
    # Print the list of module directories
    module_dirs = []
    for module_file in dproj.modules.values():
        module_dirs.append(os.path.dirname(module_file))
    return module_dirs

if __name__ == "__main__":
    args = process_arguments()
    if args.function == "get_module_dirs":
        module_dirs = get_module_dirs(args.project_file)
        print(";".join(module_dirs))

