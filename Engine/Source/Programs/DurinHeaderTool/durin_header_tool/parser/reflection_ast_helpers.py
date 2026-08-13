import re

import clang.cindex
from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner

_GENERATED_BODY_PATTERN = re.compile(r"\bGENERATED_BODY\s*\(")


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
