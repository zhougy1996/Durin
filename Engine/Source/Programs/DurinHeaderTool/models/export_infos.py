from dataclasses import dataclass, field, asdict
from pathlib import Path
import json

import utils


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
            module=raw_json_data.get("module", raw_json_data.get("ModuleName", "")),
            symbols=symbols,
        )


def load_module_export_file(module_export_file_path: Path) -> ModuleExportInfo:
    return ModuleExportInfo.from_file(module_export_file_path)


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
