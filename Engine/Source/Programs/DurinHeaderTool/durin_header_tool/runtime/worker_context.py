from pathlib import Path

from durin_header_tool import config as configs


def initialize_worker_config(
    arch: str,
    runtime_variant: str,
    project_files: tuple[Path, ...],
) -> None:
    configs.ARCH = arch
    configs.RUNTIME_VARIANT = runtime_variant
    configs.init_configs(project_files)
