from dataclasses import dataclass, field, asdict
from pathlib import Path
import json

from durin_header_tool import io as utils
from durin_header_tool.io import FileFingerprint
from durin_header_tool.model.reflection_info import SYMBOL_NAME_SCHEME, TOOL_VERSION

EXPORT_SCHEMA_VERSION = 4


@dataclass
class ExportedSymbolInfo:
    Kind: str
    ShortName: str
    Namespace: str
    QualifiedName: str
    GeneratedHelperName: str
    Header: str
    API: str
    BaseQualifiedName: str = ""
    IsScoped: bool = False
    UnderlyingType: str = ""
    UnderlyingKind: str = "Unknown"
    UnderlyingSize: int = 0


@dataclass
class ModuleExportInfo:
    SchemaVersion: int = EXPORT_SCHEMA_VERSION
    Module: str = ""
    Symbols: dict[str, ExportedSymbolInfo] = field(default_factory=dict)

    @classmethod
    def from_file(cls, module_export_file_path: Path) -> "ModuleExportInfo":
        raw_json_data = utils.load_json_file(module_export_file_path)
        symbols = {
            qualified_name: ExportedSymbolInfo(**symbol_data)
            for qualified_name, symbol_data in raw_json_data.get("Symbols", {}).items()
        }
        return cls(
            SchemaVersion=raw_json_data.get("SchemaVersion", 0),
            Module=raw_json_data.get("Module", ""),
            Symbols=symbols,
        )


@dataclass
class ModuleExportManifest:
    SchemaVersion: int = EXPORT_SCHEMA_VERSION
    ToolVersion: str = TOOL_VERSION
    SymbolNameScheme: str = SYMBOL_NAME_SCHEME
    Module: str = ""
    Profile: str = ""
    Platform: str = ""
    GeneratorOptionsHash: str = ""
    ReflectHeaders: dict[str, FileFingerprint] = field(default_factory=dict)

    @classmethod
    def from_file(cls, module_export_manifest_file_path: Path) -> "ModuleExportManifest":
        raw_json_data = utils.load_json_file(module_export_manifest_file_path)
        return cls(
            SchemaVersion=raw_json_data.get("SchemaVersion", 0),
            ToolVersion=raw_json_data.get("ToolVersion", ""),
            SymbolNameScheme=raw_json_data.get("SymbolNameScheme", ""),
            Module=raw_json_data.get("Module", ""),
            Profile=raw_json_data.get("Profile", ""),
            Platform=raw_json_data.get("Platform", ""),
            GeneratorOptionsHash=raw_json_data.get("GeneratorOptionsHash", ""),
            ReflectHeaders={
                header: _fingerprint_from_json(fingerprint)
                for header, fingerprint in raw_json_data.get("ReflectHeaders", {}).items()
            },
        )


def load_module_export_file(module_export_file_path: Path) -> ModuleExportInfo:
    return ModuleExportInfo.from_file(module_export_file_path)


def load_module_export_manifest_file(module_export_manifest_file_path: Path) -> ModuleExportManifest:
    return ModuleExportManifest.from_file(module_export_manifest_file_path)


def save_module_export_file(export_info: ModuleExportInfo) -> str:
    output_path = utils.get_module_export_file_path(export_info.Module)
    json_data = {
        "SchemaVersion": export_info.SchemaVersion,
        "Module": export_info.Module,
        "Symbols": {
            qualified_name: asdict(symbol)
            for qualified_name, symbol in sorted(export_info.Symbols.items())
        },
    }
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content


def save_module_export_manifest_file(manifest: ModuleExportManifest) -> str:
    output_path = utils.get_module_export_manifest_file_path(manifest.Module)
    json_data = {
        "SchemaVersion": manifest.SchemaVersion,
        "ToolVersion": manifest.ToolVersion,
        "SymbolNameScheme": manifest.SymbolNameScheme,
        "Module": manifest.Module,
        "Profile": manifest.Profile,
        "Platform": manifest.Platform,
        "GeneratorOptionsHash": manifest.GeneratorOptionsHash,
        "ReflectHeaders": {
            header: _fingerprint_to_json(fingerprint)
            for header, fingerprint in sorted(manifest.ReflectHeaders.items())
        },
    }
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content


def _fingerprint_to_json(fingerprint: FileFingerprint) -> dict[str, object]:
    return {
        "Timestamp": fingerprint.timestamp,
        "FileSize": fingerprint.file_size,
        "MD5": fingerprint.md5,
    }


def _fingerprint_from_json(data: dict[str, object]) -> FileFingerprint:
    return FileFingerprint(
        timestamp=data.get("Timestamp", 0.0),
        file_size=data.get("FileSize", 0),
        md5=data.get("MD5", ""),
    )
