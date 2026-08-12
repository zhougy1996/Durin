from __future__ import annotations

import argparse
import os
import shlex
from pathlib import Path
from typing import Callable, TextIO

from .errors import DevToolError
from .registry import CommandRegistry


def split_windows_command_line(line: str) -> list[str]:
    arguments: list[str] = []
    current: list[str] = []
    in_quotes = False
    token_started = False
    index = 0
    while index < len(line):
        character = line[index]
        if character in " \t" and not in_quotes:
            if token_started:
                arguments.append("".join(current))
                current.clear()
                token_started = False
            index += 1
            continue
        if character == "\\":
            slash_start = index
            while index < len(line) and line[index] == "\\":
                index += 1
            slash_count = index - slash_start
            if index < len(line) and line[index] == '"':
                current.extend("\\" * (slash_count // 2))
                token_started = True
                if slash_count % 2:
                    current.append('"')
                else:
                    in_quotes = not in_quotes
                index += 1
            else:
                current.extend("\\" * slash_count)
                token_started = True
            continue
        if character == '"':
            token_started = True
            in_quotes = not in_quotes
            index += 1
            continue
        current.append(character)
        token_started = True
        index += 1
    if in_quotes:
        raise ValueError("unmatched double quote")
    if token_started:
        arguments.append("".join(current))
    return arguments


def split_shell_command(line: str) -> list[str]:
    return (
        split_windows_command_line(line)
        if os.name == "nt"
        else shlex.split(line, posix=True)
    )


def normalize_compact_build_command(parts: list[str]) -> list[str]:
    if not parts:
        return parts
    command = parts[0].removeprefix("/").lower()
    values = parts[1:]
    if command in {"build", "rebuild"} and values and not values[0].startswith("-"):
        return [parts[0], "--target", values[0], *values[1:]]
    run_options = {
        "--profile",
        "--preset",
        "--plain",
        "--output",
        "--project",
        "--args",
    }
    if (
        command == "run"
        and values
        and values[0].partition("=")[0] not in run_options
    ):
        return [parts[0], "--args", *values]
    return parts


def run_shell(
    *,
    registry: CommandRegistry,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    input_func: Callable[[str], str] = input,
) -> int:
    print("Durin Developer Tool shell", file=stdout)
    print("Type help for available commands.", file=stdout)
    session_state: dict[str, object] = {}
    while True:
        try:
            line = input_func("DurinDevTool> ").strip()
        except EOFError:
            print("", file=stdout)
            return 0
        except KeyboardInterrupt:
            print("\nUse exit to leave the shell.", file=stderr)
            continue
        if not line:
            continue
        try:
            parts = normalize_compact_build_command(split_shell_command(line))
        except ValueError as exc:
            print(f"Error: invalid command: {exc}", file=stderr)
            continue
        command = parts[0].lower()
        if command in {"exit", "quit"}:
            return 0
        if command in {"?", "/?", "/help"}:
            parts = ["help"]
        if len(parts) == 1 and registry.group_without_default(parts[0]):
            print(registry.format_command_help(parts), file=stdout)
            continue
        try:
            spec, namespace = registry.parse(parts)
            result = registry.execute(
                spec,
                namespace,
                repository_root=repository_root,
                stdout=stdout,
                stderr=stderr,
                session_state=session_state,
            )
            if session_state.get("exit_requested"):
                return 0
        except DevToolError as exc:
            print(f"Error: {exc}", file=stderr)
        except SystemExit:
            continue
