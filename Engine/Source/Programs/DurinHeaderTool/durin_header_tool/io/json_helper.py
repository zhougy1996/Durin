from pathlib import Path
from dataclasses import MISSING, fields, is_dataclass
from functools import lru_cache
from typing import Dict, Any, Type, get_origin, get_args

import json
from jsonschema import validators


SCHEMA_DIR = Path(__file__).resolve().parents[2] / "schemas"


class _DuplicateJsonKeyError(ValueError):
    pass


def _object_without_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateJsonKeyError(f'duplicate field "{key}"')
        result[key] = value
    return result


def _json_path(parts: list[Any]) -> str:
    path = "$"
    for part in parts:
        if isinstance(part, int):
            path += f"[{part}]"
        elif isinstance(part, str) and part.isidentifier():
            path += f".{part}"
        else:
            path += f"[{json.dumps(part, ensure_ascii=False)}]"
    return path


@lru_cache(maxsize=None)
def _load_schema_validator(schema_file_name: str):
    if Path(schema_file_name).name != schema_file_name:
        raise ValueError(f'Invalid descriptor schema name "{schema_file_name}".')
    schema_path = SCHEMA_DIR / schema_file_name
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    validator_type = validators.validator_for(schema)
    validator_type.check_schema(schema)
    return validator_type(schema)


def load_json_descriptor(file_path: Path, schema_file_name: str) -> Dict[str, Any]:
    try:
        text = file_path.read_text(encoding="utf-8")
        data = json.loads(text, object_pairs_hook=_object_without_duplicate_keys)
    except FileNotFoundError as exc:
        raise FileNotFoundError(f'Descriptor file "{file_path}" does not exist.') from exc
    except json.JSONDecodeError as exc:
        raise ValueError(
            f'Descriptor "{file_path}" contains malformed JSON at '
            f"line {exc.lineno}, column {exc.colno}: {exc.msg}."
        ) from exc
    except _DuplicateJsonKeyError as exc:
        raise ValueError(f'Descriptor "{file_path}" contains {exc}.') from exc
    except OSError as exc:
        raise ValueError(f'Could not read descriptor "{file_path}": {exc}') from exc

    validator = _load_schema_validator(schema_file_name)
    errors = sorted(
        validator.iter_errors(data),
        key=lambda error: tuple(str(part) for part in error.absolute_path),
    )
    if errors:
        error = errors[0]
        raise ValueError(
            f'Descriptor "{file_path}" is invalid at '
            f"{_json_path(list(error.absolute_path))}: {error.message}."
        )
    return data
    
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
