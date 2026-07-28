from dataclasses import dataclass, field
from pathlib import Path
import re

import clang.cindex

from durin_header_tool import config as configs
from durin_header_tool import io as utils

SYMBOL_NAME_SCHEME = "qualified-underscore-v1"
TOOL_VERSION = "19"
MAX_CONTAINER_PROPERTY_DEPTH = 4


@dataclass(frozen=True)
class _DMetaUse:
    line: int
    column: int


@dataclass
class ReflectedEnumValueInfo:
    name: str
    value: int
    display_name: str = ""


@dataclass
class ReflectedEnumInfo:
    short_name: str
    namespace: str
    qualified_name: str
    generated_helper_name: str
    header: str
    api: str
    is_scoped: bool = False
    underlying_type: str = ""
    underlying_kind: str = "Unknown"
    underlying_size: int = 0
    display_name: str = ""
    values: list[ReflectedEnumValueInfo] = field(default_factory=list)

    @property
    def generated_helper_no_register_name(self) -> str:
        return f"{self.generated_helper_name}_NoRegister"

    @property
    def generated_statics_name(self) -> str:
        return f"{self.generated_helper_name}_Statics"

    @property
    def registration_info_name(self) -> str:
        return f"Z_Registration_Info_DEnum_{qualified_name_to_helper_suffix(self.qualified_name)}"


@dataclass
class ReflectedPropertyInfo:
    name: str
    type_name: str
    kind: str
    referenced_type: str = ""
    referenced_enum_type: str = ""
    referenced_struct_type: str = ""
    array_dim: int = 1
    element_size: str = "0"
    flags: str = "None"
    is_object_ptr_wrapper: bool = False
    inner: "ReflectedPropertyInfo | None" = None
    key: "ReflectedPropertyInfo | None" = None
    value: "ReflectedPropertyInfo | None" = None
    metadata: list[tuple[str, str]] = field(default_factory=list)


@dataclass
class ReflectedClassInfo:
    short_name: str
    namespace: str
    qualified_name: str
    generated_helper_name: str
    header: str
    api: str
    base_qualified_name: str = ""
    generated_body_line: int = 0
    has_default_constructor: bool = False
    has_object_initializer_constructor: bool = False
    has_destructor: bool = False
    is_abstract: bool = False
    display_name: str = ""
    default_object_name: str = ""
    properties: list[ReflectedPropertyInfo] = field(default_factory=list)

    @property
    def generated_helper_no_register_name(self) -> str:
        return f"{self.generated_helper_name}_NoRegister"

    @property
    def generated_statics_name(self) -> str:
        return f"{self.generated_helper_name}_Statics"

    @property
    def registration_info_name(self) -> str:
        return f"Z_Registration_Info_DClass_{qualified_name_to_helper_suffix(self.qualified_name)}"


@dataclass
class ReflectedStructInfo:
    short_name: str
    namespace: str
    qualified_name: str
    generated_helper_name: str
    header: str
    api: str
    generated_body_line: int = 0
    properties: list[ReflectedPropertyInfo] = field(default_factory=list)

    @property
    def generated_helper_no_register_name(self) -> str:
        return f"{self.generated_helper_name}_NoRegister"

    @property
    def generated_statics_name(self) -> str:
        return f"{self.generated_helper_name}_Statics"


@dataclass
class ReflectedHeaderInfo:
    module_name: str
    header: str
    header_path: Path
    include_path: str
    file_id: str
    classes: list[ReflectedClassInfo] = field(default_factory=list)
    enums: list[ReflectedEnumInfo] = field(default_factory=list)
    structs: list[ReflectedStructInfo] = field(default_factory=list)


_DPROPERTY_PATTERN = re.compile(r"\bDPROPERTY\s*\(([^)]*)\)")
_GENERATED_BODY_PATTERN = re.compile(r"\bGENERATED_BODY\s*\([^)]*\)")
_GEN_INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s+"[^"]+\.gen\.h"\s*$', re.MULTILINE)

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


def _init_clang() -> None:
    clang_lib_dir = configs.environment.DURIN_ROOT_DIR / "Engine" / "Source" / "ThirdParty" / "clang" / "bin"
    if clang_lib_dir.exists():
        try:
            clang.cindex.Config.set_library_path(str(clang_lib_dir))
        except Exception:
            pass


