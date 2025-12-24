
import os
import argparse
import dproject
import logging

import header_tool

# Add subparser and execution function for getting module directories
def add_subparser_get_module_dirs(parser) -> None:
    sub = parser.add_parser("get_module_dirs", help="Get the list of module directories for CMake.")
    sub.add_argument("-p", "--project_file", help="Specify the Doge project file (.dproject).", required=True)

def exec_get_module_dirs(args) -> None:
    logging.basicConfig(level=logging.DEBUG, format='%(asctime)s - %(levelname)s - %(message)s')
    module_dirs = dproject.get_module_dirs(args.project_file)
    print(";".join(module_dirs))


# Add subparser and execution function for generating reflection files
def add_subparser_run_header_tool(parser) -> None:
    sub = parser.add_parser("run_header_tool", help="Generate necessary files for the reflection system.")
    sub.add_argument("-p", "--project_file", help="Specify the Doge project file (.dproject).", required=True)
    sub.add_argument("-m", "--module", help="Specify the module name to process.", required=True)
    sub.add_argument("-a", "--arch", help="Specify the target architecture (e.g., Win64, Linux).", required=False, default="Win64", choices=["Win64", "Linux"])
    sub.add_argument("-b", "--build_mode", help="Specify the build mode.", required=False, default="Editor", choices=["Game", "Editor"])

def exec_run_header_tool(args) -> None:
    logging.basicConfig(level=logging.DEBUG, format='%(asctime)s - %(levelname)s - %(message)s')
    header_tool.setup_environment(args.arch, args.build_mode)
    header_tool.run(args.project_file, args.module)
    
def process_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="A tool for the build system of Doge Engine.")
    subparsers = parser.add_subparsers(dest="function", required=True)

    # Add subparsers for different functionalities
    add_subparser_get_module_dirs(subparsers)
    add_subparser_run_header_tool(subparsers)

    return parser.parse_args()

if __name__ == "__main__":
    args = process_arguments()

    if args.function == "get_module_dirs":
        exec_get_module_dirs(args)
    
    elif args.function == "run_header_tool":
        exec_run_header_tool(args)
