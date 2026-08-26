from dataclasses import dataclass, field, fields
from pathlib import Path
import json

from durin_header_tool import io as utils
from durin_header_tool.model.reflection_info import (
    GeneratedSymbol,
    NamespaceSegment,
    make_generated_symbol,
)

EXPORT_SCHEMA_VERSION = 6


@dataclass
class ExportedSymbolInfo:
    Kind: str
    ShortName: str
    Namespace: str
    QualifiedName: str
    Header: str
    API: str
    NamespacePath: tuple[NamespaceSegment, ...] = ()
    BaseQualifiedName: str = ""
    IsAbstract: bool = False
    IsScoped: bool = False
    UnderlyingType: str = ""
    UnderlyingKind: str = "Unknown"
    UnderlyingSize: int = 0

    def __post_init__(self) -> None:
        if not self.NamespacePath:
            self.NamespacePath = tuple(
                NamespaceSegment(segment)
                for segment in self.Namespace.split("::")
                if segment
            )

    @property
    def generated_symbol(self) -> GeneratedSymbol:
        return make_generated_symbol(self.Kind, self.ShortName, self.NamespacePath)

    def to_json(self) -> dict[str, object]:
        result = {field_info.name: getattr(self, field_info.name) for field_info in fields(self)}
        result["NamespacePath"] = [
            {"Name": segment.name, "IsInline": segment.is_inline}
            for segment in self.NamespacePath
        ]
        return result

    @classmethod
    def from_json(cls, data: object) -> "ExportedSymbolInfo":
        expected_fields = {field_info.name for field_info in fields(cls)}
        if not isinstance(data, dict) or set(data) != expected_fields:
            raise ValueError("The exported symbol has an invalid JSON object shape.")
        for field_name in expected_fields - {"NamespacePath", "IsAbstract", "IsScoped", "UnderlyingSize"}:
            if not isinstance(data[field_name], str):
                raise ValueError(f"Exported symbol field '{field_name}' must be a string.")
        raw_namespace_path = data["NamespacePath"]
        if not isinstance(raw_namespace_path, list):
            raise ValueError("Exported symbol field 'NamespacePath' must be an array.")
        namespace_path: list[NamespaceSegment] = []
        for raw_segment in raw_namespace_path:
            if (
                not isinstance(raw_segment, dict)
                or set(raw_segment) != {"Name", "IsInline"}
                or not isinstance(raw_segment["Name"], str)
                or not raw_segment["Name"]
                or not isinstance(raw_segment["IsInline"], bool)
            ):
                raise ValueError("Exported symbol NamespacePath contains an invalid segment.")
            namespace_path.append(NamespaceSegment(raw_segment["Name"], raw_segment["IsInline"]))
        for field_name in ("IsAbstract", "IsScoped"):
            if not isinstance(data[field_name], bool):
                raise ValueError(f"Exported symbol field '{field_name}' must be a boolean.")
        underlying_size = data["UnderlyingSize"]
        if not isinstance(underlying_size, int) or isinstance(underlying_size, bool) or underlying_size < 0:
            raise ValueError("Exported symbol field 'UnderlyingSize' must be a non-negative integer.")
        decoded = dict(data)
        decoded["NamespacePath"] = tuple(namespace_path)
        symbol = cls(**decoded)
        if symbol.Namespace != symbol.generated_symbol.namespace:
            raise ValueError("Exported symbol Namespace disagrees with NamespacePath.")
        expected_qualified_name = (
            f"{symbol.Namespace}::{symbol.ShortName}" if symbol.Namespace else symbol.ShortName
        )
        if symbol.QualifiedName != expected_qualified_name:
            raise ValueError("Exported symbol QualifiedName disagrees with its semantic fields.")
        return symbol


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
        schema_version = raw_json_data.get("SchemaVersion", 0)
        if schema_version != EXPORT_SCHEMA_VERSION:
            raise ValueError(
                f"Module export file '{module_export_file_path}' uses schema {schema_version}; "
                f"expected {EXPORT_SCHEMA_VERSION}."
            )
        symbols = {
            qualified_name: ExportedSymbolInfo.from_json(symbol_data)
            for qualified_name, symbol_data in raw_json_data.get("Symbols", {}).items()
        }
        return cls(
            SchemaVersion=schema_version,
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
