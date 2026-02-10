from dataclasses import dataclass, field
import os
from typing import Dict, List
from enum import Enum
from doge_exports import HeaderExports, ExportedClass, ExportedEnum, ExportedStruct, ModuleManifest, load_module_manifest
import doge_globals as g
from doge_project import *

class TypeKind(Enum):
    ENUM = "Enum"
    CLASS = "Class"
    STRUCT = "Struct"

@dataclass
class TypeInfo:
    name: str
    module: str
    kind: TypeKind

@dataclass
class ClassInfo(TypeInfo):
    pass

@dataclass
class EnumInfo(TypeInfo):
    underlying_type: str

@dataclass
class StructInfo(TypeInfo):
    pass

@dataclass
class DogeTypeDatabase:
    types: Dict[str, TypeInfo] = field(default_factory=dict)

    classes: Dict[str, ClassInfo] = field(default_factory=dict)
    enums: Dict[str, EnumInfo] = field(default_factory=dict)
    structs: Dict[str, StructInfo] = field(default_factory=dict)

    def add(self, type_info: TypeInfo) -> None:
        if type_info.name in self.types:
            raise ValueError(f"Type {type_info.name} already exists in the database.")
        self.types[type_info.name] = type_info
        if isinstance(type_info, ClassInfo):
            self.classes[type_info.name] = type_info
        elif isinstance(type_info, EnumInfo):
            self.enums[type_info.name] = type_info
        elif isinstance(type_info, StructInfo):
            self.structs[type_info.name] = type_info

    def append_manifest(self, module_manifest: ModuleManifest) -> None:
        module_name = module_manifest.module_name
        for header_name, header_exports in module_manifest.exports.items():
            for enum_name, enum in header_exports.enums.items():
                enum_info = EnumInfo(
                    name=enum_name,
                    module=module_name,
                    kind=TypeKind.ENUM,
                    underlying_type=enum.underlying_type
                )
                self.add(enum_info)
            for class_name, cls in header_exports.classes.items():
                class_info = ClassInfo(
                    name=class_name,
                    module=module_name,
                    kind=TypeKind.CLASS
                )
                self.add(class_info)
            for struct_name, struct in header_exports.structs.items():
                struct_info = StructInfo(
                    name=struct_name,
                    module=module_name,
                    kind=TypeKind.STRUCT
                )
                self.add(struct_info)

    def get(self, type_name: str) -> TypeInfo:
        return self.types.get(type_name)
    
    def contains(self, type_name: str) -> bool:
        return type_name in self.types
    
def build_database(project_name: str, module_name: str) -> DogeTypeDatabase:
    db = DogeTypeDatabase() 
    project : DogeProjectConfig = get_project_config(project_name)
    
    module_manifest_path = project.get_module_manifest_path(module_name)
    manifest = load_module_manifest(module_manifest_path)

    for dep_module_name in manifest.all_dependencies:
        dep_manifest_path = project.get_module_manifest_path(dep_module_name)
        dep_manifest = load_module_manifest(dep_manifest_path)
        db.append_manifest(dep_manifest)

    db.append_manifest(manifest)
    return db