def _annotation_payload(prefix: str, payload: str) -> str:
    payload = payload.strip().replace("\\", "\\\\").replace('"', '\\"')
    return f'{prefix},{payload}' if payload else prefix


def _display_name_from_payload(payload: str, macro_name: str, line: int, column: int) -> str:
    location = f"{macro_name} at line {line}, column {column}"
    if not payload.strip():
        return ""

    entries: list[str] = []
    entry_start = 0
    in_string = False
    escaped = False
    for index, char in enumerate(payload):
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char == ",":
            entries.append(payload[entry_start:index])
            entry_start = index + 1
    if in_string:
        raise ValueError(f"{location}: unterminated quoted string")
    entries.append(payload[entry_start:])

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
        display_name = match.group(1).replace(r'\"', '"').replace(r"\\", "\\")
    return display_name


def _class_specifiers_from_payload(
    payload: str, line: int, column: int
) -> tuple[bool, str, str]:
    location = f"DCLASS at line {line}, column {column}"
    if not payload.strip():
        return False, "", ""

    entries: list[str] = []
    entry_start = 0
    in_string = False
    escaped = False
    for index, char in enumerate(payload):
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char == ",":
            entries.append(payload[entry_start:index])
            entry_start = index + 1
    if in_string:
        raise ValueError(f"{location}: unterminated quoted string")
    entries.append(payload[entry_start:])

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
        metadata[key] = match.group(1).replace(r'\"', '"').replace(r"\\", "\\")

    return is_abstract, metadata.get("DisplayName", ""), metadata.get("DefaultObjectName", "")


def _is_cpp_code_position(source: str, position: int) -> bool:
    state = "code"
    escaped = False
    index = 0
    while index < position:
        char = source[index]
        next_char = source[index + 1] if index + 1 < position else ""
        if state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (state == "character" and char == "'"):
                state = "code"
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                index += 1
        elif char == "/" and next_char == "/":
            state = "line_comment"
            index += 1
        elif char == "/" and next_char == "*":
            state = "block_comment"
            index += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        index += 1
    return state == "code"


def _replace_macro_calls(source: str, macro_name: str, replacement) -> str:
    pattern = re.compile(rf"\b{re.escape(macro_name)}\s*\(")
    search_from = 0
    pieces: list[str] = []
    output_from = 0
    while match := pattern.search(source, search_from):
        if not _is_cpp_code_position(source, match.start()):
            search_from = match.end()
            continue
        line_start = source.rfind("\n", 0, match.start()) + 1
        if source[line_start:match.start()].lstrip().startswith("#"):
            search_from = match.end()
            continue

        depth = 1
        index = match.end()
        in_string = False
        escaped = False
        while index < len(source) and depth:
            char = source[index]
            if in_string:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    in_string = False
            elif char == '"':
                in_string = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            index += 1
        line = source.count("\n", 0, match.start()) + 1
        column = match.start() - line_start + 1
        if depth:
            raise ValueError(f"{macro_name} at line {line}, column {column}: missing closing ')'")

        payload = source[match.end():index - 1]
        replacement_text = replacement(payload, line, column)
        replacement_text += "\n" * payload.count("\n")
        pieces.extend((source[output_from:match.start()], replacement_text))
        output_from = index
        search_from = index
    pieces.append(source[output_from:])
    return "".join(pieces)


def _make_dht_parse_source(source: str) -> tuple[str, dict[int, _DMetaUse]]:
    source = _GEN_INCLUDE_PATTERN.sub("", source)
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
    source = _GENERATED_BODY_PATTERN.sub("void DHT_GENERATED_BODY();", source)
    return source, dmeta_uses


def make_dht_parse_source(source: str) -> str:
    return _make_dht_parse_source(source)[0]


def qualified_name_to_helper_suffix(qualified_name: str) -> str:
    segments = [segment for segment in qualified_name.split("::") if segment]
    for segment in segments:
        if "_" in segment:
            raise ValueError(
                f"Reflected symbol segment '{segment}' in '{qualified_name}' contains '_', "
                f"which is not allowed by {SYMBOL_NAME_SCHEME}."
            )
    return "_".join(segments)


