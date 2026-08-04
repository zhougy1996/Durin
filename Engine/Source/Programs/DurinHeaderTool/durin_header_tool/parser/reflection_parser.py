from dataclasses import dataclass
from pathlib import Path
import re
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
    ReflectedStructInfo,
    make_generated_enum_helper_name,
    make_generated_helper_name,
    make_generated_struct_helper_name,
)
from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner
from durin_header_tool.resolver.reflection_resolver import resolve_symbol_name

ExportedSymbols: TypeAlias = dict[str, ExportedSymbolInfo]
MAX_CONTAINER_PROPERTY_DEPTH = 4

@dataclass(frozen=True)
class _DMetaUse:
    line: int
    column: int


_DPROPERTY_PATTERN = re.compile(r"\bDPROPERTY\s*\(")
_GENERATED_BODY_PATTERN = re.compile(r"\bGENERATED_BODY\s*\(")
_INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\b[^\r\n]*$', re.MULTILINE)
_TYPE_DECLARATION_PATTERN = re.compile(r"\b(?:class|struct|enum(?:\s+class)?)\s+([A-Za-z_]\w*)")
PARSER_CONTEXT_VERSION = "hermetic-v1"

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


def _annotation_payload(prefix: str, payload: str) -> str:
    payload = payload.strip().replace("\\", "\\\\").replace('"', '\\"')
    return f'{prefix},{payload}' if payload else prefix


def _macro_arguments(payload: str, location: str) -> list[str]:
    try:
        return CppSourceScanner(payload).split_macro_arguments()
    except ValueError as error:
        raise ValueError(f"{location}: {error}") from None


def _unescape_string_literal(value: str) -> str:
    return CppSourceScanner.unescape_string_literal(value)


def _display_name_from_payload(payload: str, macro_name: str, line: int, column: int) -> str:
    location = f"{macro_name} at line {line}, column {column}"
    if not payload.strip():
        return ""

    entries = _macro_arguments(payload, location)

    display_name = ""
    seen_display_name = False
    for raw_entry in entries:
        entry = raw_entry.strip()
        if not entry:
            raise ValueError(f"{location}: empty metadata entry")
        key, separator, raw_value = entry.partition("=")
        key = key.strip()
        if not separator:
            raise ValueError(f"{location}: metadata '{key}' requires = \"...\"")
        if key != "DisplayName":
            raise ValueError(f"{location}: unsupported metadata key '{key}'")
        if seen_display_name:
            raise ValueError(f"{location}: duplicate DisplayName metadata")
        seen_display_name = True

        raw_value = raw_value.strip()
        match = re.fullmatch(r'"((?:\\.|[^"\\])*)"', raw_value)
        if not match:
            raise ValueError(f"{location}: DisplayName requires a quoted string")
        display_name = _unescape_string_literal(match.group(1))
    return display_name


def _class_specifiers_from_payload(
    payload: str, line: int, column: int
) -> tuple[bool, str, str]:
    location = f"DCLASS at line {line}, column {column}"
    if not payload.strip():
        return False, "", ""

    entries = _macro_arguments(payload, location)

    is_abstract = False
    metadata: dict[str, str] = {}
    for raw_entry in entries:
        entry = raw_entry.strip()
        if not entry:
            raise ValueError(f"{location}: empty class specifier")
        key, separator, raw_value = entry.partition("=")
        key = key.strip()
        if not separator:
            if key != "Abstract":
                raise ValueError(f"{location}: unsupported class specifier '{key}'")
            if is_abstract:
                raise ValueError(f"{location}: duplicate Abstract class specifier")
            is_abstract = True
            continue

        if key not in ("DisplayName", "DefaultObjectName"):
            raise ValueError(f"{location}: unsupported class metadata key '{key}'")
        if key in metadata:
            raise ValueError(f"{location}: duplicate {key} class metadata")
        raw_value = raw_value.strip()
        match = re.fullmatch(r'"((?:\\.|[^"\\])*)"', raw_value)
        if not match:
            raise ValueError(f"{location}: {key} requires a quoted string")
        metadata[key] = _unescape_string_literal(match.group(1))

    return is_abstract, metadata.get("DisplayName", ""), metadata.get("DefaultObjectName", "")


