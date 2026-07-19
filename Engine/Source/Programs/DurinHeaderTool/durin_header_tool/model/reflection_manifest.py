from dataclasses import dataclass, field
import json

from durin_header_tool.io import FileFingerprint, LightFileFingerprint
from durin_header_tool import io as utils
from durin_header_tool.model.reflection_info import SYMBOL_NAME_SCHEME, TOOL_VERSION

REFLECTION_MANIFEST_SCHEMA_VERSION = 3


@dataclass
class ModuleManifest:
    module_name: str
    schema_version: int = REFLECTION_MANIFEST_SCHEMA_VERSION
    tool_version: str = TOOL_VERSION
    tool_fingerprint: str = ""
    symbol_name_scheme: str = SYMBOL_NAME_SCHEME
    profile: str = ""
    platform: str = ""
    generator_options_hash: str = ""
    dep_module_exports: dict[str, LightFileFingerprint] = field(default_factory=dict)
    reflect_headers: dict[str, FileFingerprint] = field(default_factory=dict)
    resolved_symbol_dependencies: dict[str, dict[str, dict[str, str]]] = field(default_factory=dict)


def load_module_manifest_file(module_name: str) -> ModuleManifest:
    manifest_file_path = utils.get_module_manifest_file_path(module_name)
    if not manifest_file_path.exists():
        raise FileNotFoundError(f"Module manifest file for module '{module_name}' not found at expected path: {manifest_file_path}")
    try:
        data = json.loads(manifest_file_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise ValueError(f"Error parsing JSON in module manifest file '{manifest_file_path}': {e}")

    if not isinstance(data, dict):
        raise ValueError(f"Module manifest file '{manifest_file_path}' must contain a JSON object.")

    object_fields = ("DependencyExports", "ReflectHeaders", "ResolvedSymbolDependencies")
    for field_name in object_fields:
        if not isinstance(data.get(field_name, {}), dict):
            raise ValueError(f"Field '{field_name}' in module manifest file '{manifest_file_path}' must be a JSON object.")

    return ModuleManifest(
        module_name=data.get("ModuleName", module_name),
        schema_version=data.get("SchemaVersion", 0),
        tool_version=data.get("ToolVersion", ""),
        tool_fingerprint=data.get("ToolFingerprint", ""),
        symbol_name_scheme=data.get("SymbolNameScheme", ""),
        profile=data.get("Profile", ""),
        platform=data.get("Platform", ""),
        generator_options_hash=data.get("GeneratorOptionsHash", ""),
        dep_module_exports={
            key: _light_fingerprint_from_json(value)
            for key, value in data.get("DependencyExports", {}).items()
        },
        reflect_headers={
            key: _file_fingerprint_from_json(value)
            for key, value in data.get("ReflectHeaders", {}).items()
        },
        resolved_symbol_dependencies=data.get("ResolvedSymbolDependencies", {}),
    )


def save_module_manifest_file(manifest: ModuleManifest) -> str:
    output_path = utils.get_module_manifest_file_path(manifest.module_name)
    json_data = {
        "SchemaVersion": manifest.schema_version,
        "ToolVersion": manifest.tool_version,
        "ToolFingerprint": manifest.tool_fingerprint,
        "SymbolNameScheme": manifest.symbol_name_scheme,
        "ModuleName": manifest.module_name,
        "Profile": manifest.profile,
        "Platform": manifest.platform,
        "GeneratorOptionsHash": manifest.generator_options_hash,
        "ReflectHeaders": {key: _file_fingerprint_to_json(value) for key, value in sorted(manifest.reflect_headers.items())},
        "DependencyExports": {key: _light_fingerprint_to_json(value) for key, value in sorted(manifest.dep_module_exports.items())},
        "ResolvedSymbolDependencies": {
            header: {
                symbol_name: dict(symbol_data)
                for symbol_name, symbol_data in sorted(symbols.items())
            }
            for header, symbols in sorted(manifest.resolved_symbol_dependencies.items())
        },
    }
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content


def _light_fingerprint_to_json(fingerprint: LightFileFingerprint) -> dict[str, object]:
    return {
        "Timestamp": fingerprint.timestamp,
        "FileSize": fingerprint.file_size,
    }


def _file_fingerprint_to_json(fingerprint: FileFingerprint) -> dict[str, object]:
    return {
        "Timestamp": fingerprint.timestamp,
        "FileSize": fingerprint.file_size,
        "MD5": fingerprint.md5,
    }


def _light_fingerprint_from_json(data: dict[str, object]) -> LightFileFingerprint:
    return LightFileFingerprint(
        timestamp=data.get("Timestamp", 0.0),
        file_size=data.get("FileSize", 0),
    )


def _file_fingerprint_from_json(data: dict[str, object]) -> FileFingerprint:
    return FileFingerprint(
        timestamp=data.get("Timestamp", 0.0),
        file_size=data.get("FileSize", 0),
        md5=data.get("MD5", ""),
    )
