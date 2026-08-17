from __future__ import annotations

import argparse
import os
import re
import shlex
import sys
from pathlib import Path
from typing import Callable, TextIO

from .context import CommandIO, RepositoryContext
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


def _matching_choices(argument: object, prefix: str) -> tuple[str, ...]:
    choices = getattr(argument, "kwargs", {}).get("choices")
    if choices is None or isinstance(choices, range) and len(choices) > 100:
        return ()
    values = tuple(str(choice) for choice in choices)
    if len(values) > 100:
        return ()
    return tuple(value for value in values if value.casefold().startswith(prefix.casefold()))


def shell_completion_candidates(
    registry: CommandRegistry,
    line: str,
    cursor_position: int | None = None,
) -> tuple[str, ...]:
    before_cursor = line[:cursor_position]
    match = re.search(r"(?:^|\s)(\S*)$", before_cursor)
    prefix = match.group(1) if match else ""
    completed_text = before_cursor[: -len(prefix)] if prefix else before_cursor
    try:
        completed = split_shell_command(completed_text)
    except ValueError:
        return ()

    if not completed:
        names = [spec.name for spec in registry.specifications]
        names.extend(("exit", "quit"))
        return tuple(
            name for name in names if name.casefold().startswith(prefix.casefold())
        )

    current = next(
        (
            spec
            for spec in registry.specifications
            if spec.name == completed[0].removeprefix("/").casefold()
        ),
        None,
    )
    if current is None:
        return ()

    argument_index = 1
    while current.subcommands and argument_index < len(completed):
        child_name = completed[argument_index].removeprefix("/").casefold()
        child = next(
            (candidate for candidate in current.subcommands if candidate.name == child_name),
            None,
        )
        if child is None:
            break
        current = child
        argument_index += 1

    if current.subcommands and argument_index == len(completed):
        return tuple(
            child.name
            for child in current.subcommands
            if child.name.casefold().startswith(prefix.casefold())
        )

    if completed:
        previous = completed[-1]
        previous_argument = next(
            (
                argument
                for argument in current.arguments
                if previous in argument.flags
            ),
            None,
        )
        if previous_argument is not None:
            if previous_argument.kwargs.get("choices") is not None:
                return _matching_choices(previous_argument, prefix)

    if "=" in prefix:
        option, value_prefix = prefix.split("=", 1)
        option_argument = next(
            (
                argument
                for argument in current.arguments
                if option in argument.flags
            ),
            None,
        )
        if (
            option_argument is not None
            and option_argument.kwargs.get("choices") is not None
        ):
            return tuple(
                f"{option}={choice}"
                for choice in _matching_choices(option_argument, value_prefix)
            )

    options = (
        flag
        for argument in current.arguments
        if argument.kwargs.get("help") is not argparse.SUPPRESS
        for flag in argument.flags
        if flag.startswith("-")
    )
    return tuple(
        option
        for option in options
        if option.casefold().startswith(prefix.casefold())
    )


def _configure_readline_completion(registry: CommandRegistry) -> None:
    try:
        import readline
    except ImportError:
        return

    matches: tuple[str, ...] = ()

    def complete(_text: str, state: int) -> str | None:
        nonlocal matches
        if state == 0:
            matches = shell_completion_candidates(
                registry,
                readline.get_line_buffer(),
                readline.get_endidx(),
            )
        return matches[state] if state < len(matches) else None

    readline.set_completer(complete)
    readline.set_completer_delims(" \t\n")
    if "libedit" in (readline.__doc__ or ""):
        readline.parse_and_bind("bind ^I rl_complete")
    else:
        readline.parse_and_bind("tab: complete")


def _interactive_input(registry: CommandRegistry, stdout: TextIO) -> Callable[[str], str]:
    if not sys.stdin.isatty() or not stdout.isatty():
        return input
    try:
        from prompt_toolkit import PromptSession
        from prompt_toolkit.completion import Completer, Completion
    except ImportError:
        _configure_readline_completion(registry)
        return input

    class RegistryCompleter(Completer):
        def get_completions(self, document: object, _complete_event: object):
            text = getattr(document, "text", "")
            cursor_position = getattr(document, "cursor_position", len(text))
            before_cursor = text[:cursor_position]
            match = re.search(r"(?:^|\s)(\S*)$", before_cursor)
            prefix = match.group(1) if match else ""
            for candidate in shell_completion_candidates(
                registry, text, cursor_position
            ):
                yield Completion(candidate, start_position=-len(prefix))

    session: PromptSession[str] = PromptSession(
        completer=RegistryCompleter(),
        complete_while_typing=False,
    )
    return session.prompt


def run_shell(
    *,
    registry: CommandRegistry,
    repository_root: Path,
    stdout: TextIO,
    stderr: TextIO,
    repository_context: RepositoryContext | None = None,
    input_func: Callable[[str], str] = input,
) -> int:
    print("Durin Developer Tool shell", file=stdout)
    print("Type help for available commands.", file=stdout)
    session_state: dict[str, object] = {}
    repository = repository_context or RepositoryContext.load(repository_root)
    read_input = (
        _interactive_input(registry, stdout) if input_func is input else input_func
    )
    while True:
        try:
            line = read_input("DurinDevTool> ").strip()
        except EOFError:
            print("", file=stdout)
            return 0
        except KeyboardInterrupt:
            print("\nUse exit to leave the shell.", file=stderr)
            continue
        if not line:
            continue
        try:
            parts = split_shell_command(line)
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
            command_io = CommandIO(
                stdout,
                stderr,
                plain=bool(getattr(namespace, "plain", False)),
            )
            result = registry.execute(
                spec,
                namespace,
                repository_context=repository,
                command_io=command_io,
                session_state=session_state,
            )
            if session_state.get("exit_requested"):
                return 0
        except DevToolError as exc:
            print(f"Error: {exc}", file=stderr)
        except SystemExit:
            continue