def make_generated_helper_name(qualified_name: str) -> str:
    return f"Z_Construct_DClass_{qualified_name_to_helper_suffix(qualified_name)}"


def make_generated_enum_helper_name(qualified_name: str) -> str:
    return f"Z_Construct_DEnum_{qualified_name_to_helper_suffix(qualified_name)}"


def make_generated_struct_helper_name(qualified_name: str) -> str:
    return f"Z_Construct_DStruct_{qualified_name_to_helper_suffix(qualified_name)}"


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
    lines = source.splitlines(keepends=True)
    position = sum(len(line) for line in lines[:start_line - 1]) + max(start_column - 1, 0)
    state = "code"
    escaped = False
    brace_depth = 0
    body_started = False

    while position < len(source):
        char = source[position]
        next_char = source[position + 1] if position + 1 < len(source) else ""
        if state in ("string", "character"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (state == "character" and char == "'"):
                state = "code"
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and next_char == "/":
                state = "code"
                position += 1
        elif char == "/" and next_char == "/":
            state = "line_comment"
            position += 1
        elif char == "/" and next_char == "*":
            state = "block_comment"
            position += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "character"
        elif char == "{":
            body_started = True
            brace_depth += 1
        elif char == "}" and body_started:
            brace_depth -= 1
            if brace_depth == 0:
                return source.count("\n", 0, position) + 1
        position += 1
    return 0


def _cursor_source_line_range(source: str, cursor: clang.cindex.Cursor) -> tuple[int, int]:
    line_count = len(source.splitlines())
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
    for match in _GENERATED_BODY_PATTERN.finditer(source):
        line = source.count("\n", 0, match.start()) + 1
        if start_line <= line <= end_line and _is_cpp_code_position(source, match.start()):
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


def _property_flags_from_annotation(annotation: str) -> str:
    if "," not in annotation:
        return "None"
    payload = annotation.split(",", 1)[1]
    flags: list[str] = []
    for raw_specifier in payload.split(","):
        specifier = raw_specifier.strip()
        if "=" in specifier:
            specifier = specifier.split("=", 1)[0].strip()
        flag = _PROPERTY_FLAG_BY_SPECIFIER.get(specifier)
        if flag:
            flags.append(f"Durin::EPropertyFlags::{flag}")
    return " | ".join(flags) if flags else "None"


def _string_metadata_from_annotation(annotation: str, key: str) -> str:
    if "," not in annotation:
        return ""
    payload = annotation.split(",", 1)[1]
    match = re.search(rf'(?:^|,)\s*{re.escape(key)}\s*=\s*"((?:\\.|[^"\\])*)"', payload)
    if not match:
        return ""
    return match.group(1).replace(r'\"', '"').replace(r"\\", "\\")


def _property_metadata_from_annotation(annotation: str) -> list[tuple[str, str]]:
    payload = _string_metadata_from_annotation(annotation, "MetaData")
    if not payload:
        match = re.search(r"(?:^|,)\s*MetaData\s*=\s*([A-Za-z_][A-Za-z0-9_]*)", annotation)
        payload = match.group(1) if match else ""
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
    exported_symbols: dict[str, object] | None,
) -> ReflectedPropertyInfo | None:
    line = source_line.rstrip(";").strip()
    match = re.match(r"(.+?)\s+(\w+)(?:\s*\[(\d+)\])?(?:\s*(?:=.*|\{.*\}))?$", line)
    if not match:
        return None
    type_spelling = _normalize_type_spelling(match.group(1))
    name = match.group(2)
    array_dim = int(match.group(3)) if match.group(3) else 1
    return _apply_property_annotation(_source_property_from_type_spelling(
        name,
        type_spelling,
        exported_symbols,
        flags=_property_flags_from_annotation(annotation),
        array_dim=array_dim,
    ), annotation)


