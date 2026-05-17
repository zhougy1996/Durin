import os

DURIN_ARCH = "x64"

_current_dir = os.path.dirname(os.path.abspath(__file__))
DURIN_DIR = os.path.abspath(os.path.join(_current_dir, "..", "..", "..", ".."))
DURIN_ENGINE_DIR = os.path.abspath(os.path.join(DURIN_DIR, "Engine"))
DURIN_ENGINE_SOURCE_DIR = os.path.abspath(os.path.join(DURIN_ENGINE_DIR, "Source"))
DURIN_ENGINE_BINARY_DIR = os.path.abspath(os.path.join(DURIN_ENGINE_DIR, "Binaries"))
DURIN_ENGINE_INTERMEDIATE_DIR = os.path.abspath(os.path.join(DURIN_ENGINE_DIR, "Intermediate"))
DURIN_ENGINE_CONFIG_DIR = os.path.abspath(os.path.join(DURIN_ENGINE_DIR, "Configs"))
DURIN_ENGINE_LOG_DIR = os.path.abspath(os.path.join(DURIN_ENGINE_DIR, "Logs"))

DURIN_THIRD_PARTY_DIR = os.path.join(DURIN_ENGINE_SOURCE_DIR, "ThirdParty")
CLANG_LIB_DIR = os.path.join(DURIN_THIRD_PARTY_DIR, "clang", "bin")

DURIN_ENGINE_PROJECT_FILE = os.path.abspath(os.path.join(DURIN_ENGINE_DIR, "DurinEngine.dproject"))

ARCH = "Win64"
BUILD_MODE = "Editor"

target_project_config = None

# Project and module config storage

# project name -> DurinProjectConfig
project_configs = {} 
# module name -> DurinModuleConfig
module_configs = {} 

project_meta = None
module_meta = None

type_db = None
