import logging
import sys
import argparse

class Command:
    def __init__(self, name: str, description: str):
        self.name = name
        self.description = description

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

class GenerateModuleCMakeFileCommand(Command):
    def __init__(self):
        super().__init__("generate_module_cmake_data", "Generate CMake data for a module.")

    def add_arguments(self, parser: argparse.ArgumentParser):
        parser.add_argument("-m","--module", help="The name of the module to generate CMake data for.")

    def execute(self, args):
        print(f"Generating CMake data for module: {args.module}")