
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
import math
from pathlib import Path
import re
import struct
from typing import TypeAlias

import clang.cindex

from durin_header_tool import config as configs
from durin_header_tool.model.export_info import ExportedSymbolInfo
from durin_header_tool.model.reflection_info import (
    ReflectedClassInfo,
    ReflectedEnumInfo,
    ReflectedEnumValueInfo,
    ReflectedHeaderInfo,
    ReflectedPropertyInfo,
    ReflectedPropertyMetadataInfo,
    ReflectedStructInfo,
    make_generated_enum_helper_name,
    make_generated_helper_name,
    make_generated_struct_helper_name,
)
from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner
from durin_header_tool.resolver.reflection_resolver import (
    resolve_symbol,
    resolve_symbol_name,
    symbol_resolution_diagnostic,
)

from durin_header_tool.parser.reflection_ast_helpers import (
    _cursor_source_line_range,
    _get_annotation,
    _qualified_name,
)
from durin_header_tool.parser.annotation_rewriter import _annotation_payload, _unescape_string_literal

ExportedSymbols: TypeAlias = dict[str, ExportedSymbolInfo]
MAX_CONTAINER_PROPERTY_DEPTH = 4
_DPROPERTY_PATTERN = re.compile(r"\bDPROPERTY\s*\(")

_PROPERTY_KIND_BY_TYPE = {
    "int8": "Int8",
    "int16": "Int16",
    "int32": "Int32",
    "int64": "Int64",
    "uint8": "UInt8",
    "uint16": "UInt16",
    "uint32": "UInt32",
    "uint64": "UInt64",
    "float": "Float",
    "double": "Double",
    "bool": "Bool",
    "std::string": "String",
    "FName": "Name",
    "Durin::FName": "Name",
    "FGuid": "Guid",
    "Durin::FGuid": "Guid",
}

_PROPERTY_FLAG_BY_SPECIFIER = {
    "Edit": "Edit",
    "Transient": "Transient",
    "ReadOnly": "ReadOnly",
}

_TYPED_METADATA_KEYS = {
    "DisplayName", "ToolTip", "Category", "Units", "Step", "Precision",
    "ClampMin", "ClampMax", "UIMin", "UIMax",
}
_NUMERIC_METADATA_KEYS = {"Units", "Step", "Precision", "ClampMin", "ClampMax", "UIMin", "UIMax"}
_VALID_UNITS = {
    "Unitless", "Percent", "Degrees", "Radians", "Seconds", "Milliseconds",
    "Meters", "Centimeters", "Millimeters", "Kilometers",
}
_INTEGER_RANGES = {
    "Int8": (-(1 << 7), (1 << 7) - 1), "Int16": (-(1 << 15), (1 << 15) - 1),
    "Int32": (-(1 << 31), (1 << 31) - 1), "Int64": (-(1 << 63), (1 << 63) - 1),
    "UInt8": (0, (1 << 8) - 1), "UInt16": (0, (1 << 16) - 1),
    "UInt32": (0, (1 << 32) - 1), "UInt64": (0, (1 << 64) - 1),
}

def _normalize_type_spelling(type_spelling: str) -> str:
    return (
        type_spelling
        .replace("class ", "")
        .replace("struct ", "")
        .replace("enum ", "")
        .replace("const ", "")
        .replace(" &", "&")
        .replace(" *", "*")
        .strip()
    )


def _annotation_entries(annotation: str) -> list[str]:
    _, separator, payload = annotation.partition(",")
    if not separator:
        return []
    payload = _unescape_string_literal(payload)
    return CppSourceScanner(payload).split_macro_arguments()


def _property_flags_from_annotation(annotation: str) -> str:
    flags: list[str] = []
    for raw_specifier in _annotation_entries(annotation):
        specifier = raw_specifier.strip()
        if "=" in specifier:
            specifier = specifier.split("=", 1)[0].strip()
        flag = _PROPERTY_FLAG_BY_SPECIFIER.get(specifier)
        if flag:
            flags.append(f"Durin::EPropertyFlags::{flag}")
    return " | ".join(flags) if flags else "None"


def _string_metadata_from_annotation(annotation: str, key: str) -> str:
    for raw_entry in _annotation_entries(annotation):
        entry = raw_entry.strip()
        entry_key, separator, raw_value = entry.partition("=")
        if not separator or entry_key.strip() != key:
            continue
        match = re.fullmatch(r'"((?:\\.|[^"\\])*)"', raw_value.strip())
        if match:
            return _unescape_string_literal(match.group(1))
    return ""


def _string_list_metadata_from_annotation(annotation: str, key: str) -> list[str]:
    for raw_entry in _annotation_entries(annotation):
        entry_key, separator, raw_value = raw_entry.strip().partition("=")
        if not separator or entry_key.strip() != key:
            continue
        match = re.fullmatch(r'"((?:\\.|[^"\\])*)"', raw_value.strip())
        if not match:
            return []
        return [item.strip() for item in _unescape_string_literal(match.group(1)).split(";")]
    return []


def _property_metadata_from_annotation(annotation: str) -> list[tuple[str, str]]:
    payload = _string_metadata_from_annotation(annotation, "MetaData")
    if not payload:
        for raw_entry in _annotation_entries(annotation):
            key, separator, value = raw_entry.strip().partition("=")
            if key.strip() == "MetaData" and separator and re.fullmatch(
                r"[A-Za-z_][A-Za-z0-9_]*", value.strip()
            ):
                payload = value.strip()
                break
    metadata: list[tuple[str, str]] = []
    for entry in payload.split(";"):
        entry = entry.strip()
        if not entry:
            continue
        key, separator, value = entry.partition("=")
        key = key.strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            continue
        metadata.append((key, value.strip() if separator else "true"))
    return metadata