def _replace_macro_calls(source: str, macro_name: str, replacement) -> str:
    pattern = re.compile(rf"\b{re.escape(macro_name)}\s*\(")
    scanner = CppSourceScanner(source)
    search_from = 0
    pieces: list[str] = []
    output_from = 0
    while match := pattern.search(source, search_from):
        if not scanner.is_code_position(match.start()):
            search_from = match.end()
            continue
        line, column = scanner.line_column(match.start())
        line_start = match.start() - column + 1
        if source[line_start:match.start()].lstrip().startswith("#"):
            search_from = match.end()
            continue

        closing_parenthesis = scanner.find_matching_parenthesis(match.end() - 1)
        if closing_parenthesis is None:
            raise ValueError(f"{macro_name} at line {line}, column {column}: missing closing ')'")

        payload = source[match.end():closing_parenthesis]
        replacement_text = replacement(payload, line, column)
        replacement_text += "\n" * payload.count("\n")
        pieces.extend((source[output_from:match.start()], replacement_text))
        output_from = closing_parenthesis + 1
        search_from = closing_parenthesis + 1
    pieces.append(source[output_from:])
    return "".join(pieces)


def _make_dht_parse_source(source: str) -> tuple[str, dict[int, _DMetaUse]]:
    # Includes are deliberately not part of DHT's semantic input. Replacing
    # directive text while retaining its newline keeps every later source
    # location stable for generated-body and metadata diagnostics.
    source = _INCLUDE_PATTERN.sub("", source)
    dmeta_uses: dict[int, _DMetaUse] = {}

    def replace_dmeta(payload: str, line: int, column: int) -> str:
        _display_name_from_payload(payload, "DMETA", line, column)
        use_id = len(dmeta_uses)
        dmeta_uses[use_id] = _DMetaUse(line, column)
        return f'__attribute__((annotate("{_annotation_payload(f"DMETA:{use_id}", payload)}")))'

    # Record DMETA before rewriting other markers so diagnostics retain its
    # original source location even when markers share a line.
    source = _replace_macro_calls(source, "DMETA", replace_dmeta)
    def replace_dclass(payload: str, line: int, column: int) -> str:
        _class_specifiers_from_payload(payload, line, column)
        return (
            f'__attribute__((annotate("{_annotation_payload("DCLASS", payload)}"))) '
            f"void DHT_CLASS_{line}_{column}();"
        )

    source = _replace_macro_calls(source, "DCLASS", replace_dclass)
    source = _replace_macro_calls(
        source,
        "DSTRUCT",
        lambda payload, line, column:
            f'__attribute__((annotate("{_annotation_payload("DSTRUCT", payload)}"))) '
            f"void DHT_STRUCT_{line}_{column}();",
    )

    def replace_denum(payload: str, line: int, column: int) -> str:
        _display_name_from_payload(payload, "DENUM", line, column)
        return (
            f'__attribute__((annotate("{_annotation_payload("DENUM", payload)}"))) '
            f"void DHT_ENUM_{line}_{column}();"
        )

    source = _replace_macro_calls(source, "DENUM", replace_denum)
    source = _replace_macro_calls(
        source,
        "DPROPERTY",
        lambda payload, _line, _column:
            f'__attribute__((annotate("{_annotation_payload("DPROPERTY", payload)}")))',
    )
    source = _replace_macro_calls(
        source,
        "GENERATED_BODY",
        lambda _payload, _line, _column: "void DHT_GENERATED_BODY();",
    )
    return source, dmeta_uses


def make_dht_parse_source(source: str) -> str:
    return _make_dht_parse_source(source)[0]


def _get_annotation(cursor: clang.cindex.Cursor) -> str:
    for child in cursor.get_children():
        if child.kind == clang.cindex.CursorKind.ANNOTATE_ATTR:
            return child.spelling
    return ""


