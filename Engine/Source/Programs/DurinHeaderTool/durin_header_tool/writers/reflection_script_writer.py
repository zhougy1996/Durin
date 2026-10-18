"""Emit copied native exposure records from the parsed script metadata."""
def script_record(info, module=""):
    from durin_header_tool.writers.reflection_writer_common import _cpp_string_literal
    data = info.script
    names = {"ScriptType": "Type", "ScriptCallable": "Callable", "ScriptNoExport": "NoExport"}
    flags = " | ".join("Durin::EScriptExposure::" + flag for key, flag in names.items() if data.get(key))
    return "{ " + (flags or "Durin::EScriptExposure::None") + ", " + _cpp_string_literal(data.get("ScriptName", "")) + ", " + _cpp_string_literal(module) + " }"
