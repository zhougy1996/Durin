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
from .commands.specification import ArgumentSpec


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


def _pending_argument(arguments: tuple[ArgumentSpec, ...], tokens: list[str]) -> ArgumentSpec | None:
    """Track value consumption, including lists and options using =value."""
    options = {flag: argument for argument in arguments for flag in argument.flags if flag.startswith('-')}
    positionals = iter(argument for argument in arguments if not argument.flags[0].startswith('-'))
    positional = next(positionals, None)
    pending = None
    consumed = 0
    for token in tokens:
        if token == '--':
            return None
        option, separator, _value = token.partition('=')
        if token.startswith('-'):
            pending = options.get(option)
            consumed = int(bool(separator))
            if pending is not None:
                nargs = pending.kwargs.get('nargs', 1)
                if pending.kwargs.get('action') in {'store_true', 'store_false', 'count'} or nargs == 0:
                    pending = None
                elif separator:
                    pending = None
            continue
        if pending is not None:
            consumed += 1
            nargs = pending.kwargs.get('nargs', 1)
            if nargs not in ('+', '*') and consumed >= (nargs if isinstance(nargs, int) else 1):
                pending = None
        elif positional is not None and positional.kwargs.get('nargs') not in ('+', '*'):
            positional = next(positionals, None)
    return pending or positional


def _repository_choices(
    argument: ArgumentSpec, tokens: list[str], repository: RepositoryContext | None,
    session_state: dict[str, object] | None,
) -> tuple[str, ...]:
    destination = argument.kwargs.get('dest', argument.flags[0].lstrip('-').replace('-', '_'))
    dependencies = {'private_dependencies', 'public_dependencies', 'optional_private_dependencies', 'optional_public_dependencies'}
    if destination not in {'preset', 'selected_preset', 'profile', *dependencies}:
        return ()
    try:
        repository = repository or RepositoryContext.load()
        if destination in dependencies:
            from .build.descriptors import descriptor_schema_directory, load_project_descriptor
            from .build.workspace_manifest import load_workspace_manifest
            names = {
                name for entry in load_workspace_manifest(repository.root)
                for name in load_project_descriptor(entry.descriptor, schema_directory=descriptor_schema_directory(repository)).module_dirs
            }
            return tuple(sorted(names, key=str.casefold))
        from .build.config_io import load_local_config, load_profiles
        from .build.selection import select_profile
        from .build.settings import BuildPaths
        paths = BuildPaths.from_repository(repository)
        profiles = load_profiles(paths.profile_file)
        if destination == 'profile':
            from .build.selection import host_name
            return tuple(name for name, profile in profiles.items() if profile.host == host_name())
        requested_profile = ''
        for index, token in enumerate(tokens):
            if token.startswith('--profile='):
                requested_profile = token.partition('=')[2]
            elif token == '--profile' and index + 1 < len(tokens):
                requested_profile = tokens[index + 1]
        session = (session_state or {}).get('build_session')
        context = getattr(session, 'context', None)
        if not requested_profile and context is not None:
            profile = context.profile
        else:
            config = load_local_config(paths.local_config_file)
            profile = select_profile(profiles, requested=requested_profile, configured=config.default_build_profile, profile_file=paths.profile_file)
        return (*profile.presets, *(str(index) for index in range(1, len(profile.presets) + 1)))
    except (DevToolError, OSError, ValueError):
        # Incomplete checkout/configuration must not interrupt line editing.
        return ()


def shell_completion_candidates(
    registry: CommandRegistry,
    line: str,
    cursor_position: int | None = None,
    *,
    repository_context: RepositoryContext | None = None,
    session_state: dict[str, object] | None = None,
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

    if completed[0].casefold() == "help":
        candidates = registry.specifications
        for name in completed[1:]:
            spec = next((item for item in candidates if item.name == name.casefold()), None)
            if spec is None:
                return ()
            candidates = spec.subcommands
        return tuple(item.name for item in candidates if item.name.casefold().startswith(prefix.casefold()))

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
            if current.default_subcommand and completed[argument_index].startswith("-"):
                current = next(
                    child for child in current.subcommands
                    if child.name == current.default_subcommand
                )
            break
        current = child
        argument_index += 1

    if current.subcommands and argument_index == len(completed):
        if current.default_subcommand and prefix.startswith("-"):
            current = next(
                child for child in current.subcommands
                if child.name == current.default_subcommand
            )
        else:
            return tuple(
                name
                for name in (*(child.name for child in current.subcommands), '-h', '--help')
                if name.casefold().startswith(prefix.casefold())
            )

    if current.name == "run" and "--" in completed[1:]:
        return ()

    def values(argument: ArgumentSpec, value_prefix: str) -> tuple[str, ...]:
        if argument.kwargs.get('choices') is not None:
            return _matching_choices(argument, value_prefix)
        return tuple(value for value in _repository_choices(argument, completed[argument_index:], repository_context, session_state)
                     if value.casefold().startswith(value_prefix.casefold()))

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
        if option_argument is not None:
            return tuple(
                f"{option}={choice}"
                for choice in values(option_argument, value_prefix)
            )

    pending = _pending_argument(current.arguments, completed[argument_index:])
    if pending is not None and not prefix.startswith('-'):
        return values(pending, prefix)

    options = (
        flag
        for argument in current.arguments
        if argument.kwargs.get("help") is not argparse.SUPPRESS
        for flag in argument.flags
        if flag.startswith("-")
    )
    return tuple(
        option
        for option in (*options, '-h', '--help')
        if option.casefold().startswith(prefix.casefold())
    )


def _configure_readline_completion(registry: CommandRegistry, **context: object) -> None:
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
                **context,
            )
        return matches[state] if state < len(matches) else None

    readline.set_completer(complete)
    readline.set_completer_delims(" \t\n")
    if "libedit" in (readline.__doc__ or ""):
        readline.parse_and_bind("bind ^I rl_complete")
    else:
        readline.parse_and_bind("tab: complete")


def _interactive_input(registry: CommandRegistry, stdout: TextIO, **context: object) -> Callable[[str], str]:
    if not sys.stdin.isatty() or not stdout.isatty():
        return input
    try:
        from prompt_toolkit import PromptSession
        from prompt_toolkit.completion import Completer, Completion
    except ImportError:
        _configure_readline_completion(registry, **context)
        return input

    class RegistryCompleter(Completer):
        def get_completions(self, document: object, _complete_event: object):
            text = getattr(document, "text", "")
            cursor_position = getattr(document, "cursor_position", len(text))
            before_cursor = text[:cursor_position]
            match = re.search(r"(?:^|\s)(\S*)$", before_cursor)
            prefix = match.group(1) if match else ""
            for candidate in shell_completion_candidates(
                registry, text, cursor_position, **context
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
    print("Type help for available commands; press Tab to complete commands and values.", file=stdout)
    session_state: dict[str, object] = {}
    repository = repository_context or RepositoryContext.load(repository_root)
    read_input = (
        _interactive_input(registry, stdout, repository_context=repository, session_state=session_state) if input_func is input else input_func
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
                color=str(getattr(namespace, "color", "auto")),
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