def _scan_source_properties_for_class(
    source: str,
    class_cursor: clang.cindex.Cursor,
    exported_symbols: dict[str, object] | None,
) -> list[ReflectedPropertyInfo]:
    properties: list[ReflectedPropertyInfo] = []
    start_line, end_line = _cursor_source_line_range(source, class_cursor)
    if start_line == 0:
        return properties

    in_class = False
    brace_depth = 0
    pending_annotation = ""

    for line in source.splitlines()[start_line - 1:end_line]:
        stripped = line.strip()
        if not in_class:
            if "{" in stripped:
                in_class = True
                brace_depth += stripped.count("{") - stripped.count("}")
            continue

        if not in_class:
            continue

        dproperty_match = _DPROPERTY_PATTERN.search(stripped)
        if dproperty_match:
            pending_annotation = _annotation_payload("DPROPERTY", dproperty_match.group(1))
            brace_depth += stripped.count("{") - stripped.count("}")
            continue

        if pending_annotation:
            if not stripped or stripped in ("public:", "private:", "protected:"):
                continue
            if ";" in stripped:
                prop = _make_property_from_source_decl(stripped, pending_annotation, exported_symbols)
                if prop:
                    properties.append(prop)
                pending_annotation = ""

        brace_depth += stripped.count("{") - stripped.count("}")
        if brace_depth <= 0:
            break

    return properties


def _template_argument_types(type_info: clang.cindex.Type) -> list[clang.cindex.Type]:
    args: list[clang.cindex.Type] = []
    for candidate in (type_info, type_info.get_canonical()):
        try:
            count = candidate.get_num_template_arguments()
        except Exception:
            continue
        if count <= 0:
            continue
        for index in range(count):
            try:
                arg = candidate.get_template_argument_type(index)
            except Exception:
                return []
            if arg.kind == clang.cindex.TypeKind.INVALID:
                return []
            args.append(arg)
        if args:
            return args
    return []


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


def _cpp_type_spelling(type_spelling: str, exported_symbols: dict[str, object] | None) -> str:
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
    candidates = [type_spelling]
    if "::" not in type_spelling:
        candidates.extend(name for name in (exported_symbols or {}) if name.endswith(f"::{type_spelling}") or name == type_spelling)
    for candidate in candidates:
        symbol = (exported_symbols or {}).get(candidate)
        if symbol and getattr(symbol, "Kind", "") == "enum":
            return candidate
        if symbol and getattr(symbol, "Kind", "") in ("class", "struct"):
            return candidate
    if type_spelling.endswith("*"):
        pointee = type_spelling[:-1].strip()
        candidates = [pointee]
        if "::" not in pointee:
            candidates.extend(name for name in (exported_symbols or {}) if name.endswith(f"::{pointee}") or name == pointee)
        for candidate in candidates:
            if not exported_symbols or candidate in exported_symbols:
                return f"{candidate}*"
    return type_spelling


