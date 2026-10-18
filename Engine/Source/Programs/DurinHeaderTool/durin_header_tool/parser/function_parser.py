"""The deliberately small native function signature contract."""
from durin_header_tool.parser.script_exposure import script_exposure
import re

import clang.cindex as clang

from durin_header_tool.model.reflection_info import ReflectedFunctionInfo, ReflectedFunctionParameterInfo
from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner
from durin_header_tool.parser.reflection_ast_helpers import _get_annotation
from durin_header_tool.model.function_signature import validate_function_type, resolve_function_type, FUNCTION_VALUE_KINDS, FUNCTION_OBJECT_WRAPPERS
from durin_header_tool.model.function_defaults import reflected_default
from durin_header_tool.parser.property_parser import _split_template_args


def function_metadata(payload: str, location: str) -> dict:
    result = script_exposure(CppSourceScanner(payload).split_macro_arguments(), {"ScriptCallable", "ScriptName", "ScriptNoExport"}, location)
    for entry in CppSourceScanner(payload).split_macro_arguments():
        key, separator, value = entry.strip().partition("=")
        key = key.strip()
        if key.startswith("Script"):
            continue
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


def _declaration_tokens(member) -> list[str]:
    result = []
    parentheses = 0
    for token in member.get_tokens():
        if token.spelling == "{" and parentheses == 0:
            break
        result.append(token.spelling)
        parentheses += (token.spelling == "(") - (token.spelling == ")")
    return result


def _value_type(tokens: list[str], *, allow_reference: bool, symbols=None, namespace="", allow_unknown=False):
    if not tokens or any(token in tokens for token in ("&&", "[", "]", "=", "volatile")):
        return None
    passing = "Value"
    if "&" in tokens:
        if not allow_reference or tokens[-1] != "&" or tokens.count("&") != 1:
            return None
        passing = "ConstReference" if "const" in tokens else "Reference"
        tokens = tokens[:-1]
    # Only top-level const is a passing qualifier. Const pointees and nested const
    # values are different types and cannot be erased from the reflected schema.
    if tokens and tokens[-1] == "const":
        tokens = tokens[:-1]
    if tokens and tokens[0] == "const":
        if tokens[-1] == "*":
            return None
        tokens = tokens[1:]
    if "const" in tokens:
        return None
    spelling = " ".join(tokens).replace(" :: ", "::").replace(":: ", "::").lstrip(":")
    kind = _parse_value_spelling(spelling, allow_unknown=allow_unknown or bool(symbols))
    if kind and symbols is not None and not allow_unknown:
        try:
            kind = resolve_function_type(kind, symbols, namespace)
            if not validate_function_type(kind):
                return None
        except ValueError:
            return None
    return (kind, passing) if kind else None


def _parse_value_spelling(spelling, *, allow_unknown, depth=0):
    spelling = re.sub(r"\s*([<>,*])\s*", r"\1", spelling).strip()
    primitive = FUNCTION_VALUE_KINDS.get(spelling.removeprefix("Durin::"))
    if primitive:
        return primitive
    templates = [("std::vector", "Array"), ("std::unordered_map", "Map")]
    templates.extend((prefix + template, kind) for kind, template in FUNCTION_OBJECT_WRAPPERS.items()
                     for prefix in ("", "Durin::"))
    for template, kind in templates:
        if spelling.startswith(template + "<") and spelling.endswith(">"):
            args = _split_template_args(spelling[len(template) + 1:-1])
            if kind in FUNCTION_OBJECT_WRAPPERS:
                value = {"Kind": kind, "Name": args[0].lstrip(":")} if len(args) == 1 and allow_unknown else None
            else:
                if depth >= 4:
                    return None
                count = 1 if kind == "Array" else 2
                if len(args) != count:
                    return None
                values = [_parse_value_spelling(a.lstrip(":"), allow_unknown=allow_unknown, depth=depth + 1) for a in args]
                value = ({"Kind": kind, "Inner": values[0]} if count == 1 else
                         {"Kind": kind, "Key": values[0], "Value": values[1]})
            return value if validate_function_type(value, depth, allow_unresolved=True) else None
    if allow_unknown:
        value = {"Kind": "Object" if spelling.endswith("*") else "Named",
                 "Name": spelling.removesuffix("*").strip().lstrip(":")}
        return value if validate_function_type(value, depth, allow_unresolved=True) else None
    return None


