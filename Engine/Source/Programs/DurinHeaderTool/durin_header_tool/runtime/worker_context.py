from durin_header_tool import config as configs


def initialize_worker_config(
    arch: str,
    profile_name: str,
    build_config: str = "Debug",
    build_identifier: str = "",
) -> None:
    configs.ARCH = arch
    configs.PROFILE_NAME = profile_name
    configs.BUILD_CONFIG = build_config
    configs.BUILD_IDENTIFIER = build_identifier
    configs.init_configs()