def _semantic_namespace(cursor: clang.cindex.Cursor) -> str:
    names: list[str] = []
    parent = cursor.semantic_parent
    while parent and parent.kind != clang.cindex.CursorKind.TRANSLATION_UNIT:
        if parent.kind == clang.cindex.CursorKind.NAMESPACE and parent.spelling:
            names.append(parent.spelling)
        parent = parent.semantic_parent
    return "::".join(reversed(names))


def _qualified_name(cursor: clang.cindex.Cursor) -> str:
    namespace = _semantic_namespace(cursor)
    return f"{namespace}::{cursor.spelling}" if namespace else cursor.spelling


def _source_scope_end_line(source: str, start_line: int, start_column: int) -> int:
    scanner = CppSourceScanner(source)
    position = scanner.position_from_line_column(start_line, start_column)
    opening_brace = scanner.find_next_code_position("{", position)
    if opening_brace is None:
        return 0
    closing_brace = scanner.find_matching_brace(opening_brace)
    if closing_brace is not None:
        return scanner.line_number(closing_brace)
    return 0


def _cursor_source_line_range(source: str, cursor: clang.cindex.Cursor) -> tuple[int, int]:
    scanner = CppSourceScanner(source)
    line_count = scanner.line_count
    start_line = cursor.extent.start.line
    if start_line <= 0 or start_line > line_count:
        return 0, 0
    end_line = _source_scope_end_line(source, start_line, cursor.extent.start.column)
    return (start_line, end_line) if end_line >= start_line else (0, 0)


def _scan_generated_body_line(source: str, class_cursor: clang.cindex.Cursor) -> int:
    start_line, end_line = _cursor_source_line_range(source, class_cursor)
    if start_line == 0:
        return 0

    # Synthetic member locations can collapse to the class declaration in PCH or
    # error-recovery ASTs. The cursor extent selects the class; source owns macro lines.
    scanner = CppSourceScanner(source)
    for match in _GENERATED_BODY_PATTERN.finditer(source):
        line = scanner.line_number(match.start())
        if start_line <= line <= end_line and scanner.is_code_position(match.start()):
            return line
    return 0


def _is_default_constructor(cursor: clang.cindex.Cursor) -> bool:
    return cursor.kind == clang.cindex.CursorKind.CONSTRUCTOR and len(list(cursor.get_arguments() or [])) == 0


def _is_object_initializer_constructor(cursor: clang.cindex.Cursor) -> bool:
    if cursor.kind != clang.cindex.CursorKind.CONSTRUCTOR:
        return False
    args = list(cursor.get_arguments() or [])
    if len(args) != 1:
        return False
    spelling = args[0].type.spelling.replace("class ", "").replace("struct ", "")
    return "FObjectInitializer" in spelling


def _base_qualified_name(class_cursor: clang.cindex.Cursor) -> str:
    for child in class_cursor.get_children():
        if child.kind == clang.cindex.CursorKind.CXX_BASE_SPECIFIER:
            decl = child.get_definition() or child.referenced
            if decl and decl.spelling:
                return _qualified_name(decl)
            return child.type.spelling.replace("class ", "").replace("struct ", "").strip()
    return ""


def _source_base_name(source: str, class_cursor: clang.cindex.Cursor) -> str:
    lines = source.splitlines()
    start = max(class_cursor.location.line - 1, 0)
    declaration = " ".join(lines[start:min(start + 6, len(lines))])
    declaration = declaration.split("{", 1)[0]
    match = re.search(
        rf"\b(?:class|struct)\s+(?:[A-Za-z_]\w*_API\s+)?{re.escape(class_cursor.spelling)}"
        r"\s*(?:final\s*)?:\s*(?:(?:public|protected|private)\s+)?([A-Za-z_]\w*(?:::\w+)*)",
        declaration,
    )
    return match.group(1) if match else ""


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


def _apply_property_annotation(prop: ReflectedPropertyInfo | None, annotation: str) -> ReflectedPropertyInfo | None:
    if prop:
        prop.metadata = _property_metadata_from_annotation(annotation)
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
        flags=_property_flags_from_annotation(annotation),
        array_dim=array_dim,
    ), annotation)


