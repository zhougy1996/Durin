import argparse
from commands import *
import config

def main():
    command_manager = CommandManager()
    command_manager.register_command(GenerateModuleCMakeFileCommand())
    command_manager.register_command(GenerateModuleManifestFileCommand())
    command_manager.register_command(GenerateReflectionFilesCommand())

    parser = argparse.ArgumentParser(description="Doge Header Tool")
    command_manager.setup_parser(parser)
    args = parser.parse_args()

    config.init_configs()
    command_manager.execute_command(args.function, args)

if __name__ == "__main__":
    main()

