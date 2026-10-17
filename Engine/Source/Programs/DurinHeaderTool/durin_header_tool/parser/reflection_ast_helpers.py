import re

import clang.cindex
from durin_header_tool.model.reflection_info import NamespaceSegment
from durin_header_tool.parser.source_context import HeaderSourceContext, source_context_for


def _get_annotation(cursor: clang.cindex.Cursor) -> str:
    for child in cursor.get_children():
        if child.kind == clang.cindex.CursorKind.ANNOTATE_ATTR:
            return child.spelling
    return ""


def _semantic_namespace_path(cursor: clang.cindex.Cursor) -> tuple[NamespaceSegment, ...]:
    segments: list[NamespaceSegment] = []
    parent = cursor.semantic_parent
    while parent and parent.kind != clang.cindex.CursorKind.TRANSLATION_UNIT:
        if parent.kind == clang.cindex.CursorKind.NAMESPACE:
            if not parent.spelling:
                raise ValueError(
                    f"Reflected type '{cursor.spelling}' cannot be declared in an anonymous namespace."
                )
            is_inline = False
            inline_query = getattr(parent, "is_inline_namespace", None)
            if inline_query is not None:
                is_inline = bool(inline_query())
            if not is_inline:
                declaration_tokens: list[str] = []
                for token in parent.get_tokens():
                    if token.spelling == "{":
                        break
                    declaration_tokens.append(token.spelling)
                is_inline = "inline" in declaration_tokens
            segments.append(NamespaceSegment(parent.spelling, is_inline))
        elif parent.kind in (
            clang.cindex.CursorKind.CLASS_DECL,
            clang.cindex.CursorKind.STRUCT_DECL,
            clang.cindex.CursorKind.CLASS_TEMPLATE,
        ):
            raise ValueError(
                f"Reflected type '{cursor.spelling}' cannot be declared in class scope."
            )
        parent = parent.semantic_parent
    return tuple(reversed(segments))


def _semantic_namespace(cursor: clang.cindex.Cursor) -> str:
    return "::".join(segment.name for segment in _semantic_namespace_path(cursor))


def _qualified_name(cursor: clang.cindex.Cursor) -> str:
    namespace = _semantic_namespace(cursor)
    return f"{namespace}::{cursor.spelling}" if namespace else cursor.spelling


def _cursor_source_line_range(source: str, cursor: clang.cindex.Cursor, *,
                              context: HeaderSourceContext | None = None) -> tuple[int, int]:
    context = source_context_for(source, context)
    return context.scope_line_range(cursor.extent.start.line, cursor.extent.start.column)


def _scan_generated_body_line(source: str, class_cursor: clang.cindex.Cursor, *,
                              context: HeaderSourceContext | None = None) -> int:
    context = source_context_for(source, context)
    start_line, end_line = context.scope_line_range(class_cursor.extent.start.line, class_cursor.extent.start.column)
    if start_line == 0:
        return 0

    # Synthetic member locations can collapse to the class declaration in PCH or
    # error-recovery ASTs. The cursor extent selects the class; source owns macro lines.
    return context.generated_body_line(start_line, end_line)


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


def _source_base_name(source: str, class_cursor: clang.cindex.Cursor, *,
                      context: HeaderSourceContext | None = None) -> str:
    lines = source_context_for(source, context).lines
    start = max(class_cursor.location.line - 1, 0)
    declaration = " ".join(lines[start:min(start + 6, len(lines))])
    declaration = declaration.split("{", 1)[0]
    match = re.search(
        rf"\b(?:class|struct)\s+(?:[A-Za-z_]\w*_API\s+)?{re.escape(class_cursor.spelling)}"
        r"\s*(?:final\s*)?:\s*(?:(?:public|protected|private)\s+)?([A-Za-z_]\w*(?:::\w+)*)",
        declaration,
    )
    return match.group(1) if match else ""
