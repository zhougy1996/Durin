import logging
import sys
import argparse

def add_common_arguments(parser: argparse.ArgumentParser):
    parser.add_argument("-a","--arch", help="The target architecture (e.g., Win64, Linux, MacOS).", default="Win64", choices=["Win64", "Linux", "MacOS"])
    parser.add_argument("-b","--build_mode", help="The build mode.", default="Editor", choices=["Game", "Editor"])
    parser.add_argument("-l", "--log", help="Set the logging level (DEBUG, INFO, WARNING, ERROR, CRITICAL).", default="INFO", required=False, choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"])

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
        parser.add_argument("-p","--project", help="The name of the project to prepare the build environment and generate necessary files for.", required=True)
        add_common_arguments(parser)

    def execute(self, args):
        import configs
        from generators.project_cmake_file_generator import generate_project_cmake_file
        from generators.module_cmake_file_generator import generate_module_cmake_file
        project_config = configs.get_project_config(args.project)
        generate_project_cmake_file(args.project)
        for module_name in project_config.modules.keys():
            generate_module_cmake_file(module_name)

class GenerateModuleExportFileCommand(Command):
    def __init__(self):
        super().__init__("generate_module_export_file", "Generate the module export file containing export information extracted from the module's headers.")

    def add_arguments(self, parser: argparse.ArgumentParser):
        parser.add_argument("-m","--module", help="The name of the module to generate export file for.", required=True)
        add_common_arguments(parser)

    def execute(self, args):
        from generators.module_export_file_generator import generate_module_export_file
        generate_module_export_file(args.module)

class GenerateReflectionFilesCommand(Command):
    def __init__(self):
        super().__init__("generate_reflection_files", "Run the header tool to generate necessary files for the reflection system.")

    def add_arguments(self, parser: argparse.ArgumentParser):
        parser.add_argument("-m","--module", help="The name of the module to run the header tool for.", required=True)
        add_common_arguments(parser)

    def execute(self, args):
        from generators.module_reflection_files_generator import generate_reflection_files
        generate_reflection_files(args.module)