def _signature_parts(member, *, require_concrete: bool, symbols=None, namespace="", allow_unknown=False):
    """One signature contract for annotations and unannotated inherited methods."""
    if member.kind != clang.CursorKind.CXX_METHOD:
        return None
    if (member.is_static_method()
            or (require_concrete and member.is_pure_virtual_method())
            or member.type.is_function_variadic()
            or member.type.get_ref_qualifier() != clang.RefQualifierKind.NONE):
        return None
    tokens = _declaration_tokens(member)
    if (not re.fullmatch(r"[A-Za-z_]\w*", member.spelling)
            or "consteval" in tokens
            or any(a == "=" and b == "delete" for a, b in zip(tokens, tokens[1:]))):
        return None
    try:
        name_index = next(i for i, token in enumerate(tokens[:-1])
                          if token == member.spelling and tokens[i + 1] == "(")
    except StopIteration:
        return None
    depth, close = 0, name_index + 1
    for close in range(name_index + 1, len(tokens)):
        depth += (tokens[close] == "(") - (tokens[close] == ")")
        if depth == 0:
            break
    arrow, depth = len(tokens), 0
    for index in range(close + 1, len(tokens)):
        depth += (tokens[index] == "(") - (tokens[index] == ")")
        if tokens[index] == "->" and depth == 0:
            arrow = index
            break
    qualifiers = tokens[close + 1:arrow]
    if "volatile" in qualifiers:
        return None
    prefix = tokens[:name_index]
    # The annotation rewriter may put an attribute in the declaration extent.
    if "__attribute__" in prefix:
        start = prefix.index("__attribute__") + 1
        depth, end = 0, start
        for end in range(start, len(prefix)):
            depth += (prefix[end] == "(") - (prefix[end] == ")")
            if depth == 0:
                break
        prefix = prefix[end + 1:]
    prefix = [token for token in prefix if token not in ("virtual", "inline", "constexpr")
              and not token.endswith("_API")]
    if arrow < len(tokens):
        prefix = []
        for token in tokens[arrow + 1:]:
            if token in (";", "override", "final", "noexcept", "="):
                break
            prefix.append(token)
    context = dict(symbols=symbols, namespace=namespace, allow_unknown=allow_unknown)
    result = _value_type(prefix, allow_reference=False, **context)
    if not result:
        return None
    parameters = []
    for index, argument in enumerate(member.get_arguments()):
        parameter_tokens = [token.spelling for token in argument.get_tokens()]
        default_expression = None
        if "=" in parameter_tokens:
            equals = parameter_tokens.index("=")
            default_expression = " ".join(parameter_tokens[equals + 1:])
            parameter_tokens = parameter_tokens[:equals]
        if argument.spelling:
            if not parameter_tokens or parameter_tokens[-1] != argument.spelling:
                return None
            parameter_tokens = parameter_tokens[:-1]
        elif require_concrete:
            return None
        value = _value_type(parameter_tokens, allow_reference=True, **context)
        if not value or value[0] == "Void":
            return None
        parameter = ReflectedFunctionParameterInfo(argument.spelling or f"p{index}", *value,
                                                   default_expression=default_expression)
        parameter.default_value = reflected_default(parameter, symbols)
        parameters.append(parameter)
    return result[0], parameters, member.is_const_method()


def _native_signature(member, *, require_concrete: bool, **context) -> dict | None:
    parts = _signature_parts(member, require_concrete=require_concrete, **context)
    if parts is None:
        return None
    result, parameters, is_const = parts
    return {"Const": is_const, "Return": result,
            "Parameters": [{"Type": p.kind, "Passing": p.passing} for p in parameters]}


def parse_functions(cursor, header: str, symbols=None, namespace="", allow_unknown=False) -> list[ReflectedFunctionInfo]:
    members = list(cursor.get_children())
    functions = []
    for member in members:
        annotation = _get_annotation(member)
        if not annotation.startswith("DFUNCTION"):
            continue
        location = f"{header}:{member.location.line}: DFUNCTION {cursor.spelling}::{member.spelling}"
        parts = _signature_parts(member, require_concrete=True, symbols=symbols, namespace=namespace, allow_unknown=allow_unknown)
        if parts is None:
            raise ValueError(f"{location}: unsupported native function signature or parameter/default argument")
        return_kind, parameters, is_const = parts
        same_name = [m for m in members if m.spelling == member.spelling]
        if (sum(_get_annotation(m).startswith("DFUNCTION") for m in same_name) != 1
                or any(m.kind not in (clang.CursorKind.CXX_METHOD, clang.CursorKind.FUNCTION_TEMPLATE)
                       for m in same_name)):
            raise ValueError(f"{location}: reflected overloads are unsupported")
        metadata = function_metadata(annotation.partition(",")[2], location)
        # An override can be semantically unresolved in the hermetic export parse.
        declaration_tokens = _declaration_tokens(member)
        is_override = "override" in declaration_tokens or "final" in declaration_tokens
        functions.append(ReflectedFunctionInfo(
            name=member.spelling, source_line=member.location.line,
            is_virtual=member.is_virtual_method() or is_override,
            is_final="final" in declaration_tokens, return_kind=return_kind,
            parameters=parameters, is_const=is_const, return_const=member.result_type.is_const_qualified(),
            call_in_editor=metadata.get("CallInEditor", False),
            script={key: value for key, value in metadata.items() if key.startswith("Script")},
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
        signature = _native_signature(member, require_concrete=False, symbols=symbols, namespace=owner.namespace)
        compatible = is_override and function["Virtual"] and signature == function["Signature"]
        if function["Final"]:
            raise ValueError(
                f"{header}:{member.location.line}: {owner.qualified_name}::{member.spelling} "
                f"cannot override final reflected ancestor {base.QualifiedName}::{member.spelling} "
                f"at {base.Header}:{function['Line']}")
        if not compatible:
            raise ValueError(
                f"{header}:{member.location.line}: {owner.qualified_name}::{member.spelling} "
                f"hides reflected ancestor {base.QualifiedName}::{member.spelling} "
                f"at {base.Header}:{function['Line']}; requires a compatible virtual override")
