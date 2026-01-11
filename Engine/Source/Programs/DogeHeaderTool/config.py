import logging
import os
import doge_globals as g

logging_level = logging.DEBUG

# Intermediate/<module_name>/<arch>/<mode>/DHT
DHT_DIR_PATTERN = os.path.join(g.DOGE_ENGINE_INTERMEDIATE_DIR, "{}", g.DOGE_ARCH, "DHT")
DHT_MODULE_FILENAME_PATTERN = "{}.json"
DHT_HEADER_LIST_FILENAME = "HeaderFiles.txt"