def _scan_source_properties_for_class(
    source: str,
    class_cursor: clang.cindex.Cursor,
    exported_symbols: ExportedSymbols | None,
    known_property_names: set[str] | None = None,
    reject_unsupported: bool = True,
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
                prop = _make_property_from_source_decl(stripped, pending_annotation, exported_symbols)
                if prop:
                    properties.append(prop)
                else:
                    declaration = re.match(r"(.+?)\s+(\w+)(?:\s*\[\d+\])?(?:\s*(?:=.*|\{.*\}))?;", stripped)
                    property_name = declaration.group(2) if declaration else ""
                    if reject_unsupported and (
                        not property_name or property_name not in (known_property_names or set())
                    ):
                        type_spelling = declaration.group(1).strip() if declaration else stripped
                        raise ValueError(
                            f"DPROPERTY at line {line_number}: unsupported non-hermetic type "
                            f"spelling '{type_spelling}'"
                        )
                pending_annotation = ""

    return properties


def _is_std_vector(type_spelling: str) -> bool:
    normalized = type_spelling.replace(" ", "")
    return normalized.startswith("std::vector<")


def _is_std_unordered_map(type_spelling: str) -> bool:
    normalized = type_spelling.replace(" ", "")
    return normalized.startswith("std::unordered_map<")


def _is_tobject_ptr(type_spelling: str) -> bool:
    normalized = type_spelling.replace(" ", "")
    return normalized.startswith("TObjectPtr<") or normalized.startswith("Durin::TObjectPtr<")


def _tobject_ptr_arg(type_spelling: str) -> str:
    normalized = type_spelling.strip()
    if normalized.replace(" ", "").startswith("Durin::TObjectPtr<"):
        return _source_template_args(normalized, "Durin::TObjectPtr")[0] if _source_template_args(normalized, "Durin::TObjectPtr") else ""
    return _source_template_args(normalized, "TObjectPtr")[0] if _source_template_args(normalized, "TObjectPtr") else ""


def _resolved_symbol_name(
    spellings: tuple[str, ...],
    exported_symbols: ExportedSymbols | None,
    *,
    kinds: tuple[str, ...],
) -> str | None:
    if not exported_symbols:
        return None
    ordered_spellings = (
        *(spelling for spelling in spellings if "::" in spelling),
        *(spelling for spelling in spellings if "::" not in spelling),
    )
    for spelling in dict.fromkeys(ordered_spellings):
        if resolved := resolve_symbol_name(spelling, exported_symbols, kinds=kinds):
            return resolved
    return None


def _cpp_type_spelling(type_spelling: str, exported_symbols: ExportedSymbols | None) -> str:
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
            return f"std::vector<{_cpp_type_spelling(args[0], exported_symbols)}>"
    if _is_std_unordered_map(type_spelling):
        args = _source_template_args(type_spelling, "std::unordered_map")
        if len(args) >= 2:
            return f"std::unordered_map<{_cpp_type_spelling(args[0], exported_symbols)}, {_cpp_type_spelling(args[1], exported_symbols)}>"
    if _is_tobject_ptr(type_spelling):
        arg = _tobject_ptr_arg(type_spelling)
        if arg:
            return f"Durin::TObjectPtr<{_cpp_type_spelling(arg, exported_symbols)}>"
    if resolved := _resolved_symbol_name(
        (type_spelling,),
        exported_symbols,
        kinds=("enum", "class", "struct"),
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
    ):
        return ReflectedPropertyInfo(
            name=name,
            type_name=type_spelling,
            kind="Struct",
            referenced_struct_type=referenced_struct_type,
            array_dim=array_dim,
            element_size=element_size or f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})",
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
                    else f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})"
                ),
                flags=flags,
            )

    if allow_container and _is_std_vector(type_spelling):
        if depth >= max_depth:
            return None
        args = _source_template_args(type_spelling, "std::vector")
        if len(args) != 1:
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
        )
        if not inner:
            return None
        return ReflectedPropertyInfo(
            name=name,
            type_name=type_spelling,
            kind="Array",
            array_dim=array_dim,
            element_size=element_size or f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})",
            flags=flags,
            inner=inner,
        )

    if allow_container and _is_std_unordered_map(type_spelling):
        if depth >= max_depth:
            return None
        args = _source_template_args(type_spelling, "std::unordered_map")
        if len(args) < 2:
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
        )
        if not key or not value:
            return None
        return ReflectedPropertyInfo(
            name=name,
            type_name=type_spelling,
            kind="Map",
            array_dim=array_dim,
            element_size=element_size or f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})",
            flags=flags,
            key=key,
            value=value,
        )

    if allow_object and type_spelling.endswith("*"):
        pointee = type_spelling[:-1].strip()
        referenced_type = (
            pointee
            if not exported_symbols
            else _resolved_symbol_name((pointee,), exported_symbols, kinds=("class",))
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
            else _resolved_symbol_name((pointee,), exported_symbols, kinds=("class",))
        )
        if referenced_type:
            return ReflectedPropertyInfo(
                name=name,
                type_name=type_spelling,
                kind="Object",
                referenced_type=referenced_type,
                array_dim=array_dim,
                element_size=f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})",
                flags=flags,
                is_object_ptr_wrapper=True,
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


