from pathlib import Path

ARCH = "Win64"
BUILD_MODE = "Editor"

DHT_ROOT_DIR = Path(__file__).resolve().parents[1]
DOGE_ROOT_DIR = DHT_ROOT_DIR.parents[3]
DOGE_PROJECT_REGISTER_FILE_PATH = DHT_ROOT_DIR / "Configs" / "RegisteredProjects.json"
DOGE_ENGINE_PROJECT_DIR = DOGE_ROOT_DIR / "Engine"

def init_clang(arch: str):
    import clang.cindex
    libclang_path = None
    if arch == "Win64":
        libclang_path = DOGE_ENGINE_PROJECT_DIR / "Source" / "ThirdParty" / "clang" / "bin" / "libclang.dll"
    elif arch == "MacOS":
        # homebrew install llvm will put libclang.dylib in /opt/homebrew/opt/llvm/lib/libclang.dylib
        libclang_path = Path("/opt/homebrew/opt/llvm/lib/libclang.dylib")
    else:
        raise ValueError(f"Unsupported architecture: {arch}")
    
    if not libclang_path.exists():
        raise FileNotFoundError(f"libclang not found at {libclang_path}")
    
    clang.cindex.Config.set_library_file(str(libclang_path))


