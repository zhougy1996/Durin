import os
import logging
import clang.cindex

import config as cfg
import doge_globals as g

def init_clang():
    clang.cindex.Config.set_library_path(g.CLANG_LIB_DIR)

def init_logging():
    logging.getLogger().setLevel(cfg.logging_level)
    logging.basicConfig(format='[DBT] [%(levelname)s] %(message)s')

def get_dht_dir(module_name: str) -> str:
    return cfg.DHT_DIR_PATTERN.format(module_name)

def get_symbol_table_filepath(module_name: str) -> str:
    return os.path.join(get_dht_dir(module_name), cfg.DHT_MODULE_FILENAME_PATTERN.format(module_name))

def get_input_headers() -> list[str]:
    header_list_file = os.path.join(g.module_dht_dir, cfg.DHT_HEADER_LIST_FILENAME)
    if not os.path.isfile(header_list_file):
        return []
    with open(header_list_file, "r") as f:
        headers = [line.strip() for line in f.readlines() if line.strip()]
    return headers
