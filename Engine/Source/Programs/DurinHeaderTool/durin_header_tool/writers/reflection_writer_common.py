from durin_header_tool.model.reflection_info import ReflectedClassInfo

TAB = "\t"

def _cpp_string_literal(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")
    return f'"{escaped}"'


def _append_macro_line(builder: list[str], content: str, indent: int = 0, last: bool = False) -> None:
    builder.append((TAB * indent) + content + ("\n" if last else " \\\n"))


def _line(content: str = "", indent: int = 0) -> str:
    return f"{TAB * indent}{content}\n"


def _constructor_mode(class_info: ReflectedClassInfo) -> str:
    if class_info.has_default_constructor:
        return "default"
    return "object_initializer"


def _base_name_for_macro(class_info: ReflectedClassInfo) -> str:
    return class_info.base_qualified_name or "Durin::DObject"


def _bool_literal(value: bool) -> str:
    return "true" if value else "false"


def _append_lines_no_indent(builder: list[str], *lines: str) -> None:
    for content in lines:
        builder.append(f"{content}\n")
