import argparse
import logging
from durin_header_tool import config as configs
from durin_header_tool import io as utils
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
    configs.BUILD_IDENTIFIER = args.build_identifier
    init_logging(args.log)
    mutating_commands = {
        "prepare_project_build",
        "generate_module_export_file",
        "generate_reflection_files",
    }

    def execute() -> None:
        project_files = list(args.project_file)
        if args.function == "prepare_project_build":
            project_files.append(args.project)
        configs.init_configs(project_files)
        command_manager.execute_command(args.function, args)

    if args.function in mutating_commands:
        lock_path = utils.get_dht_output_lock_file_path()
        with utils.acquire_output_lock(lock_path, args.function):
            execute()
    else:
        execute()

if __name__ == "__main__":
    main()
