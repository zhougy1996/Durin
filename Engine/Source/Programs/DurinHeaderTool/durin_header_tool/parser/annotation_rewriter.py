from dataclasses import dataclass
import re

from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner

_INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\b[^\r\n]*$', re.MULTILINE)


@dataclass(frozen=True)
class _DMetaUse:
    line: int
    column: int

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
) -> tuple[bool, bool, str, str]:
    location = f"DCLASS at line {line}, column {column}"
    if not payload.strip():
        return False, False, "", ""

    entries = _macro_arguments(payload, location)

    is_abstract = False
    no_class_default_object = False
    metadata: dict[str, str] = {}
    for raw_entry in entries:
        entry = raw_entry.strip()
        if not entry:
            raise ValueError(f"{location}: empty class specifier")
        key, separator, raw_value = entry.partition("=")
        key = key.strip()
        if not separator:
            if key not in ("Abstract", "NoClassDefaultObject"):
                raise ValueError(f"{location}: unsupported class specifier '{key}'")
            if key == "Abstract":
                if is_abstract:
                    raise ValueError(f"{location}: duplicate Abstract class specifier")
                is_abstract = True
            else:
                if no_class_default_object:
                    raise ValueError(f"{location}: duplicate NoClassDefaultObject class specifier")
                no_class_default_object = True
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

    return (
        is_abstract,
        no_class_default_object,
        metadata.get("DisplayName", ""),
        metadata.get("DefaultObjectName", ""),
    )


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


def _dmeta_use_id(annotation: str) -> int | None:
    match = re.match(r"^DMETA:(\d+)(?:,|$)", annotation)
    return int(match.group(1)) if match else None
