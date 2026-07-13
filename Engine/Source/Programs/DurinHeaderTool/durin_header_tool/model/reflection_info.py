from dataclasses import dataclass, field
from pathlib import Path
import re

import clang.cindex

from durin_header_tool import config as configs
from durin_header_tool import io as utils

SYMBOL_NAME_SCHEME = "qualified-underscore-v1"
TOOL_VERSION = "15"
MAX_CONTAINER_PROPERTY_DEPTH = 4


@dataclass
class ReflectedEnumValueInfo:
    name: str
    value: int


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


_DCLASS_PATTERN = re.compile(r"\bDCLASS\s*\(([^)]*)\)")
_DSTRUCT_PATTERN = re.compile(r"\bDSTRUCT\s*\(([^)]*)\)")
_DENUM_PATTERN = re.compile(r"\bDENUM\s*\(([^)]*)\)")
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


def make_dht_parse_source(source: str) -> str:
    source = _GEN_INCLUDE_PATTERN.sub("", source)
    source = _DCLASS_PATTERN.sub(lambda m: f'__attribute__((annotate("{_annotation_payload("DCLASS", m.group(1))}"))) void DHT_CLASS();', source)
    source = _DSTRUCT_PATTERN.sub(lambda m: f'__attribute__((annotate("{_annotation_payload("DSTRUCT", m.group(1))}"))) void DHT_STRUCT();', source)
    source = _DENUM_PATTERN.sub(lambda m: f'__attribute__((annotate("{_annotation_payload("DENUM", m.group(1))}"))) void DHT_ENUM();', source)
    source = _DPROPERTY_PATTERN.sub(lambda m: f'__attribute__((annotate("{_annotation_payload("DPROPERTY", m.group(1))}")))', source)
    source = _GENERATED_BODY_PATTERN.sub("void DHT_GENERATED_BODY();", source)
    return source


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


def _scan_generated_body_line(source: str, class_name: str) -> int:
    class_seen = False
    for index, line in enumerate(source.splitlines(), start=1):
        if not class_seen and re.search(rf"\b(class|struct)\s+(?:\w+_API\s+)?{re.escape(class_name)}\b", line):
            class_seen = True
        if class_seen and "GENERATED_BODY" in line:
            return index
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
        line = line.split("=", 1)[0].rstrip(";").strip()
        match = re.match(rf"(.+?)\s+{re.escape(field_cursor.spelling)}(?:\s*\[[^\]]+\])?$", line)
        if match:
            return _normalize_type_spelling(match.group(1))
    return ""


def _make_property_from_source_decl(
    source_line: str,
    annotation: str,
    exported_symbols: dict[str, object] | None,
) -> ReflectedPropertyInfo | None:
    line = source_line.split("=", 1)[0].rstrip(";").strip()
    match = re.match(r"(.+?)\s+(\w+)(?:\s*\[(\d+)\])?$", line)
    if not match:
        return None
    type_spelling = _normalize_type_spelling(match.group(1))
    name = match.group(2)
    array_dim = int(match.group(3)) if match.group(3) else 1
    return _source_property_from_type_spelling(
        name,
        type_spelling,
        exported_symbols,
        flags=_property_flags_from_annotation(annotation),
        array_dim=array_dim,
    )


def _scan_source_properties_for_class(
    source: str,
    class_name: str,
    exported_symbols: dict[str, object] | None,
) -> list[ReflectedPropertyInfo]:
    properties: list[ReflectedPropertyInfo] = []
    class_seen = False
    in_class = False
    brace_depth = 0
    pending_annotation = ""

    for line in source.splitlines():
        stripped = line.strip()
        if not class_seen:
            if re.search(rf"\b(class|struct)\s+(?:\w+_API\s+)?{re.escape(class_name)}\b", stripped):
                class_seen = True
            else:
                continue

        if class_seen and not in_class:
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


