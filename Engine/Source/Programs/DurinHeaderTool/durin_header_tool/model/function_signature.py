"""Versioned recursive function value semantics, independent of C++ layout."""
import re

FUNCTION_CPP_TYPES = {
    "Bool": "bool", "Int8": "::int8", "Int16": "::int16", "Int32": "::int32", "Int64": "::int64",
    "UInt8": "::uint8", "UInt16": "::uint16", "UInt32": "::uint32", "UInt64": "::uint64",
    "Float": "float", "Double": "double", "String": "std::string",
    "Name": "::Durin::FName", "Guid": "::Durin::FGuid", "Void": "void",
}


# Derive canonical source spellings from the emitted C++ types; keep only aliases here.
FUNCTION_VALUE_KINDS = {cpp.lstrip(":").removeprefix("Durin::"): kind
                        for kind, cpp in FUNCTION_CPP_TYPES.items()}
FUNCTION_VALUE_KINDS.update({
    "int": "Int32", "signed int": "Int32", "unsigned int": "UInt32",
    "short": "Int16", "unsigned short": "UInt16", "signed char": "Int8", "unsigned char": "UInt8",
})
FUNCTION_OBJECT_WRAPPERS = {"ObjectPtr": "TObjectPtr", "SoftObject": "TSoftObjectPtr", "WeakObject": "TWeakObjectPtr"}


def validate_function_type(value, depth=0, *, allow_unresolved=False):
    if isinstance(value, str):
        return value in FUNCTION_CPP_TYPES
    if not isinstance(value, dict) or depth > 4:
        return False
    kind = value.get("Kind")
    if kind in ("Struct", "Enum", "Object", *FUNCTION_OBJECT_WRAPPERS) or (allow_unresolved and kind == "Named"):
        return (set(value) == {"Kind", "Name"} and isinstance(value["Name"], str)
                and re.fullmatch(r"[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*", value["Name"]) is not None)
    if kind == "Array":
        return (set(value) == {"Kind", "Inner"} and depth < 4
                and value["Inner"] not in ("Void", "Bool")
                and validate_function_type(value["Inner"], depth + 1, allow_unresolved=allow_unresolved))
    if kind == "Map":
        return (set(value) == {"Kind", "Key", "Value"} and depth < 4
                and ((isinstance(value["Key"], str) and value["Key"] in FUNCTION_CPP_TYPES and value["Key"] != "Void")
                     or (isinstance(value["Key"], dict) and value["Key"].get("Kind") in (("Enum", "Named") if allow_unresolved else ("Enum",))
                         and validate_function_type(value["Key"], depth + 1, allow_unresolved=allow_unresolved)))
                and value["Value"] != "Void"
                and validate_function_type(value["Value"], depth + 1, allow_unresolved=allow_unresolved))
    return False


def function_cpp_type(value, *, parser=False):
    if isinstance(value, str):
        cpp = FUNCTION_CPP_TYPES[value]
        return "::Durin" + cpp if parser and cpp.startswith(("::int", "::uint")) else cpp
    kind = value["Kind"]
    if kind == "Array":
        return f"std::vector<{function_cpp_type(value['Inner'], parser=parser)}>"
    if kind == "Map":
        return (f"std::unordered_map<{function_cpp_type(value['Key'], parser=parser)}, "
                f"{function_cpp_type(value['Value'], parser=parser)}>")
    if kind == "Named":
        raise ValueError("Unresolved function type cannot be emitted")
    cpp = "::" + value["Name"]
    return f"::Durin::{FUNCTION_OBJECT_WRAPPERS[kind]}<{cpp}>" if kind in FUNCTION_OBJECT_WRAPPERS else cpp + ("*" if kind == "Object" else "")


def function_type_names(value):
    if isinstance(value, dict):
        if "Name" in value:
            yield value["Name"]
        for key in ("Inner", "Key", "Value"):
            if key in value:
                yield from function_type_names(value[key])


def resolve_function_type(value, symbols, namespace):
    from durin_header_tool.resolver.reflection_resolver import resolve_symbol, symbol_resolution_diagnostic
    if isinstance(value, str):
        return value
    value = dict(value)
    if "Name" in value:
        kinds = {"Named": ("struct", "enum"), "Struct": ("struct",), "Enum": ("enum",)}.get(value["Kind"], ("class",))
        resolution = resolve_symbol(value["Name"], symbols, declaring_namespace=namespace, kinds=kinds)
        if not resolution.resolved:
            raise ValueError(symbol_resolution_diagnostic(resolution, "DFUNCTION", "function value type"))
        value["Name"] = resolution.qualified_name
        if value["Kind"] == "Named":
            value["Kind"] = "Enum" if symbols[resolution.qualified_name].Kind == "enum" else "Struct"
    for key in ("Inner", "Key", "Value"):
        if key in value:
            value[key] = resolve_function_type(value[key], symbols, namespace)
    return value


def validate_function_signature(signature: object, *, allow_unresolved=False) -> bool:
    if (not isinstance(signature, dict) or set(signature) != {"Const", "Return", "Parameters"}
            or type(signature["Const"]) is not bool
            or not validate_function_type(signature["Return"], allow_unresolved=allow_unresolved)
            or not isinstance(signature["Parameters"], list)):
        return False
    for parameter in signature["Parameters"]:
        if (not isinstance(parameter, dict) or set(parameter) != {"Type", "Passing"}
                or parameter["Type"] == "Void" or not validate_function_type(parameter["Type"], allow_unresolved=allow_unresolved)
                or parameter["Passing"] not in ("Value", "ConstReference", "Reference")):
            return False
    return True


def function_parameter_cpp_type(value, passing, *, parser=False):
    return function_cpp_type(value, parser=parser) + {"Value": "", "ConstReference": " const&", "Reference": "&"}[passing]


def synthetic_function_declaration(function: dict) -> str:
    signature = function["Signature"]
    arguments = []
    for index, parameter in enumerate(signature["Parameters"]):
        cpp = function_parameter_cpp_type(parameter["Type"], parameter["Passing"], parser=True)
        arguments.append(f"{cpp} p{index}")
    return (("virtual " if function["Virtual"] else "") + function_cpp_type(signature["Return"], parser=True)
            + f" {function['Name']}({', '.join(arguments)})"
            + (" const" if signature["Const"] else "") + (" final" if function["Final"] else "") + ";")