def _quoted_annotation_value(raw_value: str, key: str, location: str) -> str:
    match = re.fullmatch(r'"((?:\\.|[^"\\])*)"', raw_value.strip())
    if not match:
        raise ValueError(f"[DHT-META001] {location}: {key} requires a quoted string value")
    return _unescape_string_literal(match.group(1))


def _metadata_decimal(value: str, key: str, location: str) -> Decimal:
    try:
        parsed = Decimal(value)
    except InvalidOperation as error:
        raise ValueError(f"[DHT-META002] {location}: {key} has invalid numeric value '{value}'") from error
    if not parsed.is_finite():
        raise ValueError(f"[DHT-META003] {location}: {key} must be finite")
    return parsed


def _numeric_metadata_kind(prop: ReflectedPropertyInfo) -> str:
    if prop.kind in _INTEGER_RANGES:
        return "Signed" if prop.kind.startswith("Int") else "Unsigned"
    if prop.kind in ("Float", "Double"):
        return prop.kind
    struct_name = prop.referenced_struct_type.rsplit("::", 1)[-1]
    if struct_name in {"FVector2f", "FVector3f", "FVector4f", "FQuatf"}:
        return "Float"
    if struct_name in {
        "FVector2", "FVector3", "FVector4", "FVector2d", "FVector3d", "FVector4d",
        "FQuat", "FQuatd",
    }:
        return "Double"
    return ""


def _typed_metadata_from_annotation(
    prop: ReflectedPropertyInfo, annotation: str, location: str
) -> ReflectedPropertyMetadataInfo | None:
    values: dict[str, str] = {}
    for raw_entry in _annotation_entries(annotation):
        key, separator, raw_value = raw_entry.strip().partition("=")
        key = key.strip()
        if key not in _TYPED_METADATA_KEYS:
            continue
        if key in values:
            raise ValueError(f"[DHT-META004] {location}: duplicate metadata key '{key}'")
        if not separator:
            raise ValueError(f"[DHT-META001] {location}: {key} requires a value")
        values[key] = raw_value.strip()
    if not values:
        return None
    extension_keys = {key for key, _ in prop.metadata}
    duplicate_extensions = sorted(extension_keys & _TYPED_METADATA_KEYS)
    if duplicate_extensions:
        raise ValueError(
            f"[DHT-META004] {location}: selected metadata key '{duplicate_extensions[0]}' "
            "cannot also be declared through MetaData"
        )

    result = ReflectedPropertyMetadataInfo()
    for key, attribute in (("DisplayName", "display_name"), ("ToolTip", "tooltip"), ("Category", "category")):
        if key in values:
            value = _quoted_annotation_value(values[key], key, location)
            if not value:
                raise ValueError(f"[DHT-META006] {location}: {key} may not be empty")
            setattr(result, attribute, value)

    numeric_keys = set(values) & _NUMERIC_METADATA_KEYS
    if numeric_keys and "Durin::EPropertyFlags::Edit" not in prop.flags:
        raise ValueError(f"[DHT-META005] {location}: numeric metadata requires the Edit property flag")
    numeric_kind = _numeric_metadata_kind(prop)
    if numeric_keys and not numeric_kind:
        raise ValueError(
            f"[DHT-META005] {location}: numeric metadata is not applicable to {prop.kind} property '{prop.name}'"
        )
    result.numeric_kind = numeric_kind
    if "Units" in values:
        result.units = _quoted_annotation_value(values["Units"], "Units", location)
        if result.units not in _VALID_UNITS:
            raise ValueError(f"[DHT-META007] {location}: unknown Units value '{result.units}'")
    if "Precision" in values:
        if not re.fullmatch(r"[0-9]+", values["Precision"]):
            raise ValueError(f"[DHT-META008] {location}: Precision requires a nonnegative integer")
        result.precision = int(values["Precision"])
        limit = 9 if numeric_kind == "Float" else 17 if numeric_kind == "Double" else -1
        if result.precision > limit:
            raise ValueError(f"[DHT-META008] {location}: Precision exceeds the {numeric_kind} limit of {limit}")

    parsed: dict[str, Decimal] = {}
    for key, attribute in (("Step", "step"), ("ClampMin", "clamp_min"), ("ClampMax", "clamp_max"),
                           ("UIMin", "ui_min"), ("UIMax", "ui_max")):
        if key not in values:
            continue
        literal = _quoted_annotation_value(values[key], key, location)
        decimal_value = _metadata_decimal(literal, key, location)
        if numeric_kind in ("Signed", "Unsigned"):
            if decimal_value != decimal_value.to_integral_value():
                raise ValueError(f"[DHT-META009] {location}: {key} is not an integer")
            low, high = _INTEGER_RANGES[prop.kind]
            if decimal_value < low or decimal_value > high:
                raise ValueError(f"[DHT-META010] {location}: {key} is outside the {prop.kind} range")
            literal = str(int(decimal_value))
        elif numeric_kind == "Float":
            try:
                target_value = struct.unpack("f", struct.pack("f", float(decimal_value)))[0]
            except (OverflowError, struct.error):
                target_value = math.inf
            if not math.isfinite(target_value) or (decimal_value != 0 and target_value == 0):
                raise ValueError(f"[DHT-META010] {location}: {key} is outside the float range")
        elif numeric_kind == "Double":
            target_value = float(decimal_value)
            if not math.isfinite(target_value) or (decimal_value != 0 and target_value == 0):
                raise ValueError(f"[DHT-META010] {location}: {key} is outside the double range")
        if key == "Step" and decimal_value <= 0:
            raise ValueError(f"[DHT-META011] {location}: Step must be positive")
        parsed[key] = decimal_value
        setattr(result, attribute, literal)

    if "ClampMin" in parsed and "ClampMax" in parsed and parsed["ClampMin"] > parsed["ClampMax"]:
        raise ValueError(f"[DHT-META012] {location}: ClampMin exceeds ClampMax")
    if "UIMin" in parsed and "UIMax" in parsed and parsed["UIMin"] > parsed["UIMax"]:
        raise ValueError(f"[DHT-META012] {location}: UIMin exceeds UIMax")
    if "UIMin" in parsed and "ClampMin" in parsed and parsed["UIMin"] < parsed["ClampMin"]:
        raise ValueError(f"[DHT-META012] {location}: UIMin is below ClampMin")
    if "UIMax" in parsed and "ClampMax" in parsed and parsed["UIMax"] > parsed["ClampMax"]:
        raise ValueError(f"[DHT-META012] {location}: UIMax exceeds ClampMax")
    return result


