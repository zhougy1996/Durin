import logging
import os

logging_level = logging.DEBUG

DOGE_ARCH = "x64"

_current_dir = os.path.dirname(os.path.abspath(__file__))
DOGE_ROOT_DIR = os.path.abspath(os.path.join(_current_dir, "..", "..", "..", ".."))
DOGE_ENGINE_ROOT_DIR = os.path.abspath(os.path.join(DOGE_ROOT_DIR, "Engine"))
DOGE_SOURCE_DIR = os.path.abspath(os.path.join(DOGE_ENGINE_ROOT_DIR, "Source"))
DOGE_BINARY_DIR = os.path.abspath(os.path.join(DOGE_ENGINE_ROOT_DIR, "Binaries"))
DOGE_INTERMEDIATE_DIR = os.path.abspath(os.path.join(DOGE_ENGINE_ROOT_DIR, "Intermediate"))
DOGE_CONFIG_DIR = os.path.abspath(os.path.join(DOGE_ENGINE_ROOT_DIR, "Configs"))

DOGE_THIRD_PARTY_DIR = os.path.join(DOGE_SOURCE_DIR, "ThirdParty")
CLANG_LIB_DIR = os.path.join(DOGE_THIRD_PARTY_DIR, "clang", "bin")

# Intermediate/<module_name>/<arch>/<mode>/DHT
DHT_DIR_PATTERN = os.path.join(DOGE_INTERMEDIATE_DIR, "{}", DOGE_ARCH, "DHT")
DHT_MODULE_FILENAME_PATTERN = "{}.json"
DHT_HEADER_LIST_FILENAME = "HeaderFiles.txt"

