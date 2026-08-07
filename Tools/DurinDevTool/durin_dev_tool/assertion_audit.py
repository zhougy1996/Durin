"""Deterministic audit of side effects hidden in Durin assertion conditions."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence, TextIO

import clang.cindex

from .errors import DevToolError

SCHEMA_VERSION = 1
DEFAULT_ALLOWLIST = Path("Tools/DurinDevTool/config/assertion-side-effect-allowlist.json")
MACROS = ("require", "requiref", "check", "checkf", "verify", "verifyf", "checkSlow", "checkfSlow")
CPP_EXTENSIONS = {".h", ".hpp", ".inl", ".cpp"}
SOURCE_ROOTS = (
    "Engine/Source",
    "Engine/Tests/Native",
    "Engine/Tests/NativeTestSupport",
    "Engine/CMake/SharedPCH",
    "Sandbox/Source",
)
TEMPLATE_ROOTS = ("Templates/Scaffolding",)
EXCLUDED_PARTS = {
    "external", "thirdparty", "third_party", "vendor", "vendored",
    "build", "binaries", "install", "intermediate", ".git", ".venv",
}
CALL_EXCLUSIONS = {
    "alignof", "catch", "decltype", "if", "noexcept", "requires",
    "sizeof", "static_assert", "switch", "typeid", "while",
}
ASSIGNMENTS = {
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
}
OVERLOADABLE = {
    "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "[", "->",
}
UNSAFE_CALL_WORDS = {
    "cancel", "commit", "create", "destroy", "dispatch", "enqueue", "execute",
    "flush", "initialize", "load", "open", "publish", "register", "release",
    "remove", "request", "save", "start", "stop", "submit", "unregister", "wait", "write",
}
TRAVERSAL_WORDS = {"all_of", "any_of", "for_each", "foreach", "iterate", "traverse", "visit", "walk"}


@dataclass(frozen=True, order=True)
class Invocation:
    path: str
    offset: int
    line: int
    column: int
    macro: str
    condition: str
    source_kind: str


@dataclass(frozen=True)
class Finding:
    id: str
    macro: str
    path: str
    line: int
    column: int
    constructKind: str
    classification: str
    allowlistDisposition: str
    sourceKind: str
    owner: str
    validationTarget: str
    expression: str


def _relative(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def _is_excluded(path: Path, root: Path) -> bool:
    parts = {part.casefold() for part in path.resolve().relative_to(root.resolve()).parts}
    return bool(parts & EXCLUDED_PARTS)


def discover_sources(root: Path, requested: Sequence[Path] = ()) -> list[Path]:
    if requested:
        candidates = [path if path.is_absolute() else root / path for path in requested]
    else:
        candidates = []
        for source_root in SOURCE_ROOTS:
            base = root / source_root
            if base.is_dir():
                candidates.extend(path for path in base.rglob("*") if path.suffix.casefold() in CPP_EXTENSIONS)
        for template_root in TEMPLATE_ROOTS:
            base = root / template_root
            if base.is_dir():
                candidates.extend(path for path in base.rglob("*") if path.is_file())
        programs = root / "Engine/Source/Programs"
        if programs.is_dir():
            candidates.extend(
                path for path in programs.rglob("*")
                if path.is_file() and path.suffix.casefold() in {".py", ".template", ".in"}
            )
    expanded: list[Path] = []
    for candidate in candidates:
        if candidate.is_dir():
            expanded.extend(path for path in candidate.rglob("*") if path.is_file())
        elif candidate.is_file():
            expanded.append(candidate)
        else:
            raise DevToolError(f"Assertion audit input does not exist: {candidate}")
    return sorted(
        {path.resolve() for path in expanded if not _is_excluded(path, root)},
        key=lambda path: _relative(path, root).casefold(),
    )


def _mask_comments_and_literals(source: str) -> str:
    result = list(source)
    index = 0
    length = len(source)
    while index < length:
        raw_match = re.match(r'R"([^ ()\\\t\r\n]{0,16})\(', source[index:])
        if raw_match:
            terminator = ")" + raw_match.group(1) + '"'
            end = source.find(terminator, index + raw_match.end())
            if end < 0:
                raise DevToolError("unterminated raw string literal")
            for position in range(index, end + len(terminator)):
                if result[position] != "\n":
                    result[position] = " "
            index = end + len(terminator)
        elif source.startswith("//", index):
            end = source.find("\n", index)
            end = length if end < 0 else end
            for position in range(index, end):
                result[position] = " "
            index = end
        elif source.startswith("/*", index):
            end = source.find("*/", index + 2)
            if end < 0:
                raise DevToolError("unterminated block comment")
            for position in range(index, end + 2):
                if result[position] != "\n":
                    result[position] = " "
            index = end + 2
        elif source[index] in {'"', "'"} and not (
            source[index] == "'"
            and index > 0
            and index + 1 < length
            and source[index - 1].isalnum()
            and source[index + 1].isalnum()
        ):
            quote = source[index]
            index += 1
            while index < length:
                if source[index] == "\\":
                    result[index] = " "
                    if index + 1 < length:
                        result[index + 1] = " "
                    index += 2
                elif source[index] == quote:
                    index += 1
                    break
                else:
                    if result[index] != "\n":
                        result[index] = " "
                    index += 1
            else:
                raise DevToolError("unterminated string or character literal")
        else:
            index += 1
    return "".join(result)


def _line_column(source: str, offset: int) -> tuple[int, int]:
    line = source.count("\n", 0, offset) + 1
    previous = source.rfind("\n", 0, offset)
    return line, offset - previous


def _definition_ranges(masked: str) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    offset = 0
    lines = masked.splitlines(keepends=True)
    index = 0
    while index < len(lines):
        line = lines[index]
        if re.match(r"^\s*#\s*define\b", line):
            start = offset
            while line.rstrip("\r\n").endswith("\\") and index + 1 < len(lines):
                offset += len(line)
                index += 1
                line = lines[index]
            ranges.append((start, offset + len(line)))
        offset += len(line)
        index += 1
    return ranges


def _inside(offset: int, ranges: Sequence[tuple[int, int]]) -> bool:
    return any(start <= offset < end for start, end in ranges)


def _first_argument(source: str, masked: str, opening: int, path: str) -> tuple[str, int]:
    pairs = {"(": ")", "[": "]", "{": "}"}
    stack = [")"]
    index = opening + 1
    argument_end = -1
    while index < len(masked):
        token = masked[index]
        if token in pairs:
            stack.append(pairs[token])
        elif token in ")]}":
            if not stack or token != stack.pop():
                line, column = _line_column(source, index)
                raise DevToolError(f"{path}:{line}:{column}: mismatched delimiter in assertion")
            if not stack:
                argument_end = index
                break
        elif token == "," and len(stack) == 1:
            argument_end = index
            break
        index += 1
    if argument_end < 0:
        line, column = _line_column(source, opening)
        raise DevToolError(f"{path}:{line}:{column}: unterminated assertion invocation")
    condition = source[opening + 1:argument_end].strip()
    if not condition:
        line, column = _line_column(source, opening)
        raise DevToolError(f"{path}:{line}:{column}: assertion condition is empty")
    return condition, index


def extract_invocations(path: Path, root: Path) -> list[Invocation]:
    relative = _relative(path, root)
    try:
        source = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as exc:
        raise DevToolError(f"{relative}: source is not valid UTF-8") from exc
    pattern = re.compile(r"\b(" + "|".join(MACROS) + r")\s*\(")
    if pattern.search(source) is None:
        return []
    try:
        masked = _mask_comments_and_literals(source)
    except DevToolError as exc:
        raise DevToolError(f"{relative}: {exc}") from exc
    definitions = _definition_ranges(masked)
    source_kind = (
        "scaffolding-template"
        if relative.startswith("Templates/Scaffolding/") or path.suffix.casefold() in {".template", ".in"}
        else "invocation"
    )
    result: list[Invocation] = []
    for match in pattern.finditer(masked):
        line, column = _line_column(source, match.start())
        if _inside(match.start(), definitions):
            result.append(Invocation(relative, match.start(), line, column, match.group(1), "", "macro-definition"))
            continue
        opening = masked.find("(", match.start(), match.end())
        condition, _ = _first_argument(source, masked, opening, relative)
        result.append(Invocation(relative, match.start(), line, column, match.group(1), condition, source_kind))
    return result


def _clang_tokens(invocation: Invocation) -> list[str]:
    source = f"void __durin_assertion_scan() {{ (void)({invocation.condition}); }}"
    index = clang.cindex.Index.create()
    unit = index.parse(
        "durin_assertion_scan.cpp",
        args=["-std=c++20", "-fsyntax-only"],
        unsaved_files=[("durin_assertion_scan.cpp", source)],
    )
    syntax_diagnostics = [
        diagnostic.spelling
        for diagnostic in unit.diagnostics
        if diagnostic.severity >= clang.cindex.Diagnostic.Error
        and any(marker in diagnostic.spelling.casefold() for marker in (
            "expected expression", "expected ')'", "expected ']'", "expected '}'",
            "extraneous", "unterminated", "invalid token",
        ))
    ]
    if syntax_diagnostics:
        raise DevToolError(
            f"{invocation.path}:{invocation.line}:{invocation.column}: "
            f"libclang could not parse assertion condition: {syntax_diagnostics[0]}"
        )
    prefix = source.index(invocation.condition)
    suffix = prefix + len(invocation.condition)
    return [
        token.spelling
        for token in unit.get_tokens(extent=unit.cursor.extent)
        if token.extent.start.offset >= prefix and token.extent.end.offset <= suffix
    ]


def _constructs(invocation: Invocation) -> list[str]:
    if invocation.source_kind == "macro-definition":
        return ["macro-definition"]
    tokens = _clang_tokens(invocation)
    if not tokens:
        raise DevToolError(
            f"{invocation.path}:{invocation.line}:{invocation.column}: libclang produced no condition tokens"
        )
    kinds: set[str] = set()
    parentheses: list[bool] = []
    for index, token in enumerate(tokens):
        if token == "(":
            previous = tokens[index - 1] if index else ""
            parentheses.append(bool(re.match(r"^[A-Za-z_]\w*$", previous)) or previous in {")", "]", ">"})
        elif token == ")" and parentheses:
            parentheses.pop()
        if token in ASSIGNMENTS:
            kinds.add("assignment")
        if token in {"++", "--"}:
            kinds.add("increment-decrement")
        if token == "new":
            kinds.add("allocation")
        if token == "delete":
            kinds.add("deallocation")
        if token in {"co_await", "co_yield", "co_return"}:
            kinds.add("coroutine-transition")
        if token == "," and (not parentheses or not parentheses[-1]):
            kinds.add("comma-expression")
        if token in OVERLOADABLE:
            kinds.add("potentially-overloaded-operation")
        if index + 1 < len(tokens) and tokens[index + 1] == "(" and re.match(r"^[A-Za-z_]\w*$", token):
            if token not in CALL_EXCLUSIONS:
                kinds.add("direct-call")
                word = token.casefold()
                if any(word.startswith(fragment) for fragment in TRAVERSAL_WORDS):
                    kinds.add("traversal")
                if any(token.startswith(fragment.capitalize()) for fragment in UNSAFE_CALL_WORDS):
                    kinds.add("unsafe-operation")
    if "[" in tokens and "{" in tokens:
        kinds.add("callback")
    return sorted(kinds)


def _owner(path: str) -> tuple[str, str]:
    parts = path.split("/")
    if len(parts) >= 4 and parts[:2] == ["Engine", "Source"]:
        module = parts[3] if parts[2] in {"Runtime", "Editor", "Programs"} else parts[2]
        return module, "DurinNativeTests"
    if len(parts) >= 4 and parts[:3] == ["Engine", "Tests", "Native"]:
        return parts[3], "DurinNativeTests"
    if path.startswith("Sandbox/Source/"):
        return "Sandbox", "DurinGame"
    if path.startswith("Templates/Scaffolding/"):
        return "DurinDevTool", "DurinDevTool pytest"
    return "BuildInfrastructure", "DurinDevTool pytest"


def _classification(invocation: Invocation, kind: str) -> str:
    if kind == "macro-definition":
        return "diagnostic-only-work"
    if invocation.macro in {"require", "requiref"}:
        return "enforced-runtime-contract"
    if invocation.macro in {"verify", "verifyf"}:
        return "intentional-verify-operation"
    if kind == "unsafe-operation":
        return "unsafe-ignored-failure"
    if kind in {
        "assignment", "increment-decrement", "allocation", "deallocation",
        "coroutine-transition", "callback", "traversal", "comma-expression",
    }:
        return "required-behavior"
    if kind in {"potentially-overloaded-operation", "direct-call"}:
        return "scanner-limitation"
    return "observational-query"


def _load_allowlist(path: Path | None, root: Path) -> Mapping[str, str]:
    if path is None:
        return {}
    resolved = path if path.is_absolute() else root / path
    try:
        value = json.loads(resolved.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise DevToolError(f"Could not read assertion allowlist {resolved}: {exc}") from exc
    if not isinstance(value, dict) or value.get("schemaVersion") != 1 or not isinstance(value.get("entries"), list):
        raise DevToolError("Assertion allowlist must use schema version 1 and contain entries.")
    entries: dict[str, str] = {}
    for entry in value["entries"]:
        if not isinstance(entry, dict) or set(entry) != {"id", "rationale"}:
            raise DevToolError("Assertion allowlist entries require only id and rationale.")
        identifier, rationale = entry["id"], entry["rationale"]
        if not isinstance(identifier, str) or not identifier or not isinstance(rationale, str) or not rationale.strip():
            raise DevToolError("Assertion allowlist entries require a non-empty id and rationale.")
        if identifier in entries:
            raise DevToolError(f"Duplicate assertion allowlist id: {identifier}")
        entries[identifier] = rationale
    return entries


def scan(root: Path, sources: Sequence[Path] = (), allowlist_path: Path | None = None) -> dict[str, object]:
    allowlist = _load_allowlist(allowlist_path, root)
    findings: list[Finding] = []
    seen_allowlist: set[str] = set()
    scanned_files = 0
    invocations = 0
    for path in discover_sources(root, sources):
        extracted = extract_invocations(path, root)
        if not extracted:
            continue
        scanned_files += 1
        invocations += len(extracted)
        for invocation in extracted:
            expression = " ".join(invocation.condition.split())
            owner, target = _owner(invocation.path)
            for kind in _constructs(invocation):
                identifier = f"{invocation.path}:{invocation.line}:{invocation.column}:{invocation.macro}:{kind}"
                classification = _classification(invocation, kind)
                if identifier in allowlist:
                    disposition = "allowed"
                elif classification in {"intentional-verify-operation", "enforced-runtime-contract"}:
                    disposition = "classified"
                else:
                    disposition = "unreviewed"
                if disposition == "allowed":
                    seen_allowlist.add(identifier)
                findings.append(Finding(
                    identifier, invocation.macro, invocation.path, invocation.line,
                    invocation.column, kind, classification,
                    disposition, invocation.source_kind, owner, target, expression,
                ))
    stale = sorted(set(allowlist) - seen_allowlist)
    if stale:
        raise DevToolError("Stale assertion allowlist entries: " + ", ".join(stale))
    findings.sort(key=lambda item: (item.path.casefold(), item.line, item.column, item.macro, item.constructKind))
    return {
        "schemaVersion": SCHEMA_VERSION,
        "frontend": "libclang-token-stream",
        "summary": {
            "filesWithAssertions": scanned_files,
            "assertionInvocations": invocations,
            "findings": len(findings),
            "allowed": sum(item.allowlistDisposition == "allowed" for item in findings),
            "unreviewed": sum(item.allowlistDisposition == "unreviewed" for item in findings),
        },
        "findings": [asdict(item) for item in findings],
    }


def _render_human(report: Mapping[str, object], stdout: TextIO) -> None:
    summary = report["summary"]
    assert isinstance(summary, Mapping)
    print(
        "Assertion side-effect audit: "
        f"{summary['assertionInvocations']} invocation(s), {summary['findings']} finding(s), "
        f"{summary['unreviewed']} unreviewed.",
        file=stdout,
    )
    for value in report["findings"]:
        assert isinstance(value, Mapping)
        print(
            f"{value['path']}:{value['line']}:{value['column']} "
            f"[{value['macro']}/{value['constructKind']}] {value['classification']} "
            f"({value['allowlistDisposition']}) owner={value['owner']} "
            f"validate={value['validationTarget']}",
            file=stdout,
        )


def run(
    namespace: argparse.Namespace,
    *,
    registry: object,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    session_state: dict[str, object] | None = None,
) -> int:
    del registry, stderr, session_state
    paths = tuple(getattr(namespace, "paths", ()) or ())
    enforce = bool(getattr(namespace, "enforce", False)) or not paths
    allowlist_path = getattr(namespace, "allowlist_path", None)
    if allowlist_path is None and not paths:
        allowlist_path = DEFAULT_ALLOWLIST
    report = scan(
        repository_root,
        paths,
        allowlist_path,
    )
    serialized = json.dumps(report, indent=2, ensure_ascii=False) + "\n"
    output_path = getattr(namespace, "output_path", None)
    if output_path is not None:
        resolved = output_path if output_path.is_absolute() else repository_root / output_path
        resolved.parent.mkdir(parents=True, exist_ok=True)
        resolved.write_text(serialized, encoding="utf-8", newline="\n")
    if getattr(namespace, "format_name", "human") == "json":
        stdout.write(serialized)
    else:
        _render_human(report, stdout)
    summary = report["summary"]
    assert isinstance(summary, Mapping)
    return 1 if enforce and summary["unreviewed"] else 0