def _apply_property_annotation(
    prop: ReflectedPropertyInfo | None, annotation: str, location: str = "DPROPERTY"
) -> ReflectedPropertyInfo | None:
    if prop:
        prop.metadata = _property_metadata_from_annotation(annotation)
        prop.legacy_names = _string_list_metadata_from_annotation(annotation, "LegacyNames")
        prop.typed_metadata = _typed_metadata_from_annotation(prop, annotation, location)
    return prop


def _array_dim(field_cursor: clang.cindex.Cursor) -> int:
    field_type = field_cursor.type
    if field_type.kind == clang.cindex.TypeKind.CONSTANTARRAY:
        return int(field_type.element_count)
    return 1


def _element_type(field_cursor: clang.cindex.Cursor) -> clang.cindex.Type:
    field_type = field_cursor.type
    if field_type.kind == clang.cindex.TypeKind.CONSTANTARRAY:
        return field_type.element_type
    return field_type


def _field_size(type_info: clang.cindex.Type) -> str:
    size = type_info.get_size()
    return str(int(size)) if size and size > 0 else "0"


def _split_template_args(args: str) -> list[str]:
    result: list[str] = []
    depth = 0
    start = 0
    for index, char in enumerate(args):
        if char == "<":
            depth += 1
        elif char == ">":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(args[start:index].strip())
            start = index + 1
    tail = args[start:].strip()
    if tail:
        result.append(tail)
    return result


def _source_template_args(type_spelling: str, template_name: str) -> list[str]:
    compact = type_spelling.strip()
    prefix = f"{template_name}<"
    if not compact.startswith(prefix) or not compact.endswith(">"):
        return []
    return _split_template_args(compact[len(prefix):-1])


def _source_declared_type(source: str, field_cursor: clang.cindex.Cursor) -> str:
    lines = source.splitlines()
    line_index = max(field_cursor.location.line - 1, 0)
    for index in range(line_index, min(line_index + 4, len(lines))):
        line = lines[index].strip()
        if field_cursor.spelling not in line:
            continue
        line = line.rstrip(";").strip()
        # Preserve the declarator while accepting both assignment and brace initialization.
        # Clang canonicalizes Durin math aliases to GLM types, so their source spelling is
        # required to resolve the externally registered FVector/FQuat/FTransform/FLinearColor structs.
        match = re.match(
            rf"(.+?)\s+{re.escape(field_cursor.spelling)}(?:\s*\[[^\]]+\])?(?:\s*(?:=.*|\{{.*\}}))?$",
            line,
        )
        if match:
            return _normalize_type_spelling(match.group(1))
    return ""


def _make_property_from_source_decl(
    source_line: str,
    annotation: str,
    exported_symbols: ExportedSymbols | None,
    declaring_namespace: str = "",
    line_number: int = 0,
) -> ReflectedPropertyInfo | None:
    line = source_line.rstrip(";").strip()
    match = re.match(r"(.+?)\s+(\w+)(?:\s*\[(\d+)\])?(?:\s*(?:=.*|\{.*\}))?$", line)
    if not match:
        return None
    type_spelling = _normalize_type_spelling(match.group(1))
    name = match.group(2)
    array_dim = int(match.group(3)) if match.group(3) else 1
    return _apply_property_annotation(_make_property_from_spelling(
        name,
        type_spelling,
        exported_symbols,
        declaring_namespace=declaring_namespace,
        flags=_property_flags_from_annotation(annotation),
        array_dim=array_dim,
    ), annotation, f"DPROPERTY '{name}' at line {line_number}" if line_number else f"DPROPERTY '{name}'")


def _scan_source_properties_for_class(
    source: str,
    class_cursor: clang.cindex.Cursor,
    exported_symbols: ExportedSymbols | None,
    known_property_names: set[str] | None = None,
    reject_unsupported: bool = True,
    declaring_namespace: str = "",
) -> list[ReflectedPropertyInfo]:
    properties: list[ReflectedPropertyInfo] = []
    start_line, end_line = _cursor_source_line_range(source, class_cursor)
    if start_line == 0:
        return properties

    scanner = CppSourceScanner(source)
    class_start = scanner.position_from_line_column(start_line, class_cursor.extent.start.column)
    opening_brace = scanner.find_next_code_position("{", class_start)
    if opening_brace is None:
        return properties
    closing_brace = scanner.find_matching_brace(opening_brace)
    if closing_brace is None:
        return properties

    in_class = False
    pending_annotation = ""
    lines = source.splitlines()

    for line_number, line in enumerate(
        lines[start_line - 1:end_line],
        start=start_line,
    ):
        line_start = scanner.position_from_line_column(line_number, 1)
        if line_start > closing_brace:
            break
        line_end = scanner.position_from_line_column(line_number + 1, 1) if line_number < scanner.line_count else len(source)
        if not in_class:
            if opening_brace < line_end:
                in_class = True
            continue

        dproperty_match = next(
            (
                match
                for match in _DPROPERTY_PATTERN.finditer(source, line_start, line_end)
                if scanner.is_code_position(match.start())
            ),
            None,
        )
        if dproperty_match:
            closing_parenthesis = scanner.find_matching_parenthesis(dproperty_match.end() - 1)
            if closing_parenthesis is None:
                raise ValueError(f"DPROPERTY at line {line_number}: missing closing ')'")
            pending_annotation = _annotation_payload(
                "DPROPERTY", source[dproperty_match.end():closing_parenthesis]
            )
            continue

        stripped = line.strip()
        if pending_annotation:
            if not stripped or stripped in ("public:", "private:", "protected:"):
                continue
            if scanner.find_next_code_position(";", line_start, line_end) is not None:
                prop = _make_property_from_source_decl(
                    stripped, pending_annotation, exported_symbols, declaring_namespace, line_number
                )
                if prop:
                    properties.append(prop)
                else:
                    declaration = re.match(r"(.+?)\s+(\w+)(?:\s*\[\d+\])?(?:\s*(?:=.*|\{.*\}))?;", stripped)
                    property_name = declaration.group(2) if declaration else ""
                    type_spelling = declaration.group(1).strip() if declaration else stripped
                    if _is_std_vector(type_spelling) or _is_std_unordered_map(type_spelling):
                        _validate_explicit_container_spelling(type_spelling, property_name or "<unknown>", line_number)
                    if reject_unsupported and (
                        not property_name or property_name not in (known_property_names or set())
                    ):
                        if exported_symbols:
                            for reference_spelling in _reflected_reference_spellings(type_spelling):
                                resolution = resolve_symbol(
                                    reference_spelling, exported_symbols,
                                    declaring_namespace=declaring_namespace,
                                )
                                if resolution.candidates and not resolution.resolved:
                                    raise ValueError(symbol_resolution_diagnostic(
                                        resolution,
                                        str(class_cursor.location.file or "<unknown>"),
                                        f"DPROPERTY '{property_name or '<unknown>'}' at line {line_number}",
                                    ))
                        raise ValueError(
                            f"DPROPERTY at line {line_number}: unsupported non-hermetic type "
                            f"spelling '{type_spelling}'"
                        )
                pending_annotation = ""

    return properties


