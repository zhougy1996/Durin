from pathlib import Path

ARCH = "Win64"
BUILD_MODE = "Editor"

DHT_ROOT_DIR = Path(__file__).resolve().parents[1]
DOGE_ROOT_DIR = DHT_ROOT_DIR.parents[3]
DOGE_PROJECT_REGISTER_FILE_PATH = DHT_ROOT_DIR / "Configs" / "RegisteredProjects.json"
DOGE_ENGINE_PROJECT_DIR = DOGE_ROOT_DIR / "Engine"

LIBCLANG_BIN_DIR = DOGE_ENGINE_PROJECT_DIR / "Source" / "ThirdParty" / "clang" / "bin"
LIBCLANG_DLL_PATH = LIBCLANG_BIN_DIR / "libclang.dll"

if not LIBCLANG_DLL_PATH.exists():
    print(f"Libclang DLL '{LIBCLANG_DLL_PATH}' does not exist.")
