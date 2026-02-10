from pathlib import Path
from dataclasses import fields, is_dataclass
from typing import Dict, Any

import os
import re
import json
    
def load_json_file(file_path: Path, required_fields: list = None) -> Dict[str, Any]:
    if not file_path.exists():
        raise FileNotFoundError(f"File {file_path} does not exist.")
    
    with open(file_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    
    if required_fields:
        missing_fields = [field for field in required_fields if field not in data]
        if missing_fields:
            raise ValueError(f"Missing required fields: {', '.join(missing_fields)}")
    
    return data

# Converts a PascalCase string to snake_case
def _pascal_to_snake(pascal_str: str) -> str:
    snake_str = re.sub(r'(?<!^)(?=[A-Z])', '_', pascal_str).lower()
    return snake_str

# Converts a snake_case string to PascalCase
def _snake_to_pascal(snake_str: str) -> str:
    pascal_str = ''.join(word.capitalize() for word in snake_str.split('_'))
    return pascal_str

def dataclass_from_dict(cls, data: Dict[str, Any], auto_convert: bool = True) -> Any:
    if not isinstance(data, dict):
        raise ValueError(f"Expected a dictionary to create {cls.__name__}, got {type(data)}")
    
    if not is_dataclass(cls):
        raise ValueError(f"Expected a dataclass type, got {type(cls)}")
    
    field_mapping = {}
    for field in fields(cls):
        json_key = field.metadata.get("json_key")
        if json_key:
            field_mapping[field.name] = json_key
        elif auto_convert:
            field_mapping[field.name] = _snake_to_pascal(field.name)
    
    init_kwargs = {}
    for py_field, json_field in field_mapping.items():
        if json_field in data:
            init_kwargs[py_field] = data[json_field]
        else:
            if field.default is not None or field.default_factory is not None:
                continue
            raise ValueError(f"Missing field: {json_field} (corresponding Python field: {py_field})")
    
    return cls(**init_kwargs)