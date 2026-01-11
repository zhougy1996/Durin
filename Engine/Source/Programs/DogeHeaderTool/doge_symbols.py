from dataclasses import dataclass, field
from typing import Dict, List


@dataclass
class EnumSymbol:
    name: str
    underlying_type: str

    def to_json_dict(self):
        return {
            # "Name": self.name,
            "UnderlyingType": self.underlying_type
        }

@dataclass
class ClassSymbol:
    name: str

    def to_json_dict(self):
        return {
            # "Name": self.name
        }

@dataclass
class StructSymbol:
    name: str

    def to_json_dict(self):
        return {
            # "Name": self.name
        }

@dataclass
class HeaderExports:
    enums: Dict[str, EnumSymbol] = field(default_factory=dict)
    classes: Dict[str, ClassSymbol] = field(default_factory=dict)
    structs: Dict[str, StructSymbol] = field(default_factory=dict)

    def to_json_dict(self):
        return {
            "Enums": {k: v.to_json_dict() for k, v in self.enums.items()},
            "Classes": {k: v.to_json_dict() for k, v in self.classes.items()},
            "Structs": {k: v.to_json_dict() for k, v in self.structs.items()},
        }

@dataclass
class ModuleExports:
    module_name: str
    headers: Dict[str, HeaderExports] = field(default_factory=dict)

    def to_json_dict(self):
        return {
            "ModuleName": self.module_name,
            "Headers": {k: v.to_json_dict() for k, v in self.headers.items()},
        }