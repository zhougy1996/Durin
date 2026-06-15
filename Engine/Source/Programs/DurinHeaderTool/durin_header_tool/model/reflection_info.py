from dataclasses import dataclass, field
from pathlib import Path
import re

import clang.cindex

from durin_header_tool import config as configs
from durin_header_tool import io as utils

SYMBOL_NAME_SCHEME = "qualified-underscore-v1"
TOOL_VERSION = "2"


@dataclass
class ReflectedPropertyInfo:
    name: str
    type_name: str
    kind: str
    referenced_type: str = ""
    array_dim: int = 1
    element_size: int = 0
    flags: str = "None"


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
class ReflectedHeaderInfo:
    module_name: str
    header: str
    header_path: Path
    include_path: str
    file_id: str
    classes: list[ReflectedClassInfo] = field(default_factory=list)


_DCLASS_PATTERN = re.compile(r"\bDCLASS\s*\(([^)]*)\)")
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
}

_PROPERTY_FLAG_BY_SPECIFIER = {
    "Edit": "Edit",
    "Transient": "Transient",
    "EditConst": "EditConst",
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


def _field_size(type_info: clang.cindex.Type) -> int:
    size = type_info.get_size()
    return int(size) if size and size > 0 else 0


def _enum_kind(type_info: clang.cindex.Type) -> str | None:
    decl = type_info.get_declaration()
    if decl.kind == clang.cindex.CursorKind.ENUM_DECL:
        return "Enum"
    return None


def _make_property(field_cursor: clang.cindex.Cursor, exported_symbols: dict[str, object] | None) -> ReflectedPropertyInfo | None:
    annotation = _get_annotation(field_cursor)
    if not annotation.startswith("DPROPERTY"):
        return None

    array_dim = _array_dim(field_cursor)
    element_type = _element_type(field_cursor)
    type_spelling = _normalize_type_spelling(element_type.spelling)
    canonical = _normalize_type_spelling(element_type.get_canonical().spelling)
    kind = _PROPERTY_KIND_BY_TYPE.get(type_spelling) or _PROPERTY_KIND_BY_TYPE.get(canonical)
    referenced_type = ""
    element_size = _field_size(element_type)

    if not kind:
        kind = _enum_kind(element_type)

    if not kind and type_spelling.endswith("*"):
        pointee = type_spelling[:-1].strip()
        candidates = [pointee]
        if "::" not in pointee:
            candidates.extend(name for name in (exported_symbols or {}) if name.endswith(f"::{pointee}") or name == pointee)
        for candidate in candidates:
            if not exported_symbols or candidate in exported_symbols:
                referenced_type = candidate
                kind = "Object"
                break

    if not kind:
        return None

    return ReflectedPropertyInfo(
        name=field_cursor.spelling,
        type_name=type_spelling,
        kind=kind,
        referenced_type=referenced_type,
        array_dim=array_dim,
        element_size=element_size,
        flags=_property_flags_from_annotation(annotation),
    )


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

    def visit(parent: clang.cindex.Cursor) -> None:
        children = list(parent.get_children())
        pending_dclass = False
        for child in children:
            if child.kind == clang.cindex.CursorKind.FUNCTION_DECL and child.spelling == "DHT_CLASS":
                pending_dclass = _get_annotation(child).startswith("DCLASS")
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
                        prop = _make_property(member, exported_symbols)
                        if prop:
                            reflected_class.properties.append(prop)
                classes.append(reflected_class)
                pending_dclass = False
                continue

            if child.kind in (clang.cindex.CursorKind.NAMESPACE, clang.cindex.CursorKind.TRANSLATION_UNIT):
                visit(child)

    visit(tu.cursor)

    rel = Path(header)
    include_path = _include_path_for_header(header)
    file_id = _file_id_for_header(module_name, header)
    return ReflectedHeaderInfo(module_name, header, header_path, include_path, file_id, classes)