def _reflected_reference_spellings(type_spelling: str) -> tuple[str, ...]:
    normalized = _normalize_type_spelling(type_spelling)
    if normalized.endswith("*"):
        return _reflected_reference_spellings(normalized[:-1])
    for predicate, argument_getter in (
        (_is_tobject_ptr, _tobject_ptr_arg),
        (_is_tsoft_object_ptr, _tsoft_object_ptr_arg),
    ):
        if predicate(normalized):
            argument = argument_getter(normalized)
            return _reflected_reference_spellings(argument) if argument else ()
    if _is_std_vector(normalized):
        arguments = _source_template_args(normalized.removeprefix("::"), "std::vector")
        return _reflected_reference_spellings(arguments[0]) if arguments else ()
    if _is_std_unordered_map(normalized):
        arguments = _source_template_args(normalized.removeprefix("::"), "std::unordered_map")
        return tuple(
            spelling
            for argument in arguments[:2]
            for spelling in _reflected_reference_spellings(argument)
        )
    if normalized in _PROPERTY_KIND_BY_TYPE or normalized in (
        "bool", "float", "double", "std::string", "FName", "Durin::FName",
        "FGuid", "Durin::FGuid",
    ):
        return ()
    return (normalized,)


def _is_std_vector(type_spelling: str) -> bool:
    normalized = type_spelling.replace(" ", "").removeprefix("::")
    return normalized.startswith("std::vector<")


def _is_std_unordered_map(type_spelling: str) -> bool:
    normalized = type_spelling.replace(" ", "").removeprefix("::")
    return normalized.startswith("std::unordered_map<")


def _validate_explicit_container_spelling(
    type_spelling: str,
    property_name: str,
    line_number: int,
    *,
    depth: int = 0,
) -> None:
    location = f"DPROPERTY '{property_name}' at line {line_number}"
    if _is_std_vector(type_spelling):
        if depth >= MAX_CONTAINER_PROPERTY_DEPTH:
            raise ValueError(
                f"[DHT-CONT005] {location}: container nesting exceeds the supported "
                f"depth of {MAX_CONTAINER_PROPERTY_DEPTH}"
            )
        args = _source_template_args(type_spelling.removeprefix("::"), "std::vector")
        if len(args) != 1:
            raise ValueError(
                f"[DHT-CONT001] {location}: std::vector requires the default "
                "allocator form std::vector<T>"
            )
        if _normalize_type_spelling(args[0]) == "bool":
            raise ValueError(
                f"[DHT-CONT002] {location}: std::vector<bool> proxy references "
                "are unsupported"
            )
        if _is_std_vector(args[0]) or _is_std_unordered_map(args[0]):
            _validate_explicit_container_spelling(args[0], property_name, line_number, depth=depth + 1)
        return
    if _is_std_unordered_map(type_spelling):
        if depth >= MAX_CONTAINER_PROPERTY_DEPTH:
            raise ValueError(
                f"[DHT-CONT005] {location}: container nesting exceeds the supported "
                f"depth of {MAX_CONTAINER_PROPERTY_DEPTH}"
            )
        args = _source_template_args(type_spelling.removeprefix("::"), "std::unordered_map")
        if len(args) != 2:
            raise ValueError(
                f"[DHT-CONT003] {location}: std::unordered_map requires the default "
                "hash, equality, and allocator form std::unordered_map<K, V>"
            )
        if (
            _is_std_vector(args[0])
            or _is_std_unordered_map(args[0])
            or args[0].rstrip().endswith("*")
            or _is_tobject_ptr(args[0])
        ):
            key_type = _normalize_type_spelling(args[0])
            raise ValueError(
                f"[DHT-CONT004] {location}: Map key type '{key_type}' is unsupported"
            )
        if _is_std_vector(args[1]) or _is_std_unordered_map(args[1]):
            _validate_explicit_container_spelling(args[1], property_name, line_number, depth=depth + 1)


def _is_tobject_ptr(type_spelling: str) -> bool:
    normalized = type_spelling.replace(" ", "")
    return normalized.startswith("TObjectPtr<") or normalized.startswith("Durin::TObjectPtr<")


