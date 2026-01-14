from dataclasses import dataclass, field
from typing import Dict, List
import json

@dataclass
class ExportedEnum:
    name: str
    underlying_type: str

    def to_json_dict(self):
        return {
            # "Name": self.name,
            "UnderlyingType": self.underlying_type
        }
    
    @staticmethod
    def from_json_dict(data: dict):
        return ExportedEnum(
            name=data["Name"],
            underlying_type=data["UnderlyingType"]
        )

@dataclass
class ExportedClass:
    name: str

    def to_json_dict(self):
        return {
            # "Name": self.name
        }
    
    @staticmethod
    def from_json_dict(data: dict):
        return ExportedClass(name=data["Name"])

@dataclass
class ExportedStruct:
    name: str

    def to_json_dict(self):
        return {
            # "Name": self.name
        }
    
    @staticmethod
    def from_json_dict(data: dict):
        return ExportedStruct(name=data["Name"])

@dataclass
class HeaderExports:
    enums: Dict[str, ExportedEnum] = field(default_factory=dict)
    classes: Dict[str, ExportedClass] = field(default_factory=dict)
    structs: Dict[str, ExportedStruct] = field(default_factory=dict)

    def to_json_dict(self):
        return {
            "Enums": {k: v.to_json_dict() for k, v in self.enums.items()},
            "Classes": {k: v.to_json_dict() for k, v in self.classes.items()},
            "Structs": {k: v.to_json_dict() for k, v in self.structs.items()},
        }
    
    @staticmethod
    def from_json_dict(data: dict):
        header_exports = HeaderExports()
        for enum_name, enum_data in data.get("Enums", {}).items():
            header_exports.enums[enum_name] = ExportedEnum.from_json_dict(enum_data)
        for class_name, class_data in data.get("Classes", {}).items():
            header_exports.classes[class_name] = ExportedClass.from_json_dict(class_data)
        for struct_name, struct_data in data.get("Structs", {}).items():
            header_exports.structs[struct_name] = ExportedStruct.from_json_dict(struct_data)
        return header_exports
        

@dataclass
class ModuleManifest:
    module_name: str
    exports: Dict[str, HeaderExports] = field(default_factory=dict)
    all_dependencies: List[str] = field(default_factory=list)

    def to_json_dict(self):
        return {
            "ModuleName": self.module_name,
            "Exports": {k: v.to_json_dict() for k, v in self.exports.items()},
            "AllDependencies": self.all_dependencies
        }
    
    @staticmethod
    def from_json_dict(data: dict):
        module_manifest = ModuleManifest(
            module_name=data["ModuleName"],
            exports={k: HeaderExports.from_json_dict(v) for k, v in data.get("Exports", {}).items()},
            all_dependencies=data.get("AllDependencies", [])
        )
        return module_manifest

def load_module_manifest(filepath: str) -> ModuleManifest:
    with open(filepath, "r") as f:
        data = json.load(f)
    mainifest = ModuleManifest.from_json_dict(data)
    return mainifest