def _is_scoped_enum(enum_cursor: clang.cindex.Cursor) -> bool:
    return bool(enum_cursor.is_scoped_enum())


def _dmeta_use_id(annotation: str) -> int | None:
    match = re.match(r"^DMETA:(\d+)(?:,|$)", annotation)
    return int(match.group(1)) if match else None


def _make_enum(
    enum_cursor: clang.cindex.Cursor,
    module_config,
    header: str,
    annotation: str,
    consumed_dmeta_uses: set[int],
) -> ReflectedEnumInfo:
    qualified_name = _qualified_name(enum_cursor)
    underlying_type = _normalize_type_spelling(enum_cursor.enum_type.spelling)
    values: list[ReflectedEnumValueInfo] = []
    for child in enum_cursor.get_children():
        if child.kind == clang.cindex.CursorKind.ENUM_CONSTANT_DECL:
            value_annotation = _get_annotation(child)
            display_name = ""
            dmeta_use_id = _dmeta_use_id(value_annotation)
            if dmeta_use_id is not None:
                display_name = _string_metadata_from_annotation(value_annotation, "DisplayName")
                consumed_dmeta_uses.add(dmeta_use_id)
            values.append(
                ReflectedEnumValueInfo(
                    name=child.spelling,
                    value=int(child.enum_value),
                    display_name=display_name,
                )
            )
    return ReflectedEnumInfo(
        short_name=enum_cursor.spelling,
        namespace=_semantic_namespace(enum_cursor),
        qualified_name=qualified_name,
        generated_helper_name=make_generated_enum_helper_name(qualified_name),
        header=header,
        api=module_config.api_macro,
        is_scoped=_is_scoped_enum(enum_cursor),
        underlying_type=underlying_type,
        underlying_kind=_underlying_kind_from_type_spelling(underlying_type),
        underlying_size=max(int(enum_cursor.enum_type.get_size() or 0), 0),
        display_name=_string_metadata_from_annotation(annotation, "DisplayName"),
        values=values,
    )


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
    )


def _make_property(field_cursor: clang.cindex.Cursor, exported_symbols: ExportedSymbols | None, source: str) -> ReflectedPropertyInfo | None:
    annotation = _get_annotation(field_cursor)
    if not annotation.startswith("DPROPERTY"):
        return None

    source_type = _source_declared_type(source, field_cursor)
    if source_type and (_is_std_vector(source_type) or _is_std_unordered_map(source_type)):
        return _apply_property_annotation(_make_property_from_spelling(
            field_cursor.spelling,
            source_type,
            exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor),
        ), annotation)

    if source_type:
        source_prop = _make_property_from_spelling(
            field_cursor.spelling,
            source_type,
            exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor),
        )
        if source_prop and source_prop.kind in ("Struct", "String", "Name", "Guid"):
            # Non-fundamental layout belongs to the target compiler, not the
            # synthetic libclang context. Keep it as a C++ sizeof expression.
            return _apply_property_annotation(source_prop, annotation)

    prop = _make_property_from_type(
        field_cursor.spelling,
        _element_type(field_cursor),
        exported_symbols,
        flags=_property_flags_from_annotation(annotation),
        array_dim=_array_dim(field_cursor),
    )
    if prop:
        return _apply_property_annotation(prop, annotation)

    if source_type:
        return _apply_property_annotation(_make_property_from_spelling(
            field_cursor.spelling,
            source_type,
            exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor),
        ), annotation)
    return None


