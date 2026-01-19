
import os
import sys
import argparse
import logging

import doge_project as dproject
import doge_globals as g
import doge_header_tool as header_tool

# Utility functions
def init_logging(level: str) -> None:
    os.makedirs(g.DOGE_ENGINE_LOG_DIR, exist_ok=True)
    numeric_level = getattr(logging, level.upper(), None)
    if not isinstance(numeric_level, int):
        raise ValueError(f'Invalid log level: {level}')
    logging.basicConfig(level=numeric_level, format='%(asctime)s - %(levelname)s - %(message)s')

# Generate module cmake file
def add_subparser_generate_module_cmake_file(subparsers, parent_parser) -> None:
    sub = subparsers.add_parser("generate_module_cmake_file", parents=[parent_parser], help="Generate module CMake file.")
    sub.add_argument("-m", "--module", help="Specify the module name to process.", required=True)
    sub.add_argument("-o", "--output", help="Specify the output CMake file path.", required=True)

def exec_generate_module_cmake_file(args) -> None:
    dproject.generate_module_cmake_file(g.target_project_cfg, args.module, args.output)

# Get module directories, used by CMake to add sub directories
def add_subparser_get_module_dirs(subparsers, parent_parser) -> None:
    subparsers.add_parser("get_module_dirs", parents=[parent_parser], help="Get the list of module directories for CMake.")

def exec_get_module_dirs(args) -> None:
    module_dirs = dproject.get_module_dirs(g.target_project_cfg)
    print(";".join(module_dirs))

# Generate module manifest file
def add_subparser_generate_module_manifest_file(subparsers, parent_parser) -> None:
    sub = subparsers.add_parser("generate_module_manifest_file", parents=[parent_parser], help="Generate module manifest file.")
    sub.add_argument("-m", "--module", help="Specify the module name to process.", required=True)

def exec_generate_module_manifest_file(args) -> None:
    header_tool.generate_module_manifest_file(g.target_project_cfg, args.module)

# Run header tool
def add_subparser_run_header_tool(subparsers, parent_parser) -> None:
    sub = subparsers.add_parser("run_header_tool", parents=[parent_parser], help="Generate necessary files for the reflection system.")
    sub.add_argument("-m", "--module", help="Specify the module name to process.", required=True)
    sub.add_argument("-a", "--arch", help="Specify the target architecture (e.g., Win64, Linux).", required=False, default="Win64", choices=["Win64", "Linux"])
    sub.add_argument("-b", "--build_mode", help="Specify the build mode.", required=False, default="Editor", choices=["Game", "Editor"])

def exec_run_header_tool(args) -> None:
    header_tool.setup_environment(args.arch, args.build_mode)
    header_tool.generate_reflection_files(args.project_file, args.module)

# Setup Parser
def setup_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="A tool for the build system of Doge Engine.")
    subparsers = parser.add_subparsers(dest="function", required=True)

    # Setup common arguments
    common_parser = argparse.ArgumentParser(add_help=False)
    common_parser.add_argument("-p", "--project_file", help="Specify the Doge project file (.dproject), default is the engine project file.", required=True)
    common_parser.add_argument("-l", "--log", help="Set the logging level (DEBUG, INFO, WARNING, ERROR, CRITICAL).", default="DEBUG", required=False, choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"])

    add_subparser_get_module_dirs(subparsers, common_parser)
    add_subparser_generate_module_cmake_file(subparsers, common_parser)
    add_subparser_generate_module_manifest_file(subparsers, common_parser)
    add_subparser_run_header_tool(subparsers, common_parser)

    return parser

if __name__ == "__main__":
    parser = setup_parser()
    args = parser.parse_args()

    init_logging(args.log)
    try:
        g.target_project_cfg = dproject.load_project_config(args.project_file)
    except Exception as e:
        logging.error(f"Failed to load project file {args.project_file}: {e}")
        sys.exit(1)

    g.projects[g.target_project_cfg.name] = g.target_project_cfg

    if args.function == "get_module_dirs":
        exec_get_module_dirs(args)

    elif args.function == "generate_module_cmake_file":
        exec_generate_module_cmake_file(args)
    
    elif args.function == "generate_module_manifest_file":
        exec_generate_module_manifest_file(args)
    
    elif args.function == "run_header_tool":
        exec_run_header_tool(args)