def _source_property_from_type_spelling(
    name: str,
    type_spelling: str,
    exported_symbols: dict[str, object] | None,
    flags: str = "None",
    array_dim: int = 1,
    allow_object: bool = True,
    allow_container: bool = True,
    allow_enum: bool = True,
    depth: int = 0,
    max_depth: int = MAX_CONTAINER_PROPERTY_DEPTH,
) -> ReflectedPropertyInfo | None:
    type_spelling = _normalize_type_spelling(type_spelling)
    kind = _PROPERTY_KIND_BY_TYPE.get(type_spelling)
    if not kind and type_spelling == "std::string":
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
        return ReflectedPropertyInfo(name=name, type_name=type_spelling, kind=kind, array_dim=array_dim, element_size=size_by_kind[kind], flags=flags)

    struct_candidates = [type_spelling]
    if "::" not in type_spelling:
        struct_candidates.extend(
            symbol_name for symbol_name, symbol in (exported_symbols or {}).items()
            if getattr(symbol, "Kind", "") == "struct" and (symbol_name.endswith(f"::{type_spelling}") or symbol_name == type_spelling)
        )
    for candidate in struct_candidates:
        symbol = (exported_symbols or {}).get(candidate)
        if symbol and getattr(symbol, "Kind", "") == "struct":
            return ReflectedPropertyInfo(
                name=name,
                type_name=type_spelling,
                kind="Struct",
                referenced_struct_type=candidate,
                array_dim=array_dim,
                element_size=f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})",
                flags=flags,
            )

    if allow_enum:
        enum_candidates = [type_spelling]
        if "::" not in type_spelling:
            enum_candidates.extend(
                name
                for name, symbol in (exported_symbols or {}).items()
                if getattr(symbol, "Kind", "") == "enum" and (name.endswith(f"::{type_spelling}") or name == type_spelling)
            )
        for candidate in enum_candidates:
            symbol = (exported_symbols or {}).get(candidate)
            if symbol and getattr(symbol, "Kind", "") == "enum":
                underlying_size = getattr(symbol, "UnderlyingSize", 0) or getattr(symbol, "UnderlyingByteSize", 0) or 0
                return ReflectedPropertyInfo(
                    name=name,
                    type_name=type_spelling,
                    kind="Enum",
                    referenced_enum_type=candidate,
                    array_dim=array_dim,
                    element_size=f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})" if not underlying_size else str(underlying_size),
                    flags=flags,
                )
        if exported_symbols is not None:
            matching_enums = [
                name
                for name, symbol in exported_symbols.items()
                if getattr(symbol, "Kind", "") == "enum" and getattr(symbol, "ShortName", "") == type_spelling
            ]
            if len(matching_enums) == 1:
                return ReflectedPropertyInfo(
                    name=name,
                    type_name=type_spelling,
                    kind="Enum",
                    referenced_enum_type=matching_enums[0],
                    array_dim=array_dim,
                    element_size=f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})",
                    flags=flags,
                )

    if allow_container and _is_std_vector(type_spelling):
        if depth >= max_depth:
            return None
        args = _source_template_args(type_spelling, "std::vector")
        if len(args) != 1:
            return None
        inner = _source_property_from_type_spelling(
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
            element_size=f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})",
            flags=flags,
            inner=inner,
        )

    if allow_container and _is_std_unordered_map(type_spelling):
        if depth >= max_depth:
            return None
        args = _source_template_args(type_spelling, "std::unordered_map")
        if len(args) < 2:
            return None
        key = _source_property_from_type_spelling(
            f"{name}_Key",
            args[0],
            exported_symbols,
            allow_object=False,
            allow_container=False,
            allow_enum=True,
            depth=depth + 1,
            max_depth=max_depth,
        )
        value = _source_property_from_type_spelling(
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
            element_size=f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})",
            flags=flags,
            key=key,
            value=value,
        )

    if allow_object and type_spelling.endswith("*"):
        pointee = type_spelling[:-1].strip()
        candidates = [pointee]
        if "::" not in pointee:
            candidates.extend(name for name in (exported_symbols or {}) if name.endswith(f"::{pointee}") or name == pointee)
        for candidate in candidates:
            if not exported_symbols or candidate in exported_symbols:
                return ReflectedPropertyInfo(
                    name=name,
                    type_name=type_spelling,
                    kind="Object",
                    referenced_type=candidate,
                    array_dim=array_dim,
                    element_size="sizeof(Durin::DObject*)",
                    flags=flags,
                )
    if allow_object and _is_tobject_ptr(type_spelling):
        pointee = _tobject_ptr_arg(type_spelling)
        if not pointee:
            return None
        candidates = [pointee]
        if "::" not in pointee:
            candidates.extend(name for name in (exported_symbols or {}) if name.endswith(f"::{pointee}") or name == pointee)
        for candidate in candidates:
            if not exported_symbols or candidate in exported_symbols:
                return ReflectedPropertyInfo(
                    name=name,
                    type_name=type_spelling,
                    kind="Object",
                    referenced_type=candidate,
                    array_dim=array_dim,
                    element_size=f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})",
                    flags=flags,
                    is_object_ptr_wrapper=True,
                )
    return None


def _string_kind(type_spelling: str, canonical: str) -> str | None:
    if type_spelling == "std::string":
        return "String"
    if canonical.startswith("std::basic_string<"):
        return "String"
    return None


def _enum_kind(type_info: clang.cindex.Type) -> str | None:
    decl = type_info.get_declaration()
    if decl.kind == clang.cindex.CursorKind.ENUM_DECL:
        return "Enum"
    return None


