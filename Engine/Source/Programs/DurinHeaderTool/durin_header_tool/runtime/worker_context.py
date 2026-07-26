from durin_header_tool import config as configs


def initialize_worker_config(
    arch: str,
    runtime_variant: str,
    build_identifier: str = "",
) -> None:
    configs.ARCH = arch
    configs.RUNTIME_VARIANT = runtime_variant
    configs.BUILD_IDENTIFIER = build_identifier
    configs.init_configs()