def _tobject_ptr_arg(type_spelling: str) -> str:
    normalized = type_spelling.strip()
    if normalized.replace(" ", "").startswith("Durin::TObjectPtr<"):
        return _source_template_args(normalized, "Durin::TObjectPtr")[0] if _source_template_args(normalized, "Durin::TObjectPtr") else ""
    return _source_template_args(normalized, "TObjectPtr")[0] if _source_template_args(normalized, "TObjectPtr") else ""


def _is_tsoft_object_ptr(type_spelling: str) -> bool:
    normalized = type_spelling.replace(" ", "").removeprefix("::")
    return normalized.startswith("TSoftObjectPtr<") or normalized.startswith("Durin::TSoftObjectPtr<")


def _tsoft_object_ptr_arg(type_spelling: str) -> str:
    normalized = type_spelling.strip().removeprefix("::")
    template_name = (
        "Durin::TSoftObjectPtr"
        if normalized.replace(" ", "").startswith("Durin::TSoftObjectPtr<")
        else "TSoftObjectPtr"
    )
    args = _source_template_args(normalized, template_name)
    return args[0].strip() if len(args) == 1 else ""


def _contains_soft_object_spelling(type_spelling: str) -> bool:
    compact = type_spelling.replace(" ", "")
    return "TSoftObjectPtr" in compact or "FSoftObjectPtr" in compact


def _validate_soft_object_spelling(
    type_spelling: str,
    property_name: str,
    line_number: int,
    exported_symbols: ExportedSymbols | None,
    declaring_namespace: str = "",
) -> None:
    location = f"DPROPERTY '{property_name}' at line {line_number}"
    source_compact = type_spelling.strip().replace(" ", "").removeprefix("::")
    normalized = _normalize_type_spelling(type_spelling)
    compact = normalized.replace(" ", "").removeprefix("::")

    if "FSoftObjectPtr" in compact:
        raise ValueError(
            f"[DHT-SOFT001] {location}: raw FSoftObjectPtr is unsupported; "
            "use TSoftObjectPtr<ReflectedObjectClass>"
        )
    if compact in ("TSoftObjectPtr", "Durin::TSoftObjectPtr"):
        raise ValueError(
            f"[DHT-SOFT001] {location}: TSoftObjectPtr requires exactly one reflected object class"
        )
    if "TSoftObjectPtr" in compact and (
        source_compact.startswith("const")
        or source_compact.startswith("volatile")
        or source_compact.endswith("*")
        or source_compact.endswith("&")
        or source_compact.endswith("&&")
        or source_compact.endswith("const")
        or source_compact.endswith("volatile")
    ):
        raise ValueError(
            f"[DHT-SOFT003] {location}: soft object properties do not support "
            "cv-qualifiers, pointers, or references"
        )
    if _is_tsoft_object_ptr(type_spelling):
        target = _tsoft_object_ptr_arg(type_spelling)
        if not target:
            raise ValueError(
                f"[DHT-SOFT001] {location}: TSoftObjectPtr requires exactly one reflected object class"
            )
        if re.search(r"\b(?:const|volatile)\b", target) \
            or target.endswith("*") or target.endswith("&"):
            raise ValueError(
                f"[DHT-SOFT003] {location}: soft object target '{target}' must be an unqualified object class"
            )
        if exported_symbols:
            resolved_class = _resolved_symbol_name(
                (target,), exported_symbols, kinds=("class",),
                declaring_namespace=declaring_namespace,
            )
            if not resolved_class:
                resolved_non_object = _resolved_symbol_name(
                    (target,), exported_symbols, kinds=("struct", "enum"),
                    declaring_namespace=declaring_namespace,
                )
                code = "DHT-SOFT004" if resolved_non_object else "DHT-SOFT005"
                reason = "is not an object class" if resolved_non_object else "could not be resolved"
                raise ValueError(
                    f"[{code}] {location}: soft object target '{target}' {reason}"
                )
        return
    if _is_std_vector(normalized):
        args = _source_template_args(normalized.removeprefix("::"), "std::vector")
        if args:
            _validate_soft_object_spelling(
                args[0], property_name, line_number, exported_symbols, declaring_namespace
            )
        return
    if _is_std_unordered_map(normalized):
        args = _source_template_args(normalized.removeprefix("::"), "std::unordered_map")
        if len(args) >= 2:
            if _contains_soft_object_spelling(args[0]):
                raise ValueError(
                    f"[DHT-SOFT006] {location}: soft object references are unsupported as Map keys"
                )
            _validate_soft_object_spelling(
                args[1], property_name, line_number, exported_symbols, declaring_namespace
            )
        return
    if "TSoftObjectPtr" in compact:
        raise ValueError(
            f"[DHT-SOFT002] {location}: unsupported soft object declaration '{normalized}'; "
            "spell TSoftObjectPtr<T> directly"
        )


def _soft_object_alias_names(source: str) -> set[str]:
    aliases = {
        match.group(1)
        for match in re.finditer(
            r"\busing\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
            r"(?:::)?(?:Durin::)?TSoftObjectPtr\s*<[^;]+>;",
            source,
        )
    }
    aliases.update(
        match.group(1)
        for match in re.finditer(
            r"\btypedef\s+(?:::)?(?:Durin::)?TSoftObjectPtr\s*<[^;]+>\s+"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*;",
            source,
        )
    )
    return aliases


def _resolved_symbol_name(
    spellings: tuple[str, ...],
    exported_symbols: ExportedSymbols | None,
    *,
    kinds: tuple[str, ...],
    declaring_namespace: str = "",
) -> str | None:
    if not exported_symbols:
        return None
    ordered_spellings = (
        *(spelling for spelling in spellings if "::" in spelling),
        *(spelling for spelling in spellings if "::" not in spelling),
    )
    for spelling in dict.fromkeys(ordered_spellings):
        if resolved := resolve_symbol_name(
            spelling, exported_symbols,
            declaring_namespace=declaring_namespace, kinds=kinds,
        ):
            return resolved
    return None


