import json
from functools import lru_cache
from pathlib import Path
from typing import Any

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


def load_json_descriptor(file_path: Path, schema_file_name: str) -> dict[str, Any]:
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
    except UnicodeError as exc:
        raise ValueError(f'Descriptor "{file_path}" is not valid UTF-8: {exc}.') from exc
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


def load_json_file(file_path: Path) -> dict[str, Any]:
    if not file_path.exists():
        raise FileNotFoundError(f"File {file_path} does not exist.")

    with open(file_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    return data
