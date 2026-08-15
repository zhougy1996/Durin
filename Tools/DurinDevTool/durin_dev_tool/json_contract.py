"""Shared JSON decoding and JSON Schema structural validation."""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path
from typing import Any, Iterable

from .errors import DevToolError


class JsonContractError(DevToolError):
    """A JSON document could not be decoded or did not match its schema."""


class _DuplicateJsonKeyError(ValueError):
    pass


def _object_without_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise _DuplicateJsonKeyError(f'duplicate field "{key}"')
        result[key] = value
    return result


def json_path(parts: Iterable[Any]) -> str:
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
def schema_validator(schema_path: Path):
    try:
        from jsonschema import validators
    except ModuleNotFoundError as exc:
        raise JsonContractError(
            "JSON Schema validation requires Durin's prepared Python environment. "
            "Run 'DevTool setup' in the main checkout."
        ) from exc
    resolved = schema_path.resolve()
    try:
        schema = json.loads(
            resolved.read_text(encoding="utf-8"),
            object_pairs_hook=_object_without_duplicate_keys,
        )
        validator_type = validators.validator_for(schema)
        validator_type.check_schema(schema)
    except (OSError, UnicodeError, json.JSONDecodeError, _DuplicateJsonKeyError) as exc:
        raise JsonContractError(f'Could not load JSON schema "{resolved}": {exc}') from exc
    return validator_type(schema)


def decode_json_contract(text: str, *, label: str, source: str) -> Any:
    try:
        return json.loads(text, object_pairs_hook=_object_without_duplicate_keys)
    except json.JSONDecodeError as exc:
        raise JsonContractError(
            f'{label} contains malformed JSON at line {exc.lineno}, column {exc.colno}: {source}'
        ) from exc
    except _DuplicateJsonKeyError as exc:
        raise JsonContractError(f"{label} {source} contains {exc}.") from exc


def validate_json_contract(
    value: Any,
    *,
    label: str,
    source: str,
    schema_path: Path,
) -> Any:
    validator = schema_validator(schema_path)
    errors = sorted(
        validator.iter_errors(value),
        key=lambda error: tuple(str(part) for part in error.absolute_path),
    )
    if errors:
        error = errors[0]
        raise JsonContractError(
            f"{label} {source} is invalid at {json_path(error.absolute_path)}: {error.message}."
        )
    return value


def parse_json_contract(
    text: str,
    *,
    label: str,
    source: str,
    schema_path: Path,
) -> Any:
    value = decode_json_contract(text, label=label, source=source)
    return validate_json_contract(
        value,
        label=label,
        source=source,
        schema_path=schema_path,
    )


def load_json_contract(path: Path, *, label: str, schema_path: Path) -> Any:
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise JsonContractError(f'{label} was not found: "{path}"') from exc
    except UnicodeError as exc:
        raise JsonContractError(f'{label} is not valid UTF-8: "{path}": {exc}') from exc
    except OSError as exc:
        raise JsonContractError(f'Could not read {label.lower()} "{path}": {exc}') from exc
    return parse_json_contract(
        text,
        label=label,
        source=f'"{path}"',
        schema_path=schema_path,
    )