def _cpp_type_spelling(
    type_spelling: str,
    exported_symbols: ExportedSymbols | None,
    declaring_namespace: str = "",
) -> str:
    type_spelling = _normalize_type_spelling(type_spelling)
    primitive_types = {
        "int8": "Durin::int8",
        "int16": "Durin::int16",
        "int32": "Durin::int32",
        "int64": "Durin::int64",
        "uint8": "Durin::uint8",
        "uint16": "Durin::uint16",
        "uint32": "Durin::uint32",
        "uint64": "Durin::uint64",
        "bool": "bool",
        "float": "float",
        "double": "double",
        "std::string": "std::string",
        "FGuid": "Durin::FGuid",
        "Durin::FGuid": "Durin::FGuid",
    }
    if type_spelling in primitive_types:
        return primitive_types[type_spelling]
    if _is_std_vector(type_spelling):
        args = _source_template_args(type_spelling, "std::vector")
        if len(args) == 1:
            return f"std::vector<{_cpp_type_spelling(args[0], exported_symbols, declaring_namespace)}>"
    if _is_std_unordered_map(type_spelling):
        args = _source_template_args(type_spelling, "std::unordered_map")
        if len(args) >= 2:
            return f"std::unordered_map<{_cpp_type_spelling(args[0], exported_symbols, declaring_namespace)}, {_cpp_type_spelling(args[1], exported_symbols, declaring_namespace)}>"
    if _is_tobject_ptr(type_spelling):
        arg = _tobject_ptr_arg(type_spelling)
        if arg:
            return f"Durin::TObjectPtr<{_cpp_type_spelling(arg, exported_symbols, declaring_namespace)}>"
    if _is_tsoft_object_ptr(type_spelling):
        arg = _tsoft_object_ptr_arg(type_spelling)
        if arg:
            return f"Durin::TSoftObjectPtr<{_cpp_type_spelling(arg, exported_symbols, declaring_namespace)}>"
    if resolved := _resolved_symbol_name(
        (type_spelling,),
        exported_symbols,
        kinds=("enum", "class", "struct"),
        declaring_namespace=declaring_namespace,
    ):
        return resolved
    if type_spelling.endswith("*"):
        pointee = type_spelling[:-1].strip()
        if not exported_symbols:
            return f"{pointee}*"
        if resolved := _resolved_symbol_name(
            (pointee,),
            exported_symbols,
            kinds=("class",),
            declaring_namespace=declaring_namespace,
        ):
            return f"{resolved}*"
    return type_spelling


