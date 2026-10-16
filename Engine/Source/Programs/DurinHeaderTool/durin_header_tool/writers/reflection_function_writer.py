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
        records = []
        for index, parameter in enumerate(function.parameters):
            field = f"Prop_{function.name}_P{index}"
            flags = ["Parm"]
            if parameter.passing == "ConstReference":
                flags += ["ReferenceParm", "ConstParm"]
            elif parameter.passing == "Reference":
                flags += ["ReferenceParm", "OutParm"]
            role = " | ".join(f"Durin::EPropertyFlags::{flag}" for flag in flags)
            lines.append(_line(f"inline static const Durin::DurinCodeGen::TNativePropertyParams<{function_cpp_type(parameter.kind)}> {field}"
                               f"{{{_cpp_string_literal(parameter.name)}, static_cast<::uint16>(STRUCT_OFFSET({aggregate}, P{index})), {role}}};", 1))
            records.append(f"&{field}.Params")
        if function.return_kind != "Void":
            field = f"Prop_{function.name}_ReturnValue"
            lines.append(_line(f"inline static const Durin::DurinCodeGen::TNativePropertyParams<{function_cpp_type(function.return_kind)}> {field}"
                               f'{{"ReturnValue", static_cast<::uint16>(STRUCT_OFFSET({aggregate}, ReturnValue)), '
                               "Durin::EPropertyFlags::Parm | Durin::EPropertyFlags::ReturnParm | Durin::EPropertyFlags::OutParm};", 1))
            records.append(f"&{field}.Params")
        lines.append(_line(f"inline static const Durin::DurinCodeGen::FPropertyParamsBase* const Props_{function.name}[] = {{ " + ", ".join(records) + " };", 1))
        lines.append(_line(f"inline static const auto Layout_{function.name} = Durin::DurinCodeGen::MakeFunctionLayoutParams<{aggregate}>(Props_{function.name}, std::size(Props_{function.name}));", 1))
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


def function_layout_record(function, statics: str) -> str:
    return (f"&{statics}::Layout_{function.name}"
            if function.parameters or function.return_kind != "Void" else "nullptr")
