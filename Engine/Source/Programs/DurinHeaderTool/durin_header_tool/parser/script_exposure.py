"""Language-neutral positive script exposure; no Python runtime dependency."""
import re
from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner

def script_exposure(entries, allowed, location):
    result = {}
    for entry in entries:
        key, sep, value = entry.strip().partition("=")
        key = key.strip()
        if not key.startswith("Script"):
            continue
        if key not in allowed or key in result:
            raise ValueError(f"{location}: unsupported or duplicate script specifier {key}")
        if key == "ScriptName":
            match = re.fullmatch(r'"([A-Za-z_][A-Za-z_0-9]*)"', value.strip())
            if not sep or not match:
                raise ValueError(f"{location}: ScriptName requires a quoted identifier")
            result[key] = match.group(1)
        else:
            if sep:
                raise ValueError(f"{location}: {key} is a flag")
            result[key] = True
    if result.get("ScriptReadOnly") and result.get("ScriptReadWrite"):
        raise ValueError(f"{location}: ScriptReadOnly and ScriptReadWrite conflict")
    return result
