from durin_header_tool import config as configs


def initialize_worker_config(arch: str, profile_name: str) -> None:
    configs.ARCH = arch
    configs.PROFILE_NAME = profile_name
    configs.init_configs()

