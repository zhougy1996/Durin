from dataclasses import dataclass, field
from typing import Dict, List
from enum import Enum

class TypeKind(Enum):
    ENUM = "Enum"
    CLASS = "Class"
    STRUCT = "Struct"

@dataclass
class TypeInfo:
    name: str
    full_name: str
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

    def get(self, type_name: str) -> TypeInfo:
        return self.types.get(type_name)
    
    def contains(self, type_name: str) -> bool:
        return type_name in self.types
    
    


    
