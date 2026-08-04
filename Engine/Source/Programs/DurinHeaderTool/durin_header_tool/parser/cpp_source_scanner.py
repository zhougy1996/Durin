from __future__ import annotations

from bisect import bisect_right


class CppSourceScanner:
    """Small deterministic lexical scanner for the C++ source used by DHT."""

    _CODE = "code"
    _LINE_COMMENT = "line_comment"
    _BLOCK_COMMENT = "block_comment"
    _STRING = "string"
    _CHARACTER = "character"
    _RAW_STRING = "raw_string"

    _OPEN_TO_CLOSE = {
        "(": ")",
        "{": "}",
        "[": "]",
    }
    _CLOSE_TO_OPEN = {closing: opening for opening, closing in _OPEN_TO_CLOSE.items()}

    def __init__(self, source: str):
        self.source = source
        self._line_starts = [0]
        self._line_starts.extend(index + 1 for index, char in enumerate(source) if char == "\n")
        self._states = self._scan_states()

    def _scan_states(self) -> list[str]:
        states = [self._CODE] * (len(self.source) + 1)
        self._has_unterminated_string = False
        position = 0
        while position < len(self.source):
            if self.source.startswith("//", position):
                end = self.source.find("\n", position)
                end = len(self.source) if end < 0 else end
                states[position:end] = [self._LINE_COMMENT] * (end - position)
                position = end
                continue

            if self.source.startswith("/*", position):
                end_marker = self.source.find("*/", position + 2)
                end = len(self.source) if end_marker < 0 else end_marker + 2
                states[position:end] = [self._BLOCK_COMMENT] * (end - position)
                position = end
                continue

            raw_end = self._raw_string_end(position)
            if raw_end is not None:
                states[position:raw_end] = [self._RAW_STRING] * (raw_end - position)
                position = raw_end
                continue

            if self.source[position] in ('"', "'"):
                quote = self.source[position]
                state = self._STRING if quote == '"' else self._CHARACTER
                end = position + 1
                closed = False
                while end < len(self.source):
                    if self.source[end] == "\\":
                        end += 2
                        continue
                    end += 1
                    if self.source[end - 1] == quote:
                        closed = True
                        break
                end = min(end, len(self.source))
                states[position:end] = [state] * (end - position)
                if quote == '"' and not closed:
                    self._has_unterminated_string = True
                position = end
                continue

            position += 1
        return states

    def _raw_string_end(self, position: int) -> int | None:
        if self.source[position:position + 2] != 'R"':
            return None

        delimiter_start = position + 2
        opening_parenthesis = self.source.find("(", delimiter_start)
        if opening_parenthesis < 0 or opening_parenthesis - delimiter_start > 16:
            return None

        delimiter = self.source[delimiter_start:opening_parenthesis]
        if any(char in "\\ \t\r\n()" or ord(char) < 32 for char in delimiter):
            return None

        closing_marker = ")" + delimiter + '"'
        closing_marker_start = self.source.find(closing_marker, opening_parenthesis + 1)
        return (
            len(self.source)
            if closing_marker_start < 0
            else closing_marker_start + len(closing_marker)
        )

    def lexical_state(self, position: int) -> str:
        self._validate_position(position)
        return self._states[position]

    def is_code_position(self, position: int) -> bool:
        return self.lexical_state(position) == self._CODE

    def is_comment_position(self, position: int) -> bool:
        return self.lexical_state(position) in (self._LINE_COMMENT, self._BLOCK_COMMENT)

    def is_string_position(self, position: int) -> bool:
        return self.lexical_state(position) in (
            self._STRING,
            self._CHARACTER,
            self._RAW_STRING,
        )

    def find_next_code_position(
        self,
        character: str,
        start: int = 0,
        end: int | None = None,
    ) -> int | None:
        if len(character) != 1:
            raise ValueError("character must contain exactly one character")
        self._validate_position(start)
        if end is None:
            end = len(self.source)
        self._validate_position(end)
        if end < start:
            raise ValueError("end must not precede start")
        for position in range(start, end):
            if self._states[position] == self._CODE and self.source[position] == character:
                return position
        return None

    def find_matching_delimiter(self, opening_position: int) -> int | None:
        self._validate_position(opening_position)
        if not self.is_code_position(opening_position):
            return None
        opening = self.source[opening_position]
        if opening not in self._OPEN_TO_CLOSE:
            return None

        stack = [self._OPEN_TO_CLOSE[opening]]
        for position in range(opening_position + 1, len(self.source)):
            if self._states[position] != self._CODE:
                continue
            character = self.source[position]
            if character in self._OPEN_TO_CLOSE:
                stack.append(self._OPEN_TO_CLOSE[character])
                continue
            if character not in self._CLOSE_TO_OPEN:
                continue
            if not stack or character != stack[-1]:
                return None
            stack.pop()
            if not stack:
                return position
        return None

    def find_matching_parenthesis(self, opening_position: int) -> int | None:
        self._validate_position(opening_position)
        if self.source[opening_position:opening_position + 1] != "(":
            return None
        return self.find_matching_delimiter(opening_position)

    def find_matching_brace(self, opening_position: int) -> int | None:
        self._validate_position(opening_position)
        if self.source[opening_position:opening_position + 1] != "{":
            return None
        return self.find_matching_delimiter(opening_position)

    def split_macro_arguments(self) -> list[str]:
        if not self.source.strip():
            return []
        if self._has_unterminated_string:
            raise ValueError("unterminated quoted string")

        entries: list[str] = []
        entry_start = 0
        stack: list[str] = []
        for position, character in enumerate(self.source):
            if self._states[position] != self._CODE:
                continue
            if character in self._OPEN_TO_CLOSE:
                stack.append(self._OPEN_TO_CLOSE[character])
            elif character in self._CLOSE_TO_OPEN:
                if not stack or character != stack[-1]:
                    raise ValueError("unbalanced macro argument delimiters")
                stack.pop()
            elif character == "," and not stack:
                entries.append(self.source[entry_start:position])
                entry_start = position + 1

        if stack:
            raise ValueError("unbalanced macro argument delimiters")
        entries.append(self.source[entry_start:])
        return entries

    @staticmethod
    def unescape_string_literal(value: str) -> str:
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]

        result: list[str] = []
        position = 0
        while position < len(value):
            character = value[position]
            if character == "\\" and position + 1 < len(value) and value[position + 1] in ('"', "\\"):
                result.append(value[position + 1])
                position += 2
                continue
            result.append(character)
            position += 1
        return "".join(result)

    def position_from_line_column(self, line: int, column: int) -> int:
        if line < 1 or line > len(self._line_starts):
            raise ValueError("line must be a one-based line within the source")
        if column < 1:
            raise ValueError("column must be one-based")
        return min(self._line_starts[line - 1] + column - 1, len(self.source))

    def line_column(self, position: int) -> tuple[int, int]:
        self._validate_position(position)
        line_index = bisect_right(self._line_starts, position) - 1
        return line_index + 1, position - self._line_starts[line_index] + 1

    def line_number(self, position: int) -> int:
        return self.line_column(position)[0]

    @property
    def line_count(self) -> int:
        return len(self._line_starts)

    def _validate_position(self, position: int) -> None:
        if position < 0 or position > len(self.source):
            raise ValueError("position is outside the source")
