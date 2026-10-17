import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool.parser.clang_context import _validate_preprocessor_context


@pytest.mark.parametrize("source", [
    "/*\n#if EXAMPLE_FLAG\n#elif OTHER_FLAG\n#endif\n*/\n",
    "// #ifdef EXAMPLE_FLAG\n",
    'const char* Text = R"sample(\n#ifndef EXAMPLE_FLAG\n#endif\n)sample";\n',
    'const char* Text = "example\\\n#if EXAMPLE_FLAG\\\n";\n',
])
def test_non_code_directives_do_not_require_macro_dependencies(source):
    _validate_preprocessor_context(source)


@pytest.mark.parametrize("wrapper", [
    "/*\n{}\n*/\n",
    'const char* Text = R"sample(\n{}\n)sample";\n',
])
def test_non_code_define_cannot_whitelist_a_real_unknown_condition(wrapper):
    source = wrapper.format("#define INCLUDED_FEATURE 1") + "#if INCLUDED_FEATURE\n#endif\n"
    with pytest.raises(ValueError, match="unsupported non-hermetic macro dependency 'INCLUDED_FEATURE'"):
        _validate_preprocessor_context(source)


@pytest.mark.parametrize("wrapper", [
    "/*\n{}\n*/\n",
    'const char* Text = R"sample(\n{}\n)sample";\n',
])
def test_non_code_undef_does_not_remove_a_real_local_definition(wrapper):
    source = "#define LOCAL_FEATURE 1\n" + wrapper.format("#undef LOCAL_FEATURE")
    _validate_preprocessor_context(source + "#if LOCAL_FEATURE\n#endif\n")


@pytest.mark.parametrize("condition", [
    "#if NDEBUG /* explanatory words */",
    "#if NDEBUG // explanatory words",
    "#ifdef NDEBUG /* explanatory words */",
    "#ifndef NDEBUG // explanatory words",
    "/* leading comment */ #if NDEBUG",
    "#if 'a' == 97 // character constants are not macro names",
])
def test_real_conditions_ignore_comment_and_literal_contents(condition):
    _validate_preprocessor_context(condition + "\n#endif\n")


def test_real_unknown_condition_keeps_original_line_diagnostic():
    source = "/*\n#if DOCUMENTATION_ONLY\n*/\n/* leading comment */ #if INCLUDED_FEATURE\n#endif\n"
    with pytest.raises(ValueError, match="line 4: unsupported non-hermetic macro dependency 'INCLUDED_FEATURE'"):
        _validate_preprocessor_context(source)


def test_real_undef_still_removes_a_local_definition():
    with pytest.raises(ValueError, match="macro dependency 'LOCAL_FEATURE'"):
        _validate_preprocessor_context("#define LOCAL_FEATURE 1\n#undef LOCAL_FEATURE\n#if LOCAL_FEATURE\n#endif\n")
