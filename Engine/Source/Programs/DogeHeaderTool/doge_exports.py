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
    def from_json_dict(name: str, data: dict):
        return ExportedEnum(
            name=name,
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
    def from_json_dict(name: str, data: dict):
        return ExportedClass(name=name)

@dataclass
class ExportedStruct:
    name: str

    def to_json_dict(self):
        return {
            # "Name": self.name
        }
    
    @staticmethod
    def from_json_dict(name: str, data: dict):
        return ExportedStruct(name=name)

@dataclass
class HeaderExports:
    name: str

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
    def from_json_dict(name: str, data: dict):
        header_exports = HeaderExports(name)
        header_exports.enums = {k: ExportedEnum.from_json_dict(k, v) for k, v in data.get("Enums", {}).items()}
        header_exports.classes = {k: ExportedClass.from_json_dict(k, v) for k, v in data.get("Classes", {}).items()}
        header_exports.structs = {k: ExportedStruct.from_json_dict(k, v) for k, v in data.get("Structs", {}).items()}
        return header_exports
        

@dataclass
class ModuleManifest:
    module_name: str
    exports: Dict[str, HeaderExports] = field(default_factory=dict)
    all_dependencies: List[str] = field(default_factory=list)
    dependent_manifests: List[str] = field(default_factory=list)

    def to_json_dict(self):
        return {
            "ModuleName": self.module_name,
            "Exports": {k: v.to_json_dict() for k, v in self.exports.items()},
            "AllDependencies": self.all_dependencies,
        }
    
    @staticmethod
    def from_json_dict(data: dict):
        module_manifest = ModuleManifest(
            module_name=data["ModuleName"],
            exports={k: HeaderExports.from_json_dict(k, v) for k, v in data.get("Exports", {}).items()},
            all_dependencies=data.get("AllDependencies", []),
        )
        return module_manifest

def load_module_manifest(filepath: str) -> ModuleManifest:
    with open(filepath, "r") as f:
        data = json.load(f)
    manifest = ModuleManifest.from_json_dict(data)
    return manifest