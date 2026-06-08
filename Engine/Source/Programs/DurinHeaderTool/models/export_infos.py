from dataclasses import dataclass, field, asdict
from pathlib import Path
import json

import utils
from utils import FileFingerprint
from models.reflection_info import SYMBOL_NAME_SCHEME, TOOL_VERSION


@dataclass
class ExportedSymbolInfo:
    kind: str
    shortName: str
    namespace: str
    qualifiedName: str
    generatedHelperName: str
    header: str
    api: str
    baseQualifiedName: str = ""


@dataclass
class ModuleExportInfo:
    schemaVersion: int = 1
    module: str = ""
    symbols: dict[str, ExportedSymbolInfo] = field(default_factory=dict)

    @classmethod
    def from_file(cls, module_export_file_path: Path) -> "ModuleExportInfo":
        raw_json_data = utils.load_json_file(module_export_file_path)
        symbols = {
            qualified_name: ExportedSymbolInfo(**symbol_data)
            for qualified_name, symbol_data in raw_json_data.get("symbols", {}).items()
        }
        return cls(
            schemaVersion=raw_json_data.get("schemaVersion", 1),
            module=raw_json_data.get("module", ""),
            symbols=symbols,
        )


@dataclass
class ModuleExportManifest:
    schemaVersion: int = 1
    toolVersion: str = TOOL_VERSION
    symbolNameScheme: str = SYMBOL_NAME_SCHEME
    module: str = ""
    profile: str = ""
    platform: str = ""
    generatorOptionsHash: str = ""
    reflectHeaders: dict[str, FileFingerprint] = field(default_factory=dict)
    headerSymbols: dict[str, dict[str, ExportedSymbolInfo]] = field(default_factory=dict)

    @classmethod
    def from_file(cls, module_export_manifest_file_path: Path) -> "ModuleExportManifest":
        raw_json_data = utils.load_json_file(module_export_manifest_file_path)
        header_symbols = {}
        for header, symbols_data in raw_json_data.get("headerSymbols", {}).items():
            header_symbols[header] = {
                qualified_name: ExportedSymbolInfo(**symbol_data)
                for qualified_name, symbol_data in symbols_data.items()
            }
        return cls(
            schemaVersion=raw_json_data.get("schemaVersion", 0),
            toolVersion=raw_json_data.get("toolVersion", ""),
            symbolNameScheme=raw_json_data.get("symbolNameScheme", ""),
            module=raw_json_data.get("module", ""),
            profile=raw_json_data.get("profile", ""),
            platform=raw_json_data.get("platform", ""),
            generatorOptionsHash=raw_json_data.get("generatorOptionsHash", ""),
            reflectHeaders={
                header: FileFingerprint(**fingerprint)
                for header, fingerprint in raw_json_data.get("reflectHeaders", {}).items()
            },
            headerSymbols=header_symbols,
        )


def load_module_export_file(module_export_file_path: Path) -> ModuleExportInfo:
    return ModuleExportInfo.from_file(module_export_file_path)


def load_module_export_manifest_file(module_export_manifest_file_path: Path) -> ModuleExportManifest:
    return ModuleExportManifest.from_file(module_export_manifest_file_path)


def save_module_export_file(export_info: ModuleExportInfo) -> str:
    output_path = utils.get_module_export_file_path(export_info.module)
    json_data = {
        "schemaVersion": export_info.schemaVersion,
        "module": export_info.module,
        "symbols": {
            qualified_name: asdict(symbol)
            for qualified_name, symbol in sorted(export_info.symbols.items())
        },
    }
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content


def save_module_export_manifest_file(manifest: ModuleExportManifest) -> str:
    output_path = utils.get_module_export_manifest_file_path(manifest.module)
    json_data = {
        "schemaVersion": manifest.schemaVersion,
        "toolVersion": manifest.toolVersion,
        "symbolNameScheme": manifest.symbolNameScheme,
        "module": manifest.module,
        "profile": manifest.profile,
        "platform": manifest.platform,
        "generatorOptionsHash": manifest.generatorOptionsHash,
        "reflectHeaders": {
            header: asdict(fingerprint)
            for header, fingerprint in sorted(manifest.reflectHeaders.items())
        },
        "headerSymbols": {
            header: {
                qualified_name: asdict(symbol)
                for qualified_name, symbol in sorted(symbols.items())
            }
            for header, symbols in sorted(manifest.headerSymbols.items())
        },
    }
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content