def _clang_args(module_name: str, export_mode: bool) -> list[str]:
    module_config = configs.get_module_config(module_name)
    deps = configs.collect_all_dependent_modules(module_name)
    modules = [module_name, *sorted(deps)]

    args = [
        "-x", "c++",
        "-std=c++20",
        "-ferror-limit=0",
        "-w",
        "-D_DHT_PARSER=1",
        "-DNDEBUG",
        "-D_MSC_VER=1930",
        "-D_WIN32=1",
        "-DFORCEINLINE=inline",
        f"-DDURIN_WITH_EDITOR={1 if configs.RUNTIME_VARIANT == 'DurinEditor' else 0}",
    ]
    if export_mode:
        args.append("-D_DHT_EXPORTS_PARSER=1")

    for dep_module_name in modules:
        dep_config = configs.get_module_config(dep_module_name)
        args.append(f"-D{dep_config.api_macro}=")
    args.append(f"-D{module_config.api_macro}=")
    return args


def _validate_preprocessor_context(source: str) -> None:
    known_macros = {
        "_DHT_PARSER",
        "_DHT_EXPORTS_PARSER",
        "DURIN_WITH_EDITOR",
        "NDEBUG",
        "_MSC_VER",
        "_WIN32",
    }
    locally_defined: set[str] = set()
    for line_number, line in enumerate(source.splitlines(), start=1):
        directive = re.match(r"\s*#\s*(define|undef|ifdef|ifndef|if|elif)\b(.*)", line)
        if not directive:
            continue
        kind, payload = directive.groups()
        payload = payload.strip()
        if kind == "define":
            name = re.match(r"([A-Za-z_]\w*)", payload)
            if name:
                locally_defined.add(name.group(1))
            continue
        if kind == "undef":
            name = re.match(r"([A-Za-z_]\w*)", payload)
            if name:
                locally_defined.discard(name.group(1))
            continue
        if kind in ("ifdef", "ifndef"):
            identifiers = [payload]
        else:
            expression = re.sub(r"defined\s*(?:\(\s*([A-Za-z_]\w*)\s*\)|\s+([A-Za-z_]\w*))", r"\1\2", payload)
            identifiers = re.findall(r"\b[A-Za-z_]\w*\b", expression)
        unknown = sorted({
            identifier
            for identifier in identifiers
            if identifier not in known_macros
            and identifier not in locally_defined
            and identifier not in {"true", "false"}
        })
        if unknown:
            raise ValueError(
                f"preprocessor condition at line {line_number}: unsupported non-hermetic "
                f"macro dependency '{unknown[0]}'"
            )


def _synthetic_declaration(symbol: ExportedSymbolInfo) -> str:
    parts = symbol.QualifiedName.split("::")
    name = parts[-1]
    namespaces = parts[:-1]
    if symbol.Kind == "enum":
        underlying = symbol.UnderlyingType or "int"
        keyword = "enum class" if symbol.IsScoped else "enum"
        declaration = f"{keyword} {name} : {underlying};"
    else:
        declaration = f"{symbol.Kind} {name} {{}};"
    for namespace in reversed(namespaces):
        declaration = f"namespace {namespace} {{ {declaration} }}"
    return declaration


