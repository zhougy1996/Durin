import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from durin_header_tool.parser import CppSourceScanner


def test_split_macro_arguments_ignores_quoted_commas_and_nested_delimiters():
    scanner = CppSourceScanner(
        'DisplayName = "A, B", Factory(make_value(1, 2), { 3, 4 }), ReadOnly'
    )

    assert scanner.split_macro_arguments() == [
        'DisplayName = "A, B"',
        " Factory(make_value(1, 2), { 3, 4 })",
        " ReadOnly",
    ]


def test_scanner_unescapes_supported_cpp_string_escapes():
    assert CppSourceScanner.unescape_string_literal(r'Editor \"Mode\" and \\ path') == (
        'Editor "Mode" and \\ path'
    )
    assert CppSourceScanner.unescape_string_literal(r'"Editor"') == "Editor"


def test_scanner_identifies_code_comments_strings_and_raw_strings():
    source = (
        '// DCLASS(Comment)\n'
        'const char* text = "DCLASS(String)";\n'
        'const char* raw = R"tag(DCLASS(Raw))tag";\n'
        'DCLASS(Code)'
    )
    scanner = CppSourceScanner(source)

    assert not scanner.is_code_position(source.index("DCLASS(Comment)"))
    assert not scanner.is_code_position(source.index("DCLASS(String)"))
    assert not scanner.is_code_position(source.index("DCLASS(Raw)"))
    assert scanner.is_code_position(source.rindex("DCLASS(Code)"))


def test_scanner_matches_parentheses_and_braces_across_comments_and_strings():
    source = 'DCLASS({ "}" /* ) */ }) trailing'
    scanner = CppSourceScanner(source)
    parenthesis = source.index("(")
    brace = source.index("{")

    assert scanner.find_matching_parenthesis(parenthesis) == source.rindex(")")
    assert scanner.find_matching_brace(brace) == source.rindex("}")


def test_scanner_maps_source_positions():
    scanner = CppSourceScanner("one\ntwo\nthree")

    position = scanner.position_from_line_column(2, 2)
    assert position == 5
    assert scanner.line_column(position) == (2, 2)
    assert scanner.line_number(len(scanner.source)) == 3


def test_scanner_reports_unterminated_quoted_arguments():
    with pytest.raises(ValueError, match="unterminated quoted string"):
        CppSourceScanner('DisplayName = "broken').split_macro_arguments()


@pytest.mark.parametrize("literal", [
    "1'000", "1'000'000ULL", "0xCA'FE", "0b1010'0011", "07'77",
    "1.2'5e+1'0", ".1'25", "0x1.a'fp+1'0", "1'000.0f",
])
def test_numeric_separators_leave_following_markers_and_delimiters_in_code(literal):
    source = f"Factory({literal}, {{ 2, 3 }}) DFUNCTION()"
    scanner = CppSourceScanner(source)
    assert scanner.is_code_position(source.index("DFUNCTION"))
    assert all(scanner.is_code_position(index) for index, character in enumerate(source) if character == "'")
    assert scanner.find_matching_parenthesis(source.index("(")) == source.index(")")
    assert CppSourceScanner(f"{literal}, Next").split_macro_arguments() == [literal, " Next"]


@pytest.mark.parametrize("literal", ["'1'", "u8'1'", "u'1'", "U'1'", "L'1'", r"'\''"])
def test_numeric_token_detection_preserves_character_literals(literal):
    source = f"{literal}; DFUNCTION()"
    scanner = CppSourceScanner(source)
    assert not scanner.is_code_position(source.index("'"))
    assert scanner.is_code_position(source.index("DFUNCTION"))
