from dataclasses import dataclass, field, asdict
import json

from utils import FileFingerprint, LightFileFingerprint
import utils
from models.reflection_info import SYMBOL_NAME_SCHEME, TOOL_VERSION


@dataclass
class ModuleManifest:
    module_name: str
    schema_version: int = 1
    tool_version: str = TOOL_VERSION
    symbol_name_scheme: str = SYMBOL_NAME_SCHEME
    profile: str = ""
    platform: str = ""
    generator_options_hash: str = ""
    dep_module_exports: dict[str, LightFileFingerprint] = field(default_factory=dict)
    reflect_headers: dict[str, FileFingerprint] = field(default_factory=dict)
    resolved_symbol_dependencies: dict[str, dict[str, dict[str, str]]] = field(default_factory=dict)
    generated_outputs: list[str] = field(default_factory=list)


def _fingerprint_from_dict(cls, value):
    if isinstance(value, cls):
        return value
    return cls(**value)


def load_module_manifest_file(module_name: str) -> ModuleManifest:
    manifest_file_path = utils.get_module_manifest_file_path(module_name)
    if not manifest_file_path.exists():
        raise FileNotFoundError(f"Module manifest file for module '{module_name}' not found at expected path: {manifest_file_path}")
    try:
        data = json.loads(manifest_file_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise ValueError(f"Error parsing JSON in module manifest file '{manifest_file_path}': {e}")

    return ModuleManifest(
        module_name=data.get("moduleName", module_name),
        schema_version=data.get("schemaVersion", 0),
        tool_version=data.get("toolVersion", ""),
        symbol_name_scheme=data.get("symbolNameScheme", ""),
        profile=data.get("profile", ""),
        platform=data.get("platform", ""),
        generator_options_hash=data.get("generatorOptionsHash", ""),
        dep_module_exports={
            key: _fingerprint_from_dict(LightFileFingerprint, value)
            for key, value in data.get("dependencyExports", {}).items()
        },
        reflect_headers={
            key: _fingerprint_from_dict(FileFingerprint, value)
            for key, value in data.get("reflectHeaders", {}).items()
        },
        resolved_symbol_dependencies=data.get("resolvedSymbolDependencies", {}),
        generated_outputs=data.get("generatedOutputs", []),
    )


def save_module_manifest_file(manifest: ModuleManifest) -> str:
    output_path = utils.get_module_manifest_file_path(manifest.module_name)
    json_data = {
        "schemaVersion": manifest.schema_version,
        "toolVersion": manifest.tool_version,
        "symbolNameScheme": manifest.symbol_name_scheme,
        "moduleName": manifest.module_name,
        "profile": manifest.profile,
        "platform": manifest.platform,
        "generatorOptionsHash": manifest.generator_options_hash,
        "reflectHeaders": {key: asdict(value) for key, value in sorted(manifest.reflect_headers.items())},
        "dependencyExports": {key: asdict(value) for key, value in sorted(manifest.dep_module_exports.items())},
        "resolvedSymbolDependencies": {
            header: {
                symbol_name: dict(symbol_data)
                for symbol_name, symbol_data in sorted(symbols.items())
            }
            for header, symbols in sorted(manifest.resolved_symbol_dependencies.items())
        },
        "generatedOutputs": sorted(manifest.generated_outputs),
    }
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content