def _make_property_from_spelling(
    name: str,
    type_spelling: str,
    exported_symbols: ExportedSymbols | None,
    flags: str = "None",
    array_dim: int = 1,
    allow_object: bool = True,
    allow_container: bool = True,
    allow_enum: bool = True,
    depth: int = 0,
    max_depth: int = MAX_CONTAINER_PROPERTY_DEPTH,
    canonical_spelling: str = "",
    element_size: str = "",
    enum_qualified_name: str = "",
    enum_short_name: str = "",
    declaring_namespace: str = "",
) -> ReflectedPropertyInfo | None:
    type_spelling = _normalize_type_spelling(type_spelling)
    canonical_spelling = _normalize_type_spelling(canonical_spelling)
    kind = (
        _PROPERTY_KIND_BY_TYPE.get(type_spelling)
        or _PROPERTY_KIND_BY_TYPE.get(canonical_spelling)
    )
    if not kind and (
        type_spelling == "std::string"
        or canonical_spelling.startswith("std::basic_string<")
    ):
        kind = "String"

    if kind:
        size_by_kind = {
            "Bool": "sizeof(bool)",
            "Int8": "sizeof(Durin::int8)",
            "Int16": "sizeof(Durin::int16)",
            "Int32": "sizeof(Durin::int32)",
            "Int64": "sizeof(Durin::int64)",
            "UInt8": "sizeof(Durin::uint8)",
            "UInt16": "sizeof(Durin::uint16)",
            "UInt32": "sizeof(Durin::uint32)",
            "UInt64": "sizeof(Durin::uint64)",
            "Float": "sizeof(float)",
            "Double": "sizeof(double)",
            "String": "sizeof(std::string)",
            "Name": "sizeof(Durin::FName)",
            "Guid": "sizeof(Durin::FGuid)",
        }
        return ReflectedPropertyInfo(
            name=name,
            type_name=type_spelling,
            kind=kind,
            array_dim=array_dim,
            element_size=element_size or size_by_kind[kind],
            flags=flags,
        )

    if referenced_struct_type := _resolved_symbol_name(
        (type_spelling, canonical_spelling),
        exported_symbols,
        kinds=("struct",),
        declaring_namespace=declaring_namespace,
    ):
        return ReflectedPropertyInfo(
            name=name,
            type_name=type_spelling,
            kind="Struct",
            referenced_struct_type=referenced_struct_type,
            array_dim=array_dim,
            element_size=element_size or f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols, declaring_namespace)})",
            flags=flags,
        )

    if allow_enum:
        enum_spellings = (
            enum_qualified_name,
            canonical_spelling,
            type_spelling,
            enum_short_name,
        )
        referenced_enum_type = _resolved_symbol_name(
            enum_spellings,
            exported_symbols,
            kinds=("enum",),
            declaring_namespace=declaring_namespace,
        )
        if not exported_symbols and enum_qualified_name:
            referenced_enum_type = enum_qualified_name
        if referenced_enum_type:
            symbol = (exported_symbols or {}).get(referenced_enum_type)
            underlying_size = symbol.UnderlyingSize if symbol is not None else 0
            return ReflectedPropertyInfo(
                name=name,
                type_name=type_spelling,
                kind="Enum",
                referenced_enum_type=referenced_enum_type,
                array_dim=array_dim,
                element_size=element_size or (
                    str(underlying_size)
                    if underlying_size
                    else f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols, declaring_namespace)})"
                ),
                flags=flags,
            )

    vector_spelling = type_spelling if _is_std_vector(type_spelling) else canonical_spelling
    if allow_container and _is_std_vector(vector_spelling):
        if depth >= max_depth:
            return None
        args = _source_template_args(vector_spelling.removeprefix("::"), "std::vector")
        if len(args) < 1 or (_is_std_vector(type_spelling) and len(args) != 1):
            return None
        if _normalize_type_spelling(args[0]) == "bool":
            return None
        inner = _make_property_from_spelling(
            f"{name}_Inner",
            args[0],
            exported_symbols,
            allow_object=True,
            allow_container=True,
            allow_enum=True,
            depth=depth + 1,
            max_depth=max_depth,
            declaring_namespace=declaring_namespace,
        )
        if not inner:
            return None
        return ReflectedPropertyInfo(
            name=name,
            type_name=type_spelling,
            kind="Array",
            array_dim=array_dim,
            element_size=element_size or f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols, declaring_namespace)})",
            flags=flags,
            inner=inner,
        )

    map_spelling = type_spelling if _is_std_unordered_map(type_spelling) else canonical_spelling
    if allow_container and _is_std_unordered_map(map_spelling):
        if depth >= max_depth:
            return None
        args = _source_template_args(map_spelling.removeprefix("::"), "std::unordered_map")
        if len(args) < 2 or (_is_std_unordered_map(type_spelling) and len(args) != 2):
            return None
        key = _make_property_from_spelling(
            f"{name}_Key",
            args[0],
            exported_symbols,
            allow_object=False,
            allow_container=False,
            allow_enum=True,
            depth=depth + 1,
            max_depth=max_depth,
            declaring_namespace=declaring_namespace,
        )
        value = _make_property_from_spelling(
            f"{name}_Value",
            args[1],
            exported_symbols,
            allow_object=True,
            allow_container=True,
            allow_enum=True,
            depth=depth + 1,
            max_depth=max_depth,
            declaring_namespace=declaring_namespace,
        )
        if not key or not value:
            return None
        return ReflectedPropertyInfo(
            name=name,
            type_name=type_spelling,
            kind="Map",
            array_dim=array_dim,
            element_size=element_size or f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols, declaring_namespace)})",
            flags=flags,
            key=key,
            value=value,
        )

    if allow_object and type_spelling.endswith("*"):
        pointee = type_spelling[:-1].strip()
        referenced_type = (
            pointee
            if not exported_symbols
            else _resolved_symbol_name(
                (pointee,), exported_symbols, kinds=("class",),
                declaring_namespace=declaring_namespace,
            )
        )
        if referenced_type:
            return ReflectedPropertyInfo(
                name=name,
                type_name=type_spelling,
                kind="Object",
                referenced_type=referenced_type,
                array_dim=array_dim,
                element_size=element_size or "sizeof(Durin::DObject*)",
                flags=flags,
            )
    if allow_object and _is_tobject_ptr(type_spelling):
        pointee = _tobject_ptr_arg(type_spelling)
        if not pointee:
            return None
        referenced_type = (
            pointee
            if not exported_symbols
            else _resolved_symbol_name(
                (pointee,), exported_symbols, kinds=("class",),
                declaring_namespace=declaring_namespace,
            )
        )
        if referenced_type:
            return ReflectedPropertyInfo(
                name=name,
                type_name=type_spelling,
                kind="Object",
                referenced_type=referenced_type,
                array_dim=array_dim,
                element_size=f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols, declaring_namespace)})",
                flags=flags,
                is_object_ptr_wrapper=True,
            )
    if allow_object and _is_tsoft_object_ptr(type_spelling):
        pointee = _tsoft_object_ptr_arg(type_spelling)
        if not pointee:
            return None
        referenced_type = (
            pointee
            if not exported_symbols
            else _resolved_symbol_name(
                (pointee,), exported_symbols, kinds=("class",),
                declaring_namespace=declaring_namespace,
            )
        )
        if referenced_type:
            return ReflectedPropertyInfo(
                name=name,
                type_name=type_spelling,
                kind="SoftObject",
                referenced_type=referenced_type,
                array_dim=array_dim,
                element_size=element_size or f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols, declaring_namespace)})",
                flags=flags,
            )
    return None


def _underlying_kind_from_type_spelling(type_spelling: str) -> str:
    normalized = _normalize_type_spelling(type_spelling)
    mapping = {
        "signed char": "Int8",
        "char": "Int8",
        "short": "Int16",
        "short int": "Int16",
        "int": "Int32",
        "long": "Int32",
        "long int": "Int32",
        "long long": "Int64",
        "long long int": "Int64",
        "int8": "Int8",
        "int16": "Int16",
        "int32": "Int32",
        "int64": "Int64",
        "Durin::int8": "Int8",
        "Durin::int16": "Int16",
        "Durin::int32": "Int32",
        "Durin::int64": "Int64",
        "unsigned char": "UInt8",
        "unsigned short": "UInt16",
        "unsigned short int": "UInt16",
        "unsigned int": "UInt32",
        "unsigned long": "UInt32",
        "unsigned long int": "UInt32",
        "unsigned long long": "UInt64",
        "unsigned long long int": "UInt64",
        "uint8": "UInt8",
        "uint16": "UInt16",
        "uint32": "UInt32",
        "uint64": "UInt64",
        "Durin::uint8": "UInt8",
        "Durin::uint16": "UInt16",
        "Durin::uint32": "UInt32",
        "Durin::uint64": "UInt64",
    }
    return mapping.get(normalized, "Unknown")


