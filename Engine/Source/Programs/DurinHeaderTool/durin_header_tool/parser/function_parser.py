"""The deliberately small native function signature contract."""
import re

import clang.cindex as clang

from durin_header_tool.model.reflection_info import ReflectedFunctionInfo
from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner
from durin_header_tool.parser.reflection_ast_helpers import _get_annotation


def function_metadata(payload: str, location: str) -> dict:
    result = {}
    for entry in CppSourceScanner(payload).split_macro_arguments():
        key, separator, value = entry.strip().partition("=")
        key = key.strip()
        if key in result:
            raise ValueError(f"{location}: duplicate {key}")
        if key == "CallInEditor" and not separator:
            result[key] = True
        elif key in ("Category", "DisplayName", "ToolTip") and separator:
            match = re.fullmatch(r'"((?:\\.|[^"\\])*)"', value.strip())
            if not match:
                raise ValueError(f"{location}: {key} requires a quoted string")
            result[key] = CppSourceScanner.unescape_string_literal(match.group(1))
        else:
            raise ValueError(f"{location}: unsupported function specifier '{key}'")
    return result


def parse_functions(cursor, header: str) -> list[ReflectedFunctionInfo]:
    members = list(cursor.get_children())
    functions = []
    for member in members:
        annotation = _get_annotation(member)
        if not annotation.startswith("DFUNCTION"):
            continue
        location = f"{header}:{member.location.line}: DFUNCTION {cursor.spelling}::{member.spelling}"
        if member.kind != clang.CursorKind.CXX_METHOD:
            raise ValueError(f"{location}: only ordinary class member functions are supported")
        if (member.is_static_method() or member.is_const_method()
                or member.is_pure_virtual_method() or list(member.get_arguments())
                or member.result_type.kind != clang.TypeKind.VOID
                or member.type.is_function_variadic()
                or member.type.get_ref_qualifier() != clang.RefQualifierKind.NONE):
            raise ValueError(f"{location}: requires a non-static, non-const, concrete void Method()")
        if sum(m.spelling == member.spelling for m in members) != 1:
            raise ValueError(f"{location}: reflected overloads are unsupported")
        metadata = function_metadata(annotation.partition(",")[2], location)
        # An override can be semantically unresolved in the hermetic export parse.
        declaration_tokens = []
        for token in member.get_tokens():
            if token.spelling == "{":
                break
            declaration_tokens.append(token.spelling)
        if (not re.fullmatch(r"[A-Za-z_]\w*", member.spelling)
                or "volatile" in declaration_tokens or "consteval" in declaration_tokens
                or any(a == "=" and b == "delete" for a, b in zip(declaration_tokens, declaration_tokens[1:]))):
            raise ValueError(f"{location}: requires a concrete ordinary unqualified member function")
        is_override = "override" in declaration_tokens or "final" in declaration_tokens
        functions.append(ReflectedFunctionInfo(
            name=member.spelling, source_line=member.location.line,
            is_virtual=member.is_virtual_method() or is_override,
            call_in_editor=metadata.get("CallInEditor", False),
            category=metadata.get("Category", "Actions"),
            display_name=metadata.get("DisplayName", ""), tool_tip=metadata.get("ToolTip", "")))
    return functions


def validate_function_inheritance(cursor, owner, header: str, symbols) -> None:
    """Validate local names against reflected ancestors without included headers."""
    inherited = {}
    seen = set()
    base_name = owner.base_qualified_name
    while base_name and base_name not in seen:
        seen.add(base_name)
        base = symbols.get(base_name)
        if not base:
            break
        for function in base.Functions:
            inherited.setdefault(function["Name"], (base, function))
        base_name = base.BaseQualifiedName
    for member in cursor.get_children():
        if member.spelling not in inherited:
            continue
        base, function = inherited[member.spelling]
        is_override = (member.kind == clang.CursorKind.CXX_METHOD
                       and (member.is_virtual_method() or any(
                           c.kind == clang.CursorKind.CXX_OVERRIDE_ATTR for c in member.get_children())))
        compatible = (is_override and function["Virtual"] and not member.is_static_method()
                      and not member.is_const_method() and not list(member.get_arguments())
                      and member.result_type.kind == clang.TypeKind.VOID
                      and member.type.get_ref_qualifier() == clang.RefQualifierKind.NONE)
        if not compatible:
            raise ValueError(
                f"{header}:{member.location.line}: {owner.qualified_name}::{member.spelling} "
                f"hides reflected ancestor {base.QualifiedName}::{member.spelling} "
                f"at {base.Header}:{function['Line']}; requires a compatible virtual override")
