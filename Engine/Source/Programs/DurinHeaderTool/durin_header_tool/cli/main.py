import argparse
from contextlib import ExitStack
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


def _get_output_lock_paths(args):
    if args.function == "prepare_project_build":
        project_config = configs.load_project_config_file(args.project)
        # Preparation may delete disabled module directories, so it takes both
        # the project metadata lock and every module lock that it can affect.
        return [
            utils.get_dht_project_lock_file_path(project_config.project_name),
            *(
                utils.get_dht_module_lock_file_path(module_name)
                for module_name in sorted(project_config.modules)
            ),
        ]
    if args.function in {"generate_module_export_file", "generate_reflection_files"}:
        configs.get_module_config(args.module)  # Validate the module before using it as a lock scope.
        return [utils.get_dht_module_lock_file_path(args.module)]
    return []

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
    configs.RUNTIME_VARIANT = args.runtime_variant
    configs.TOOL_FINGERPRINT = args.tool_fingerprint
    init_logging(args.log)
    project_files = list(args.project_file)
    if args.function == "prepare_project_build":
        project_files.append(args.project)
    configs.init_configs(project_files)

    with ExitStack() as lock_stack:
        for lock_path in _get_output_lock_paths(args):
            lock_stack.enter_context(utils.acquire_output_lock(lock_path, args.function))
        command_manager.execute_command(args.function, args)

if __name__ == "__main__":
    main()
