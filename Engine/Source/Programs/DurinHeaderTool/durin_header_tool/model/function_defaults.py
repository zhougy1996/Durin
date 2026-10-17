"""Restricted literal defaults. Never interpret arbitrary native expressions."""
import math
import re
import struct


def reflected_default(parameter, symbols=None):
    expression = parameter.default_expression
    if expression is None or parameter.passing == "Reference":
        return None
    compact = re.sub(r"\s+", "", expression)
    kind = parameter.kind
    if kind == "Bool":
        return compact if compact in ("true", "false") else None
    if isinstance(kind, str) and re.fullmatch(r"U?Int(8|16|32|64)", kind):
        match = re.fullmatch(r"([+-]?)(0[xX][0-9a-fA-F']+|0[bB][01']+|[0-9][0-9']*)([uUlL]*)", compact)
        if not match:
            return None
        sign, digits, suffix = match.groups()
        if suffix.lower() not in ("", "u", "l", "ll", "ul", "ull", "lu", "llu"):
            return None
        if sign == "-" and "u" in suffix.lower():
            return None
        digits = digits.replace("'", "")
        try:
            value = int(digits, 16 if digits.lower().startswith("0x") else
                        2 if digits.lower().startswith("0b") else 8 if len(digits) > 1 and digits[0] == "0" else 10)
        except ValueError:
            return None
        # A negative nondecimal literal may first select unsigned int/long and
        # wrap before conversion to the parameter type (-0xffffffff is 1 on
        # 32-bit int targets). Do not normalize that target-dependent operation
        # into signed arithmetic. An explicit LL keeps magnitudes below 2^63 signed.
        nondecimal = digits.lower().startswith(("0x", "0b")) or (len(digits) > 1 and digits[0] == "0")
        if sign == "-" and nondecimal and value > 2147483647 and suffix.lower() != "ll":
            return None
        value *= -1 if sign == "-" else 1
        bits = int(re.search(r"\d+", kind).group())
        unsigned = kind.startswith("U")
        low, high = (0, (1 << bits) - 1) if unsigned else (-(1 << (bits - 1)), (1 << (bits - 1)) - 1)
        if not low <= value <= high:
            return None
        if value == -(1 << 63):
            return "(-9223372036854775807LL - 1)"
        return str(value) + ("ULL" if unsigned else "LL")
    if kind in ("Float", "Double"):
        if not re.fullmatch(r"[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?[fF]?", compact):
            return None
        try:
            value = float(compact.rstrip("fF"))
            if not math.isfinite(value):
                return None
            if kind == "Float" or compact.endswith(("f", "F")):
                if not math.isfinite(struct.unpack("f", struct.pack("f", value))[0]):
                    return None
        except (ValueError, OverflowError):
            return None
        # Preserve the native float-literal promotion when the target is double.
        return compact
    if kind == "String":
        # Keep C++ literal decoding in the target compiler, including embedded NUL
        # and adjacent literals. No calls, prefixes, macros or raw strings here.
        literal = r'"(?:[^"\\\r\n]|\\(?:[abfnrtv\\\'"?]|[0-7]{1,3}|x[0-9a-fA-F]+|u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}))*"'
        return expression if re.fullmatch(r"(?:" + literal + r"\s*)+", expression) else None
    if isinstance(kind, dict):
        if kind["Kind"] == "Object" and compact == "nullptr":
            return "nullptr"
        if kind["Kind"] == "Enum":
            name = kind["Name"]
            prefix, separator, entry = compact.rpartition("::")
            symbol = (symbols or {}).get(name)
            if re.fullmatch(r"[A-Za-z_]\w*", entry) and (
                    prefix.lstrip(":") in (name, name.rsplit("::", 1)[-1])
                    or (not separator and symbol and not symbol.IsScoped)):
                return "::" + name + "::" + entry
    return None