def _enum_referenced_type(type_info: clang.cindex.Type, exported_symbols: dict[str, object] | None) -> str:
    decl = type_info.get_declaration()
    if decl.kind != clang.cindex.CursorKind.ENUM_DECL or not decl.spelling:
        return ""
    qualified_name = _qualified_name(decl)
    if exported_symbols and qualified_name not in exported_symbols:
        short_name = decl.spelling
        matches = [
            name
            for name, symbol in exported_symbols.items()
            if getattr(symbol, "Kind", "") == "enum" and getattr(symbol, "ShortName", "") == short_name
        ]
        return matches[0] if len(matches) == 1 else qualified_name
    return qualified_name


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
    try:
        return bool(enum_cursor.is_scoped_enum())
    except Exception:
        return "enum class" in enum_cursor.type.spelling


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
    exported_symbols: dict[str, object] | None,
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
    kind = _PROPERTY_KIND_BY_TYPE.get(type_spelling) or _PROPERTY_KIND_BY_TYPE.get(canonical) or _string_kind(type_spelling, canonical)
    referenced_type = ""
    element_size = _field_size(type_info)

    referenced_struct_type = ""
    if not kind:
        candidates = [type_spelling, canonical]
        if "::" not in type_spelling:
            candidates.extend(name for name, symbol in (exported_symbols or {}).items() if getattr(symbol, "Kind", "") == "struct" and getattr(symbol, "ShortName", "") == type_spelling)
        for candidate in candidates:
            symbol = (exported_symbols or {}).get(candidate)
            if symbol and getattr(symbol, "Kind", "") == "struct":
                kind = "Struct"
                referenced_struct_type = candidate
                break

    if not kind and allow_enum:
        kind = _enum_kind(type_info)
        if kind == "Enum":
            referenced_type = _enum_referenced_type(type_info, exported_symbols)
            if exported_symbols is not None and referenced_type not in exported_symbols:
                return None

    if not kind and allow_container and _is_std_vector(type_spelling):
        if depth >= max_depth:
            return None
        args = _template_argument_types(type_info)
        if len(args) != 1:
            return None
        inner = _make_property_from_type(
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
            element_size=element_size,
            flags=flags,
            inner=inner,
        )

    if not kind and allow_container and _is_std_unordered_map(type_spelling):
        if depth >= max_depth:
            return None
        args = _template_argument_types(type_info)
        if len(args) < 2:
            return None
        key = _make_property_from_type(
            f"{name}_Key",
            args[0],
            exported_symbols,
            allow_object=False,
            allow_container=False,
            allow_enum=True,
            depth=depth + 1,
            max_depth=max_depth,
        )
        value = _make_property_from_type(
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
            element_size=element_size,
            flags=flags,
            key=key,
            value=value,
        )

    if not kind and allow_object and type_spelling.endswith("*"):
        pointee = type_spelling[:-1].strip()
        candidates = [pointee]
        if "::" not in pointee:
            candidates.extend(name for name in (exported_symbols or {}) if name.endswith(f"::{pointee}") or name == pointee)
        for candidate in candidates:
            if not exported_symbols or candidate in exported_symbols:
                referenced_type = candidate
                kind = "Object"
                break

    is_object_ptr_wrapper = False
    if not kind and allow_object and _is_tobject_ptr(type_spelling):
        pointee = _tobject_ptr_arg(type_spelling)
        candidates = [pointee]
        if "::" not in pointee:
            candidates.extend(name for name in (exported_symbols or {}) if name.endswith(f"::{pointee}") or name == pointee)
        for candidate in candidates:
            if not exported_symbols or candidate in exported_symbols:
                referenced_type = candidate
                element_size = f"sizeof({_cpp_type_spelling(type_spelling, exported_symbols)})"
                kind = "Object"
                is_object_ptr_wrapper = True
                break

    if not kind:
        return None

    return ReflectedPropertyInfo(
        name=name,
        type_name=type_spelling,
        kind=kind,
        referenced_type="" if kind == "Enum" else referenced_type,
        referenced_enum_type=referenced_type if kind == "Enum" else "",
        referenced_struct_type=referenced_struct_type,
        array_dim=array_dim,
        element_size=element_size,
        flags=flags,
        is_object_ptr_wrapper=is_object_ptr_wrapper,
    )


