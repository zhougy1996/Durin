# Editor Console

The Console combines structured engine logs with a command prompt. Open it from
the editor's window menu when it is hidden.

## Commands

Command names are case-insensitive. The editor currently provides these
commands:

| Command | Usage | Description |
| --- | --- | --- |
| `help` | `help [command]` | Lists every registered command, or shows the description and usage of one command. |
| `clear` | `clear` | Clears records displayed in the Console. It does not delete or truncate the log file. |

Other runtime or editor modules may register commands while they are loaded.
Use `help` in the Console for the authoritative list available in the current
session.

Examples:

```text
help
help clear
clear
```

## Input And Navigation

- Press **Enter** to execute the current command.
- Press **Tab** to complete a command name. When several commands match, the
  Console prints the candidates and completes their shared prefix.
- Press **Up** or **Down** to browse up to 100 commands from the current editor
  session.
- Arguments are separated by whitespace. Single or double quotes preserve
  whitespace, and a backslash escapes the following character.

```text
example plain "two words" 'three words' escaped\ value
```

## Output Controls

- **Search output** filters log messages, command echoes, results, and errors.
- The level buttons independently show or hide Trace, Debug, Info, Warn, and
  Error log records. They do not hide command results.
- **Follow** keeps the view at the newest record while the user is already at
  the bottom. Scrolling upward temporarily suspends following.
- **Copy** copies the currently visible, filtered records.
- **Clear** has the same behavior as the `clear` command.

The Console retains at most 5,000 displayed records. Log files continue to use
the normal runtime log path documented in `Documentation/Setup/BuildAndRun.md`.

## Registering A Command

Runtime modules register commands through `FConsoleCommandRegistry` in Core and
must unregister their handle during shutdown:

```cpp
const FConsoleCommandHandle Handle = FConsoleCommandRegistry::Get().RegisterCommand({
    "example",
    "Prints an example result.",
    "example [value]",
    [](std::span<const std::string> Args) {
        return FConsoleCommandResult::Success(Args.empty() ? "Example" : Args.front());
    }
});

FConsoleCommandRegistry::Get().UnregisterCommand(Handle);
```

Callbacks execute synchronously on the thread that calls `Execute`; the editor
prompt executes them on the main thread. Long-running commands should schedule
their own work instead of blocking the callback.