def _make_property_from_type(
    name: str,
    type_info: clang.cindex.Type,
    exported_symbols: ExportedSymbols | None,
    flags: str = "None",
    array_dim: int = 1,
    allow_object: bool = True,
    allow_container: bool = True,
    allow_enum: bool = True,
    depth: int = 0,
    max_depth: int = MAX_CONTAINER_PROPERTY_DEPTH,
    declaring_namespace: str = "",
) -> ReflectedPropertyInfo | None:
    type_spelling = _normalize_type_spelling(type_info.spelling)
    canonical = _normalize_type_spelling(type_info.get_canonical().spelling)
    enum_qualified_name = ""
    enum_short_name = ""
    declaration = type_info.get_declaration()
    if declaration.kind == clang.cindex.CursorKind.ENUM_DECL and declaration.spelling:
        enum_qualified_name = _qualified_name(declaration)
        enum_short_name = declaration.spelling
    return _make_property_from_spelling(
        name=name,
        type_spelling=type_spelling,
        exported_symbols=exported_symbols,
        flags=flags,
        array_dim=array_dim,
        allow_object=allow_object,
        allow_container=allow_container,
        allow_enum=allow_enum,
        depth=depth,
        max_depth=max_depth,
        canonical_spelling=canonical,
        element_size=_field_size(type_info),
        enum_qualified_name=enum_qualified_name,
        enum_short_name=enum_short_name,
        declaring_namespace=declaring_namespace,
    )


def _property_reflected_identity(prop: ReflectedPropertyInfo) -> tuple | None:
    if not prop:
        return None
    return (
        prop.kind,
        prop.referenced_type,
        prop.referenced_enum_type,
        prop.referenced_struct_type,
        _property_reflected_identity(prop.inner),
        _property_reflected_identity(prop.key),
        _property_reflected_identity(prop.value),
    )


def _require_ast_source_agreement(
    field_cursor: clang.cindex.Cursor,
    source_prop: ReflectedPropertyInfo | None,
    ast_prop: ReflectedPropertyInfo | None,
) -> None:
    source_identity = _property_reflected_identity(source_prop)
    ast_identity = _property_reflected_identity(ast_prop)
    if source_identity and ast_identity and source_identity != ast_identity:
        raise ValueError(
            f"DPROPERTY '{field_cursor.spelling}' at line {field_cursor.location.line}: "
            f"AST/source reflected type disagreement: source={source_identity}, AST={ast_identity}"
        )


def _make_property(
    field_cursor: clang.cindex.Cursor,
    exported_symbols: ExportedSymbols | None,
    source: str,
    declaring_namespace: str = "",
) -> ReflectedPropertyInfo | None:
    annotation = _get_annotation(field_cursor)
    if not annotation.startswith("DPROPERTY"):
        return None

    source_type = _source_declared_type(source, field_cursor)
    if source_type:
        if any(
            re.search(rf"\b{re.escape(alias)}\b", source_type)
            for alias in _soft_object_alias_names(source)
        ):
            raise ValueError(
                f"[DHT-SOFT002] DPROPERTY '{field_cursor.spelling}' at line "
                f"{field_cursor.location.line}: aliases of TSoftObjectPtr<T> are unsupported; "
                "spell the template directly"
            )
        _validate_soft_object_spelling(
            source_type, field_cursor.spelling, field_cursor.location.line,
            exported_symbols, declaring_namespace
        )
    if source_type and (_is_std_vector(source_type) or _is_std_unordered_map(source_type)):
        _validate_explicit_container_spelling(source_type, field_cursor.spelling, field_cursor.location.line)
        source_prop = _make_property_from_spelling(
            field_cursor.spelling,
            source_type,
            exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor),
            declaring_namespace=declaring_namespace,
        )
        ast_prop = _make_property_from_type(
            field_cursor.spelling, _element_type(field_cursor), exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor), declaring_namespace=declaring_namespace,
        )
        _require_ast_source_agreement(field_cursor, source_prop, ast_prop)
        return _apply_property_annotation(source_prop, annotation,
            f"DPROPERTY '{field_cursor.spelling}' at line {field_cursor.location.line}")

    source_prop = None
    if source_type:
        source_prop = _make_property_from_spelling(
            field_cursor.spelling,
            source_type,
            exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor),
            declaring_namespace=declaring_namespace,
        )
        if source_prop and source_prop.kind in ("Struct", "String", "Name", "Guid"):
            ast_prop = _make_property_from_type(
                field_cursor.spelling, _element_type(field_cursor), exported_symbols,
                flags=_property_flags_from_annotation(annotation),
                array_dim=_array_dim(field_cursor), declaring_namespace=declaring_namespace,
            )
            _require_ast_source_agreement(field_cursor, source_prop, ast_prop)
            # Non-fundamental layout belongs to the target compiler, not the
            # synthetic libclang context. Keep it as a C++ sizeof expression.
            return _apply_property_annotation(source_prop, annotation,
                f"DPROPERTY '{field_cursor.spelling}' at line {field_cursor.location.line}")

    prop = _make_property_from_type(
        field_cursor.spelling,
        _element_type(field_cursor),
        exported_symbols,
        flags=_property_flags_from_annotation(annotation),
        array_dim=_array_dim(field_cursor),
        declaring_namespace=declaring_namespace,
    )
    if prop:
        _require_ast_source_agreement(field_cursor, source_prop, prop)
        if prop.kind == "SoftObject" and source_type and not _is_tsoft_object_ptr(source_type):
            raise ValueError(
                f"[DHT-SOFT002] DPROPERTY '{field_cursor.spelling}' at line "
                f"{field_cursor.location.line}: aliases of TSoftObjectPtr<T> are unsupported; "
                "spell the template directly"
            )
        return _apply_property_annotation(prop, annotation,
            f"DPROPERTY '{field_cursor.spelling}' at line {field_cursor.location.line}")

    if source_type:
        return _apply_property_annotation(_make_property_from_spelling(
            field_cursor.spelling,
            source_type,
            exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor),
            declaring_namespace=declaring_namespace,
        ), annotation, f"DPROPERTY '{field_cursor.spelling}' at line {field_cursor.location.line}")
    return None
