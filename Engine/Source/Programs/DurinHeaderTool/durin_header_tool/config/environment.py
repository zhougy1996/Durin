from pathlib import Path

ARCH = "Win64"
RUNTIME_VARIANT = "DurinEditor"
TOOL_FINGERPRINT = ""
NATIVE_LIBCLANG_FINGERPRINT = ""

DHT_ROOT_DIR = Path(__file__).resolve().parents[2]
DURIN_ROOT_DIR = DHT_ROOT_DIR.parents[3]
DURIN_ENGINE_PROJECT_DIR = DURIN_ROOT_DIR / "Engine"