def _make_property(field_cursor: clang.cindex.Cursor, exported_symbols: dict[str, object] | None, source: str) -> ReflectedPropertyInfo | None:
    annotation = _get_annotation(field_cursor)
    if not annotation.startswith("DPROPERTY"):
        return None

    source_type = _source_declared_type(source, field_cursor)
    if source_type and (_is_std_vector(source_type) or _is_std_unordered_map(source_type)):
        return _apply_property_annotation(_source_property_from_type_spelling(
            field_cursor.spelling,
            source_type,
            exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor),
        ), annotation)

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
        return _apply_property_annotation(_source_property_from_type_spelling(
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
    ]
    if export_mode:
        args.append("-D_DHT_EXPORTS_PARSER=1")

    for dep_module_name in modules:
        dep_config = configs.get_module_config(dep_module_name)
        args.append(f"-D{dep_config.api_macro}=")
        args.append(f"-I{(dep_config.module_dir / 'Public').resolve()}")
        args.append(f"-I{(dep_config.module_dir / 'Private').resolve()}")
        dht_output_dir = utils.get_module_dht_output_dir(dep_module_name)
        args.append(f"-I{dht_output_dir}")

    args.append(f"-I{configs.environment.DURIN_ROOT_DIR / 'Engine' / 'Source' / 'Runtime' / 'Core' / 'Public'}")
    args.append(f"-I{configs.environment.DURIN_ROOT_DIR / 'Engine' / 'Source' / 'ThirdParty'}")
    args.append(f"-D{module_config.api_macro}=")
    return args


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


def _fake_generated_header_content(module_name: str, header: str) -> str:
    source_path = (configs.get_module_config(module_name).module_dir / header).resolve()
    source = source_path.read_text(encoding="utf-8")
    file_id = _file_id_for_header(module_name, header)
    lines = ["#pragma once\n"]
    for line_number, line in enumerate(source.splitlines(), start=1):
        if "GENERATED_BODY" in line:
            lines.append(f"#define {file_id}_{line_number}_GENERATED_BODY public:\n")
    lines.append("#undef CURRENT_FILE_ID\n")
    lines.append(f"#define CURRENT_FILE_ID {file_id}\n")
    return "".join(lines)


def _fake_generated_headers(module_name: str) -> list[tuple[str, str]]:
    unsaved: list[tuple[str, str]] = []
    modules = [module_name, *sorted(configs.collect_all_dependent_modules(module_name))]
    for dep_module_name in modules:
        dep_config = configs.get_module_config(dep_module_name)
        output_dir = utils.get_module_dht_output_dir(dep_module_name)
        for header in dep_config.reflect_headers:
            gen_header_path = output_dir / f"{Path(header).stem}.gen.h"
            unsaved.append((str(gen_header_path), _fake_generated_header_content(dep_module_name, header)))
    return unsaved


def _parse_translation_unit(
    module_name: str,
    header_path: Path,
    source: str,
    export_mode: bool,
) -> tuple[clang.cindex.TranslationUnit, dict[int, _DMetaUse]]:
    _init_clang()
    index = clang.cindex.Index.create()
    parsed_source, dmeta_uses = _make_dht_parse_source(source)
    unsaved_files = [(str(header_path), parsed_source)]
    unsaved_files.extend(_fake_generated_headers(module_name))
    translation_unit = index.parse(
        str(header_path),
        args=_clang_args(module_name, export_mode),
        unsaved_files=unsaved_files,
        options=clang.cindex.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES,
    )
    return translation_unit, dmeta_uses


def parse_reflection_header(
    module_name: str,
    header: str,
    exported_symbols: dict[str, object] | None = None,
    export_mode: bool = False,
) -> ReflectedHeaderInfo:
    module_config = configs.get_module_config(module_name)
    header_path = (module_config.module_dir / header).resolve()
    source = header_path.read_text(encoding="utf-8")
    tu, dmeta_uses = _parse_translation_unit(module_name, header_path, source, export_mode)

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
                for prop in _scan_source_properties_for_class(source, child, exported_symbols):
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
                    base_qualified_name=_base_qualified_name(child),
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
                for prop in _scan_source_properties_for_class(source, child, exported_symbols):
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
