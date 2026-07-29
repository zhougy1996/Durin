from pathlib import Path
from dataclasses import MISSING, fields, is_dataclass
from typing import Dict, Any, Type, get_origin, get_args

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

# Converts a snake_case string to PascalCase
def _snake_to_pascal(snake_str: str) -> str:
    pascal_str = ''.join(word.capitalize() for word in snake_str.split('_'))
    return pascal_str

def dataclass_from_dict(
    cls: Type,
    data: Dict[str, Any],
    auto_convert: bool = True
) -> Any:
    """
    Convert a dictionary to a dataclass instance, supporting nested dataclasses and container types.
    
    Args:
        cls: Target dataclass type to instantiate (e.g. ModuleExportInfo)
        data: Dictionary to parse (supports nested structures with PascalCase keys)
        auto_convert: Whether to automatically convert PascalCase JSON keys back to snake_case Python field names
    
    Returns:
        Instantiated dataclass object with all nested structures resolved
    
    Raises:
        ValueError: If input data is not a dictionary, cls is not a dataclass, or required fields are missing
    """
    # Basic input validation
    if not isinstance(data, dict):
        raise ValueError(f"Expected a dictionary to create {cls.__name__}, got {type(data)}")
    
    if not is_dataclass(cls):
        raise ValueError(f"Expected a dataclass type, got {type(cls)}")

    # Build mapping between Python field names and JSON keys
    field_mapping = {}
    for field in fields(cls):
        json_key = field.metadata.get("json_key")
        if json_key:
            # Use explicit json_key from field metadata if provided
            field_mapping[field.name] = json_key
        elif auto_convert:
            # Auto-convert snake_case Python field to PascalCase JSON key
            field_mapping[field.name] = _snake_to_pascal(field.name)

    # Reverse mapping: JSON key → Python field name (for value lookup)
    json_to_py_mapping = {}
    py_field_info = {}  # Store field metadata (type, default values) for later use
    for field in fields(cls):
        py_field = field.name
        py_field_info[py_field] = field

        json_key = field.metadata.get("json_key")
        if json_key:
            json_to_py_mapping[json_key] = py_field
        elif auto_convert:
            json_to_py_mapping[_snake_to_pascal(py_field)] = py_field
        else:
            json_to_py_mapping[py_field] = py_field

    # Recursive value processing function
    def _process_value(field_type: Type, value: Any) -> Any:
        """
        Recursively process values to resolve nested structures:
        - Convert dictionaries to dataclass instances
        - Resolve dataclass objects inside Dict/List containers
        - Keep basic types (str/int/bool) unchanged
        
        Args:
            field_type: Expected type of the field (from dataclass annotation)
            value: Raw value from input dictionary
        
        Returns:
            Processed value (dataclass instance/resolved container/basic type)
        """
        # Handle None values directly
        if value is None:
            return None

        # Case 1: Target type is a dataclass → recursively parse to dataclass instance
        if is_dataclass(field_type) and get_origin(field_type) is None and isinstance(field_type, type):
            return dataclass_from_dict(field_type, value, auto_convert)

        # Case 2: Target type is Dict (e.g. Dict[str, HeaderExportInfo])
        origin_type = get_origin(field_type)
        if origin_type is dict:
            dict_args = get_args(field_type)
            if len(dict_args) < 2:
                # Return raw dict if no specific type annotation is provided
                return value
            _, value_cls = dict_args  # Extract value type from Dict[KeyType, ValueType]
            processed_dict = {}
            for k, v in value.items():
                processed_dict[k] = _process_value(value_cls, v)
            return processed_dict

        # Case 3: Target type is List (e.g. List[ExportedClassInfo])
        if origin_type is list:
            list_args = get_args(field_type)
            if not list_args:
                # Return raw list if no specific type annotation is provided
                return value
            elem_cls = list_args[0]  # Extract element type from List[ElementType]
            processed_list = []
            for elem in value:
                processed_list.append(_process_value(elem_cls, elem))
            return processed_list

        # Case 4: Basic types (str/int/bool/float) → return as-is
        return value

    # Build initialization arguments for dataclass
    init_kwargs = {}
    for json_field, py_field in json_to_py_mapping.items():
        field = py_field_info[py_field]
        field_type = field.type

        # Get value from input data or use default values
        if json_field in data:
            raw_value = data[json_field]
            # Resolve nested structures recursively
            processed_value = _process_value(field_type, raw_value)
            init_kwargs[py_field] = processed_value
        else:
            # Correctly check if field has default value (using dataclasses.MISSING)
            if field.default is not MISSING:
                # Field has explicit default value (e.g. namespace: str = "")
                init_kwargs[py_field] = field.default
            elif field.default_factory is not MISSING:
                # Field has default factory (e.g. enums: Dict = field(default_factory=dict))
                init_kwargs[py_field] = field.default_factory()
            else:
                # Raise error if required field is missing (no default value)
                raise ValueError(f"Missing required field: {json_field} (mapped to Python field: {py_field})")

    # Create and return the dataclass instance
    return cls(** init_kwargs)
