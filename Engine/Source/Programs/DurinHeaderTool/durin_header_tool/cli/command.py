import logging
import sys
import argparse
import re
from pathlib import Path


_BUILD_IDENTIFIER_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


def parse_build_identifier(value: str) -> str:
    if value and not _BUILD_IDENTIFIER_PATTERN.fullmatch(value):
        raise argparse.ArgumentTypeError(
            "build identifier must start with an alphanumeric character and contain only "
            "alphanumeric characters, '.', '_' or '-'"
        )
    return value


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
        "--build-identifier",
        help="Optional identifier used to isolate generated build metadata.",
        default="",
        type=parse_build_identifier,
    )
    parser.add_argument(
        "--tool-fingerprint",
        help="Fingerprint of the DHT implementation used to invalidate generated caches.",
        default=TOOL_VERSION,
    )
    parser.add_argument(
        "--workers",
        help="Maximum parser workers; parallelism is used only for sufficiently large header sets.",
        default=1,
        type=parse_worker_count,
    )
    parser.add_argument("-l", "--log", help="Set the logging level (DEBUG, INFO, WARNING, ERROR, CRITICAL).", default="INFO", required=False, choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"])
    parser.add_argument("--project-file", action="append", default=[], type=Path, help="A .dproject file that supplies module ownership and dependency context. May be repeated.")

class Command:
    def __init__(self, name: str, description: str):
        self.name = name
        self.description = description

    # Add command-specific arguments to the parser
    def add_arguments(self, parser: argparse.ArgumentParser):
        pass

    def execute(self, args):
        pass

    def run(self, args):
        self.execute(args)

class CommandManager:
    commands: dict[str, Command]

    def __init__(self):
        self.commands = {}

    def register_command(self, command: Command):
        self.commands[command.name] = command

    def get_command(self, name: str):
        return self.commands.get(name)

    def setup_parser(self, parser: argparse.ArgumentParser):
        subparsers = parser.add_subparsers(dest="function", required=True)
        for command in self.commands.values():
            sub = subparsers.add_parser(command.name, help=command.description)
            command.add_arguments(sub)

    def execute_command(self, name: str, args):
        command = self.get_command(name)
        if command:
            command.run(args)
        else:
            logging.error(f"Unknown command: {name}")
            sys.exit(1)


class EmptyCommand(Command):
    def __init__(self):
        super().__init__("empty", "An empty command that does nothing.")

    def add_arguments(self, parser: argparse.ArgumentParser):
        add_common_arguments(parser)

    def execute(self, args):
        pass

class PrepareProjectBuildCommand(Command):
    def __init__(self):
        super().__init__("prepare_project_build", "Prepare the build environment for the entire project.")

    def add_arguments(self, parser: argparse.ArgumentParser):
        parser.add_argument("-p","--project", type=Path, help="The full path to the .dproject file to prepare.", required=True)
        add_common_arguments(parser)

    def execute(self, args):
        from durin_header_tool.generators.project_cmake_file_generator import generate_project_cmake_file
        from durin_header_tool.generators.module_cmake_file_generator import generate_all_module_cmake_files_for_project
        from durin_header_tool import config as configs
        project_name = configs.load_project_config_file(args.project).project_name
        generate_project_cmake_file(project_name)
        generate_all_module_cmake_files_for_project(project_name)

class GenerateModuleExportFileCommand(Command):
    def __init__(self):
        super().__init__("generate_module_export_file", "Generate the module export file containing export information extracted from the module's headers.")

    def add_arguments(self, parser: argparse.ArgumentParser):
        parser.add_argument("-m","--module", help="The name of the module to generate export file for.", required=True)
        add_common_arguments(parser)

    def execute(self, args):
        from durin_header_tool.generators.module_export_file_generator import generate_module_export_file
        generate_module_export_file(args.module, args.workers)

class GenerateReflectionFilesCommand(Command):
    def __init__(self):
        super().__init__("generate_reflection_files", "Run the header tool to generate necessary files for the reflection system.")

    def add_arguments(self, parser: argparse.ArgumentParser):
        parser.add_argument("-m","--module", help="The name of the module to run the header tool for.", required=True)
        add_common_arguments(parser)

    def execute(self, args):
        from durin_header_tool.generators.module_reflection_files_generator import generate_reflection_files
        generate_reflection_files(args.module, args.workers)
