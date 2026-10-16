from durin_header_tool.model.function_signature import function_cpp_type, function_parameter_cpp_type
from durin_header_tool.writers.reflection_writer_common import _cpp_string_literal, _line


def function_declarations(function, owner: str) -> str:
    traits = f"FMethod_{function.name}"
    lines = [_line(f"using {traits} = Durin::DurinCodeGen::TNativeFunctionTraits<decltype(&{owner}::{function.name})>;", 1),
             _line(f"static_assert(std::is_same_v<std::remove_cv_t<typename {traits}::ReturnType>, "
                   f"{function_cpp_type(function.return_kind)}>);", 1),
             _line(f"static_assert({traits}::IsConst == {'true' if function.is_const else 'false'});", 1)]
    lines.append(_line(f"static_assert({traits}::NumArguments == {len(function.parameters)});", 1))
    for index, parameter in enumerate(function.parameters):
        cpp = function_parameter_cpp_type(parameter.kind, parameter.passing)
        lines.append(_line(f"static_assert(std::is_same_v<typename {traits}::template ArgumentType<{index}>, {cpp}>);", 1))
    has_storage = bool(function.parameters) or function.return_kind != "Void"
    if has_storage:
        aggregate = f"FParams_{function.name}"
        lines.append(_line(f"struct {aggregate}", 1))
        lines.append(_line("{", 1))
        for index, parameter in enumerate(function.parameters):
            lines.append(_line(f"{function_cpp_type(parameter.kind)} P{index}{{}};", 2))
        if function.return_kind != "Void":
            lines.append(_line(f"{function_cpp_type(function.return_kind)} ReturnValue{{}};", 2))
        lines.append(_line("};", 1))
        lines.append(_line(f"static auto MakeSignature_{function.name}() -> Durin::FFunctionSignature", 1))
        lines.append(_line("{", 1))
        parameters = []
        for index, parameter in enumerate(function.parameters):
            prop = (f"Durin::DurinCodeGen::MakeFunctionProperty<{function_cpp_type(parameter.kind)}>("
                    f"{_cpp_string_literal(parameter.name)}, static_cast<::uint16>(STRUCT_OFFSET({aggregate}, P{index})))")
            parameters.append(f"{{ {prop}, Durin::EFunctionParameterPassing::{parameter.passing} }}")
        result = "{}"
        if function.return_kind != "Void":
            result = (f"Durin::DurinCodeGen::MakeFunctionProperty<{function_cpp_type(function.return_kind)}>("
                      f'"ReturnValue", static_cast<::uint16>(STRUCT_OFFSET({aggregate}, ReturnValue)))')
        lines.append(_line(f"return Durin::DurinCodeGen::MakeFunctionSignature<{aggregate}>("
                           + "{ " + ", ".join(parameters) + f" }}, {result});", 2))
        lines.append(_line("}", 1))
    frame = "Durin::FFunctionFrame&" + (" Frame" if function.parameters else "")
    result = "void*" + (" Result" if function.return_kind != "Void" else "")
    arguments = ", ".join(f"Frame.GetParameter<{function_cpp_type(p.kind)}>({i})"
                          for i, p in enumerate(function.parameters))
    target = ("const " if function.is_const else "") + owner
    call = f"static_cast<{target}&>(Target).{function.name}({arguments})"
    if function.return_kind != "Void":
        call = f"*static_cast<{function_cpp_type(function.return_kind)}*>(Result) = " + call
    lines.append(_line(f"static void Invoke_{function.name}(Durin::DObject& Target, {frame}, {result}) {{ {call}; }}", 1))
    return "".join(lines)


def signature_factory(function, statics: str) -> str:
    return (f"&{statics}::MakeSignature_{function.name}"
            if function.parameters or function.return_kind != "Void" else "nullptr")