def _synthetic_parser_prelude(
    source: str,
    header: str,
    exported_symbols: ExportedSymbols | None,
) -> str:
    declared_names = set(_TYPE_DECLARATION_PATTERN.findall(source))
    lines = [
        f"// DHT parser context {PARSER_CONTEXT_VERSION}",
        "namespace std { class string {}; template<class T> class vector {}; "
        "template<class K, class V> class unordered_map {}; }",
        "namespace Durin {",
        "using int8 = signed char; using int16 = short; using int32 = int; using int64 = long long;",
        "using uint8 = unsigned char; using uint16 = unsigned short; using uint32 = unsigned int; "
        "using uint64 = unsigned long long;",
        "class FName {}; struct FGuid {}; class FObjectInitializer {};",
        "template<class T> class TObjectPtr {};",
        "}",
    ]
    for symbol in sorted((exported_symbols or {}).values(), key=lambda item: item.QualifiedName):
        if symbol.Header == header or symbol.ShortName in declared_names:
            continue
        lines.append(_synthetic_declaration(symbol))
    return "\n".join(lines) + "\n"


def _include_path_for_header(header: str) -> str:
    include_path = Path(header).as_posix()
    if include_path.startswith("Public/"):
        include_path = include_path[len("Public/"):]
    elif include_path.startswith("Private/"):
        include_path = include_path[len("Private/"):]
    return include_path


def _file_id_for_header(module_name: str, header: str) -> str:
    include_path = _include_path_for_header(header)
    return "FID_DURIN_" + module_name + "_" + include_path.replace("/", "_").replace(".", "_")


def _parse_translation_unit(
    module_name: str,
    header: str,
    header_path: Path,
    source: str,
    export_mode: bool,
    exported_symbols: ExportedSymbols | None = None,
) -> tuple[clang.cindex.TranslationUnit, dict[int, _DMetaUse]]:
    index = clang.cindex.Index.create()
    parsed_source, dmeta_uses = _make_dht_parse_source(source)
    prelude_path = header_path.parent / f".__dht_prelude_{header_path.stem}.h"
    unsaved_files = [
        (str(header_path), parsed_source),
        (str(prelude_path), _synthetic_parser_prelude(source, header, exported_symbols)),
    ]
    translation_unit = index.parse(
        str(header_path),
        args=[*_clang_args(module_name, export_mode), "-include", str(prelude_path)],
        unsaved_files=unsaved_files,
        options=clang.cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES,
    )
    return translation_unit, dmeta_uses


