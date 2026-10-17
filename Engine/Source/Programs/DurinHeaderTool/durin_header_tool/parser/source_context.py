"""Lexical state owned by one immutable header source snapshot."""
from bisect import bisect_left
from dataclasses import dataclass, field
from functools import cached_property
import re

from durin_header_tool.parser.cpp_source_scanner import CppSourceScanner

_GENERATED_BODY_PATTERN = re.compile(r"\bGENERATED_BODY\s*\(")


@dataclass(frozen=True)
class HeaderSourceContext:
    source: str
    scanner: CppSourceScanner = field(init=False, repr=False, compare=False)
    _scope_bounds: dict[tuple[int, int], tuple[int, int] | None] = field(
        default_factory=dict, init=False, repr=False, compare=False)

    def __post_init__(self):
        object.__setattr__(self, "scanner", CppSourceScanner(self.source))

    @cached_property
    def lines(self) -> tuple[str, ...]:
        return tuple(self.source.splitlines())

    def scope_bounds(self, start_line: int, start_column: int) -> tuple[int, int] | None:
        key = (start_line, start_column)
        if key not in self._scope_bounds:
            bounds = None
            if 0 < start_line <= self.scanner.line_count:
                position = self.scanner.position_from_line_column(start_line, start_column)
                opening = self.scanner.find_next_code_position("{", position)
                closing = self.scanner.find_matching_brace(opening) if opening is not None else None
                if closing is not None:
                    bounds = (opening, closing)
            self._scope_bounds[key] = bounds
        return self._scope_bounds[key]

    def scope_line_range(self, start_line: int, start_column: int) -> tuple[int, int]:
        bounds = self.scope_bounds(start_line, start_column)
        return (start_line, self.scanner.line_number(bounds[1])) if bounds else (0, 0)

    @cached_property
    def _generated_body_lines(self) -> tuple[int, ...]:
        return tuple(self.scanner.line_number(match.start())
                     for match in _GENERATED_BODY_PATTERN.finditer(self.source)
                     if self.scanner.is_code_position(match.start()))

    def generated_body_line(self, start_line: int, end_line: int) -> int:
        index = bisect_left(self._generated_body_lines, start_line)
        if index < len(self._generated_body_lines) and self._generated_body_lines[index] <= end_line:
            return self._generated_body_lines[index]
        return 0


def source_context_for(source: str, context: HeaderSourceContext | None = None) -> HeaderSourceContext:
    if context is None:
        return HeaderSourceContext(source)
    if context.source is not source and context.source != source:
        raise ValueError("header source context belongs to a different source snapshot")
    return context
