import hashlib
from pathlib import Path
import re
from typing import TypeAlias

import clang.cindex

from durin_header_tool import config as configs
from durin_header_tool.model.export_info import ExportedSymbolInfo
from durin_header_tool.parser.annotation_rewriter import _DMetaUse, _make_dht_parse_source

ExportedSymbols: TypeAlias = dict[str, ExportedSymbolInfo]
PARSER_CONTEXT_VERSION = "target-predefines-v6"
_FILE_ID_READABLE_PREFIX_LENGTH = 48
_INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\b[^\r\n]*$', re.MULTILINE)
_TYPE_DECLARATION_PATTERN = re.compile(r"\b(?:class|struct|enum(?:\s+class)?)\s+([A-Za-z_]\w*)")


def _target_predefined_macros(target: str) -> list[str]:
    if target == "Win64":
        return [
            "--target=x86_64-pc-windows-msvc",
            "-D_MSC_VER=1930",
            "-D_WIN32=1",
            "-D_WIN64=1",
        ]
    if target == "MacOS":
        return [
            "--target=arm64-apple-macos",
            "-D__APPLE__=1",
            "-D__MACH__=1",
            "-D__arm64__=1",
            "-D__aarch64__=1",
        ]
    if target == "Linux":
        return ["--target=x86_64-unknown-linux-gnu", "-D__linux__=1", "-D__x86_64__=1"]
    raise ValueError(f"unsupported DHT target architecture '{target}'")


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
        "-DFORCEINLINE=inline",
        f"-DDURIN_WITH_EDITOR={1 if configs.RUNTIME_VARIANT == 'DurinEditor' else 0}",
    ]
    args.extend(_target_predefined_macros(configs.ARCH))
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
        "_WIN64",
        "__APPLE__",
        "__MACH__",
        "__arm64__",
        "__aarch64__",
        "__linux__",
        "__x86_64__",
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
        "class FName {}; struct FGuid {}; class FObjectInitializer {}; class FByteBuffer {};",
        "template<class T> class TObjectPtr {}; template<class T> class TSoftObjectPtr {};",
        "}",
    ]
    for symbol in sorted((exported_symbols or {}).values(), key=lambda item: item.QualifiedName):
        if symbol.Header == header or symbol.ShortName in declared_names:
            continue
        lines.append(_synthetic_declaration(symbol))
    return "\n".join(lines) + "\n"


def _include_path_for_header(header: str) -> str:
    include_path = Path(header.replace("\\", "/")).as_posix()
    if include_path.startswith("Public/"):
        include_path = include_path[len("Public/"):]
    elif include_path.startswith("Private/"):
        include_path = include_path[len("Private/"):]
    return include_path


def _file_id_for_header(module_name: str, header: str) -> str:
    include_path = _include_path_for_header(header)
    identity = f"{module_name}\0{include_path}".encode("utf-8")
    digest = hashlib.sha256(identity).hexdigest()[:32]
    readable_path = re.sub(r"[^A-Za-z0-9]+", "_", include_path).strip("_")
    readable_path = readable_path[:_FILE_ID_READABLE_PREFIX_LENGTH] or "Header"
    return f"FID_DURIN_{module_name}_{readable_path}_{digest}"


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
