from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List
import json

import utils

@dataclass
class ExportedClassInfo:
    pass

@dataclass
class ExportedEnumInfo:
    underlying_type: str
    pass

@dataclass
class ExportedStructInfo:
    pass

@dataclass
class HeaderExportInfo:
    enums: Dict[str, ExportedEnumInfo] = field(default_factory=dict)
    classes: Dict[str, ExportedClassInfo] = field(default_factory=dict)
    structs: Dict[str, ExportedStructInfo] = field(default_factory=dict)

@dataclass
class ModuleExportInfo:
    module_name: str
    exports: Dict[str, HeaderExportInfo] = field(default_factory=dict)

    @classmethod
    def from_file(cls, module_export_file_path: Path) -> "ModuleExportInfo":
        raw_json_data = utils.load_json_file(module_export_file_path, required_fields=["ModuleName"])
        module_export_info = utils.dataclass_from_dict(cls, raw_json_data)
        return module_export_info
    
    def from_string(cls, json_string: str) -> "ModuleExportInfo":
        raw_json_data = utils.parse_json_content(json_string, required_fields=["ModuleName"])
        module_export_info = utils.dataclass_from_dict(cls, raw_json_data)
        return module_export_info
    
def load_module_export_file(module_export_file_path: Path) -> ModuleExportInfo:
    return ModuleExportInfo.from_file(module_export_file_path)

def save_module_export_file(export_info: ModuleExportInfo) -> str:
    module_name = export_info.module_name
    output_path = utils.get_module_export_file_path(module_name)
    json_data = utils.dict_from_dataclass(ModuleExportInfo, export_info, auto_convert=True)
    content = json.dumps(json_data, indent=4)
    utils.generate_file(output_path, content)
    return content