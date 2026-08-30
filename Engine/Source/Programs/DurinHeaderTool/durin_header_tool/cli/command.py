import argparse
from pathlib import Path


def parse_worker_count(value: str) -> int:
    try:
        worker_count = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("workers must be an integer from 1 to 8") from error
    if not 1 <= worker_count <= 8:
        raise argparse.ArgumentTypeError("workers must be an integer from 1 to 8")
    return worker_count


def add_common_arguments(parser: argparse.ArgumentParser):
    from durin_header_tool.model.reflection_info import TOOL_VERSION

    parser.add_argument("-a","--arch", help="The target architecture (e.g., Win64, Linux, MacOS).", default="Win64", choices=["Win64", "Linux", "MacOS"])
    parser.add_argument("--runtime-variant", help="The workspace runtime variant.", default="DurinEditor")
    parser.add_argument(
        "--tool-fingerprint",
        help="Fingerprint of the DHT implementation used to invalidate generated caches.",
        default=TOOL_VERSION,
    )
    parser.add_argument(
        "--native-libclang-fingerprint",
        help="Precomputed libclang fingerprint; direct invocations hash the library when omitted.",
        default="",
    )
    parser.add_argument(
        "--workers",
        help="Maximum parser workers; parallelism is used only for sufficiently large header sets.",
        default=1,
        type=parse_worker_count,
    )
    parser.add_argument("-l", "--log", help="Set the logging level (DEBUG, INFO, WARNING, ERROR, CRITICAL).", default="INFO", required=False, choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"])
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Suppress informational output while preserving warnings and errors.",
    )
    parser.add_argument("--project-file", action="append", default=[], type=Path, help="A .dproject file that supplies module ownership and dependency context. May be repeated.")


def prepare_project_build(args):
    from durin_header_tool.generators.project_cmake_file_generator import generate_project_cmake_file
    from durin_header_tool.generators.module_cmake_file_generator import generate_all_module_cmake_files_for_project
    from durin_header_tool import config as configs

    project_name = configs.load_project_config_file(args.project).project_name
    generate_project_cmake_file(project_name)
    generate_all_module_cmake_files_for_project(project_name)


def generate_module_export_file(args):
    from durin_header_tool.generators.module_export_file_generator import generate_module_export_file as generate

    generate(args.module, args.workers)


def generate_reflection_files(args):
    from durin_header_tool.generators.module_reflection_files_generator import generate_reflection_files as generate

    generate(args.module, args.workers)


def setup_parser(parser: argparse.ArgumentParser):
    subparsers = parser.add_subparsers(dest="function", required=True)

    prepare_project_parser = subparsers.add_parser(
        "prepare_project_build",
        help="Prepare the build environment for the entire project.",
    )
    prepare_project_parser.add_argument(
        "-p",
        "--project",
        type=Path,
        help="The full path to the .dproject file to prepare.",
        required=True,
    )
    add_common_arguments(prepare_project_parser)
    prepare_project_parser.set_defaults(execute=prepare_project_build)

    module_export_parser = subparsers.add_parser(
        "generate_module_export_file",
        help="Generate the module export file containing export information extracted from the module's headers.",
    )
    module_export_parser.add_argument(
        "-m",
        "--module",
        help="The name of the module to generate export file for.",
        required=True,
    )
    add_common_arguments(module_export_parser)
    module_export_parser.set_defaults(execute=generate_module_export_file)

    reflection_parser = subparsers.add_parser(
        "generate_reflection_files",
        help="Run the header tool to generate necessary files for the reflection system.",
    )
    reflection_parser.add_argument(
        "-m",
        "--module",
        help="The name of the module to run the header tool for.",
        required=True,
    )
    add_common_arguments(reflection_parser)
    reflection_parser.set_defaults(execute=generate_reflection_files)
