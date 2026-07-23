# Editor Console

The Console combines structured engine logs with a command prompt. Open it from
the editor's window menu when it is hidden.

## Log History

The Console reads the logger's bounded structured history for the current
process session. It starts at sequence 1, so opening the panel after editor
startup still shows the oldest startup records that remain in the logger's
retained window. It does not load logs from previous process sessions.

Records are consumed in sequence order through a cursor. The editor reads at
most one bounded batch per frame and continues consuming while the panel is
hidden, so opening or closing the panel does not change which records are
observed. Sink thresholds only control terminal and file output; Console
history retention is unfiltered and can include Trace through Fatal.

Logger history and the Console display are separate bounded windows. The
logger retains 5,000 records by default; `Logging.HistoryCapacity` may configure
that window from 256 through 65,536 records. The Console keeps at most 5,000
combined log, command, result, gap-warning, and error records. If the Console's
cursor falls behind evicted logger history, it resumes at the oldest available
sequence and displays the cumulative number of skipped records. If the
asynchronous producer queue drops lower-priority records under overload, the
logger emits an ordered Warn summary when capacity becomes available.

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
- The level buttons independently show or hide Trace, Debug, Info, Warn, Error,
  and Fatal log records. They do not hide command results. Fatal uses the error
  presentation color but remains an independent filter level.
- **Follow** keeps the view at the newest record while the user is already at
  the bottom. Scrolling upward temporarily suspends following.
- **Copy** copies the currently visible, filtered records.
- **Clear** has the same behavior as the `clear` command.

**Clear** removes the Console's currently displayed records but does not rewind
its logger cursor, delete retained logger history, or truncate the log file.
Already consumed records are therefore not replayed after a clear; new records
continue from the current sequence. Log files continue to use the normal
runtime log path documented in `Documentation/Setup/BuildAndRun.md`.

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
