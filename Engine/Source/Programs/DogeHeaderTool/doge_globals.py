import os

DOGE_ARCH = "x64"

_current_dir = os.path.dirname(os.path.abspath(__file__))
DOGE_DIR = os.path.abspath(os.path.join(_current_dir, "..", "..", "..", ".."))
DOGE_ENGINE_DIR = os.path.abspath(os.path.join(DOGE_DIR, "Engine"))
DOGE_ENGINE_SOURCE_DIR = os.path.abspath(os.path.join(DOGE_ENGINE_DIR, "Source"))
DOGE_ENGINE_BINARY_DIR = os.path.abspath(os.path.join(DOGE_ENGINE_DIR, "Binaries"))
DOGE_ENGINE_INTERMEDIATE_DIR = os.path.abspath(os.path.join(DOGE_ENGINE_DIR, "Intermediate"))
DOGE_ENGINE_CONFIG_DIR = os.path.abspath(os.path.join(DOGE_ENGINE_DIR, "Configs"))
DOGE_ENGINE_LOG_DIR = os.path.abspath(os.path.join(DOGE_ENGINE_DIR, "Logs"))

DOGE_THIRD_PARTY_DIR = os.path.join(DOGE_ENGINE_SOURCE_DIR, "ThirdParty")
CLANG_LIB_DIR = os.path.join(DOGE_THIRD_PARTY_DIR, "clang", "bin")

DOGE_ENGINE_PROJECT_FILE = os.path.abspath(os.path.join(DOGE_ENGINE_DIR, "DogeEngine.dproject"))

ARCH = "Win64"
BUILD_MODE = "Editor"

target_project_config = None

project_meta = None
module_meta = None