def parse_reflection_header(
    module_name: str,
    header: str,
    exported_symbols: ExportedSymbols | None = None,
    export_mode: bool = False,
) -> ReflectedHeaderInfo:
    module_config = configs.get_module_config(module_name)
    header_path = (module_config.module_dir / header).resolve()
    source = header_path.read_text(encoding="utf-8")
    _validate_preprocessor_context(source)
    tu, dmeta_uses = _parse_translation_unit(
        module_name,
        header,
        header_path,
        source,
        export_mode,
        exported_symbols,
    )

    classes: list[ReflectedClassInfo] = []
    enums: list[ReflectedEnumInfo] = []
    structs: list[ReflectedStructInfo] = []
    consumed_dmeta_uses: set[int] = set()

    def visit(parent: clang.cindex.Cursor) -> None:
        children = list(parent.get_children())
        pending_dclass_annotation = ""
        pending_dstruct = False
        pending_denum_annotation = ""
        for child in children:
            if child.location.file is None or Path(str(child.location.file)) != header_path:
                continue

            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling.startswith("DHT_CLASS_"):
                annotation = _get_annotation(child)
                pending_dclass_annotation = annotation if annotation.startswith("DCLASS") else ""
                continue
            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling.startswith("DHT_STRUCT_"):
                pending_dstruct = _get_annotation(child).startswith("DSTRUCT")
                continue
            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling.startswith("DHT_ENUM_"):
                annotation = _get_annotation(child)
                pending_denum_annotation = annotation if annotation.startswith("DENUM") else ""
                continue

            if pending_dstruct and child.kind == clang.cindex.CursorKind.STRUCT_DECL and child.spelling:
                qualified_name = _qualified_name(child)
                reflected_struct = ReflectedStructInfo(
                    short_name=child.spelling,
                    namespace=_semantic_namespace(child),
                    qualified_name=qualified_name,
                    generated_helper_name=make_generated_struct_helper_name(qualified_name),
                    header=header,
                    api=module_config.api_macro,
                    generated_body_line=_scan_generated_body_line(source, child),
                )
                for member in child.get_children():
                    if member.kind == clang.cindex.CursorKind.FIELD_DECL:
                        prop = _make_property(member, exported_symbols, source)
                        if prop:
                            reflected_struct.properties.append(prop)
                existing_property_names = {prop.name for prop in reflected_struct.properties}
                for prop in _scan_source_properties_for_class(
                    source,
                    child,
                    exported_symbols,
                    existing_property_names,
                    reject_unsupported=not export_mode,
                ):
                    if prop.name not in existing_property_names:
                        reflected_struct.properties.append(prop)
                        existing_property_names.add(prop.name)
                structs.append(reflected_struct)
                pending_dstruct = False
                continue

            if pending_dclass_annotation and child.kind in (clang.cindex.CursorKind.CLASS_DECL, clang.cindex.CursorKind.STRUCT_DECL) and child.spelling:
                qualified_name = _qualified_name(child)
                helper_name = make_generated_helper_name(qualified_name)
                class_payload = pending_dclass_annotation.split(",", 1)[1] if "," in pending_dclass_annotation else ""
                is_abstract, display_name, default_object_name = _class_specifiers_from_payload(
                    class_payload, child.location.line, child.location.column
                )
                reflected_class = ReflectedClassInfo(
                    short_name=child.spelling,
                    namespace=_semantic_namespace(child),
                    qualified_name=qualified_name,
                    generated_helper_name=helper_name,
                    header=header,
                    api=module_config.api_macro,
                    base_qualified_name=_source_base_name(source, child) or _base_qualified_name(child),
                    generated_body_line=_scan_generated_body_line(source, child),
                    is_abstract=is_abstract,
                    display_name=display_name,
                    default_object_name=default_object_name,
                )
                for member in child.get_children():
                    if _is_default_constructor(member):
                        reflected_class.has_default_constructor = True
                    elif _is_object_initializer_constructor(member):
                        reflected_class.has_object_initializer_constructor = True
                    elif member.kind == clang.cindex.CursorKind.DESTRUCTOR:
                        reflected_class.has_destructor = True
                    elif member.kind == clang.cindex.CursorKind.FIELD_DECL:
                        prop = _make_property(member, exported_symbols, source)
                        if prop:
                            reflected_class.properties.append(prop)
                existing_property_names = {prop.name for prop in reflected_class.properties}
                for prop in _scan_source_properties_for_class(
                    source,
                    child,
                    exported_symbols,
                    existing_property_names,
                    reject_unsupported=not export_mode,
                ):
                    if prop.name not in existing_property_names:
                        reflected_class.properties.append(prop)
                        existing_property_names.add(prop.name)
                classes.append(reflected_class)
                pending_dclass_annotation = ""
                continue

            if pending_denum_annotation and child.kind == clang.cindex.CursorKind.ENUM_DECL and child.spelling:
                enums.append(
                    _make_enum(
                        child,
                        module_config,
                        header,
                        pending_denum_annotation,
                        consumed_dmeta_uses,
                    )
                )
                pending_denum_annotation = ""
                continue

            if child.kind in (clang.cindex.CursorKind.NAMESPACE, clang.cindex.CursorKind.TRANSLATION_UNIT):
                visit(child)

    visit(tu.cursor)

    unconsumed_dmeta_uses = dmeta_uses.keys() - consumed_dmeta_uses
    if unconsumed_dmeta_uses:
        use = dmeta_uses[min(unconsumed_dmeta_uses)]
        raise ValueError(
            f"DMETA at line {use.line}, column {use.column}: "
            "annotation is only valid on an enumerator in a reflected enum"
        )

    rel = Path(header)
    include_path = _include_path_for_header(header)
    file_id = _file_id_for_header(module_name, header)
    return ReflectedHeaderInfo(module_name, header, header_path, include_path, file_id, classes, enums, structs)