def _make_enum(enum_cursor: clang.cindex.Cursor, module_config, header: str) -> ReflectedEnumInfo:
    qualified_name = _qualified_name(enum_cursor)
    underlying_type = _normalize_type_spelling(enum_cursor.enum_type.spelling)
    values: list[ReflectedEnumValueInfo] = []
    for child in enum_cursor.get_children():
        if child.kind == clang.cindex.CursorKind.ENUM_CONSTANT_DECL:
            values.append(ReflectedEnumValueInfo(name=child.spelling, value=int(child.enum_value)))
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
        return _source_property_from_type_spelling(
            field_cursor.spelling,
            source_type,
            exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor),
        )

    prop = _make_property_from_type(
        field_cursor.spelling,
        _element_type(field_cursor),
        exported_symbols,
        flags=_property_flags_from_annotation(annotation),
        array_dim=_array_dim(field_cursor),
    )
    if prop:
        return prop

    if source_type:
        return _source_property_from_type_spelling(
            field_cursor.spelling,
            source_type,
            exported_symbols,
            flags=_property_flags_from_annotation(annotation),
            array_dim=_array_dim(field_cursor),
        )
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


def _parse_translation_unit(module_name: str, header_path: Path, source: str, export_mode: bool) -> clang.cindex.TranslationUnit:
    _init_clang()
    index = clang.cindex.Index.create()
    parsed_source = make_dht_parse_source(source)
    unsaved_files = [(str(header_path), parsed_source)]
    unsaved_files.extend(_fake_generated_headers(module_name))
    return index.parse(str(header_path), args=_clang_args(module_name, export_mode), unsaved_files=unsaved_files)


def parse_reflection_header(
    module_name: str,
    header: str,
    exported_symbols: dict[str, object] | None = None,
    export_mode: bool = False,
) -> ReflectedHeaderInfo:
    module_config = configs.get_module_config(module_name)
    header_path = (module_config.module_dir / header).resolve()
    source = header_path.read_text(encoding="utf-8")
    tu = _parse_translation_unit(module_name, header_path, source, export_mode)

    classes: list[ReflectedClassInfo] = []
    enums: list[ReflectedEnumInfo] = []
    structs: list[ReflectedStructInfo] = []

    def visit(parent: clang.cindex.Cursor) -> None:
        children = list(parent.get_children())
        pending_dclass = False
        pending_dstruct = False
        pending_denum = False
        for child in children:
            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling == "DHT_CLASS":
                pending_dclass = _get_annotation(child).startswith("DCLASS")
                continue
            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling == "DHT_STRUCT":
                pending_dstruct = _get_annotation(child).startswith("DSTRUCT")
                continue
            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling == "DHT_ENUM":
                pending_denum = _get_annotation(child).startswith("DENUM")
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
                    generated_body_line=_scan_generated_body_line(source, child.spelling),
                )
                for member in child.get_children():
                    if member.kind == clang.cindex.CursorKind.FIELD_DECL:
                        prop = _make_property(member, exported_symbols, source)
                        if prop:
                            reflected_struct.properties.append(prop)
                existing_property_names = {prop.name for prop in reflected_struct.properties}
                for prop in _scan_source_properties_for_class(source, child.spelling, exported_symbols):
                    if prop.name not in existing_property_names:
                        reflected_struct.properties.append(prop)
                        existing_property_names.add(prop.name)
                structs.append(reflected_struct)
                pending_dstruct = False
                continue

            if pending_dclass and child.kind in (clang.cindex.CursorKind.CLASS_DECL, clang.cindex.CursorKind.STRUCT_DECL) and child.spelling:
                qualified_name = _qualified_name(child)
                helper_name = make_generated_helper_name(qualified_name)
                reflected_class = ReflectedClassInfo(
                    short_name=child.spelling,
                    namespace=_semantic_namespace(child),
                    qualified_name=qualified_name,
                    generated_helper_name=helper_name,
                    header=header,
                    api=module_config.api_macro,
                    base_qualified_name=_base_qualified_name(child),
                    generated_body_line=_scan_generated_body_line(source, child.spelling),
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
                for prop in _scan_source_properties_for_class(source, child.spelling, exported_symbols):
                    if prop.name not in existing_property_names:
                        reflected_class.properties.append(prop)
                        existing_property_names.add(prop.name)
                classes.append(reflected_class)
                pending_dclass = False
                continue

            if pending_denum and child.kind == clang.cindex.CursorKind.ENUM_DECL and child.spelling:
                enums.append(_make_enum(child, module_config, header))
                pending_denum = False
                continue

            if child.kind in (clang.cindex.CursorKind.NAMESPACE, clang.cindex.CursorKind.TRANSLATION_UNIT):
                visit(child)

    visit(tu.cursor)

    rel = Path(header)
    include_path = _include_path_for_header(header)
    file_id = _file_id_for_header(module_name, header)
    return ReflectedHeaderInfo(module_name, header, header_path, include_path, file_id, classes, enums, structs)
