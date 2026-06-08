import argparse
import logging
import configs
from .command import (
    CommandManager,
    EmptyCommand,
    GenerateModuleExportFileCommand,
    GenerateReflectionFilesCommand,
    PrepareProjectBuildCommand,
)

def init_logging(log_level_str: str):
    log_level = getattr(logging, log_level_str.upper(), logging.INFO)
    logging.basicConfig(level=log_level, format="[%(levelname)s] %(message)s")

def main():
    command_manager = CommandManager()
    command_manager.register_command(EmptyCommand())
    command_manager.register_command(PrepareProjectBuildCommand())
    command_manager.register_command(GenerateModuleExportFileCommand())
    command_manager.register_command(GenerateReflectionFilesCommand())

    parser = argparse.ArgumentParser(description="Durin Header Tool")
    command_manager.setup_parser(parser)
    args = parser.parse_args()

    configs.ARCH = args.arch
    configs.PROFILE_NAME = args.profile
    init_logging(args.log)
    configs.init_configs()
    command_manager.execute_command(args.function, args)

if __name__ == "__main__":
    main()

