import os
import globals as g
from config import *

def get_dht_dir(module_name: str) -> str:
    return DHT_DIR_PATTERN.format(module_name)

def get_dht_module_filepath(module_name: str) -> str:
    return os.path.join(get_dht_dir(module_name), DHT_MODULE_FILENAME_PATTERN.format(module_name))

def get_input_headers() -> list[str]:
    header_list_file = os.path.join(g.dht_dir, DHT_HEADER_LIST_FILENAME)
    if not os.path.isfile(header_list_file):
        return []
    with open(header_list_file, "r") as f:
        headers = [line.strip() for line in f.readlines() if line.strip()]
    return headers
