from dataclasses import dataclass, field, fields
from pathlib import Path
import json

from durin_header_tool import io as utils

EXPORT_SCHEMA_VERSION = 5


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
    IsAbstract: bool = False
    IsScoped: bool = False
    UnderlyingType: str = ""
    UnderlyingKind: str = "Unknown"
    UnderlyingSize: int = 0

    def to_json(self) -> dict[str, object]:
        return {field_info.name: getattr(self, field_info.name) for field_info in fields(self)}

    @classmethod
    def from_json(cls, data: object) -> "ExportedSymbolInfo":
        expected_fields = {field_info.name for field_info in fields(cls)}
        if not isinstance(data, dict) or set(data) != expected_fields:
            raise ValueError("The exported symbol has an invalid JSON object shape.")
        for field_name in expected_fields - {"IsAbstract", "IsScoped", "UnderlyingSize"}:
            if not isinstance(data[field_name], str):
                raise ValueError(f"Exported symbol field '{field_name}' must be a string.")
        for field_name in ("IsAbstract", "IsScoped"):
            if not isinstance(data[field_name], bool):
                raise ValueError(f"Exported symbol field '{field_name}' must be a boolean.")
        underlying_size = data["UnderlyingSize"]
        if not isinstance(underlying_size, int) or isinstance(underlying_size, bool) or underlying_size < 0:
            raise ValueError("Exported symbol field 'UnderlyingSize' must be a non-negative integer.")
        return cls(**data)


@dataclass
class ModuleExportInfo:
    SchemaVersion: int = EXPORT_SCHEMA_VERSION
    Module: str = ""
    Symbols: dict[str, ExportedSymbolInfo] = field(default_factory=dict)

    @classmethod
    def from_file(cls, module_export_file_path: Path) -> "ModuleExportInfo":
        raw_json_data = utils.load_json_file(module_export_file_path)
        if not isinstance(raw_json_data, dict) or not isinstance(raw_json_data.get("Symbols", {}), dict):
            raise ValueError(f"Module export file '{module_export_file_path}' has an invalid JSON structure.")
        symbols = {
            qualified_name: ExportedSymbolInfo.from_json(symbol_data)
            for qualified_name, symbol_data in raw_json_data.get("Symbols", {}).items()
        }
        return cls(
            SchemaVersion=raw_json_data.get("SchemaVersion", 0),
            Module=raw_json_data.get("Module", ""),
            Symbols=symbols,
        )


def load_module_export_file(module_export_file_path: Path) -> ModuleExportInfo:
    return ModuleExportInfo.from_file(module_export_file_path)


def save_module_export_file(export_info: ModuleExportInfo) -> str:
    output_path = utils.get_module_export_file_path(export_info.Module)
    json_data = {
        "SchemaVersion": export_info.SchemaVersion,
        "Module": export_info.Module,
        "Symbols": {
            qualified_name: symbol.to_json()
            for qualified_name, symbol in sorted(export_info.Symbols.items())
        },
    }
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content
