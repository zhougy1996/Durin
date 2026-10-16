# Platform Processes

Summary: Define child argument, working-directory and environment configuration for Windows and macOS process launch.
Modules: Core

Last reviewed: 2026-10-16

## Launch Options

`HAL/PlatformProcess.h` exposes `LaunchProcess` and `ExecuteProcess` with an
absolute UTF-8 executable filename and `FProcessLaunchOptions`. Both use the
same configuration and validation. There is no implicit PATH search or shell.

- `Arguments` is an owned array of UTF-8 strings excluding `argv[0]`. Each
  element is one child argument, including empty strings. Callers do not add
  quotes or escapes. Spaces, quotes, backslashes, Unicode and shell syntax
  remain literal data. Shell scripts require explicitly invoking a shell and
  passing its own command argument.
- `WorkingDirectory` is a native `std::filesystem::path`. Empty inherits the
  parent directory; otherwise it must be absolute. It affects only the child,
  and never changes executable-path resolution.
- `EnvironmentOverrides` inherits the parent environment and sets or removes
  selected variables. A string sets the value, including an empty string;
  `std::nullopt` removes the variable. Names must be nonempty UTF-8 without
  `=` or NUL, and values must be UTF-8 without NUL. Values can contain `=`.

The executable and arguments reject embedded NULs and malformed UTF-8. An
explicit working directory rejects NULs and relative paths. Invalid options
return `EPlatformProcessError::InvalidArguments` before creating a child.
Missing executables, nonexistent working directories and native spawn failures
return `Launch` errors with the executable path and native error code.
Allocation failures can still throw.

```cpp
FProcessLaunchOptions Options;
Options.Arguments = {"--output", "a file with spaces", ""};
Options.WorkingDirectory = NativeAbsoluteDirectory;
Options.EnvironmentOverrides = {
    {"TOOL_MODE", "batch"},
    {"EMPTY_VALUE", ""},
    {"REMOVE_FROM_CHILD", std::nullopt}
};
const auto Result = FPlatformProcess::ExecuteProcess(AbsoluteExecutableUtf8, Options);
```

## Platform Behavior

Windows passes an explicit application filename to `CreateProcessW`, with
Microsoft CRT argument escaping for each array element. This includes doubling
backslashes before quotes and closing quotes. A command line exceeding 32767
UTF-16 units including its terminator is rejected. Programs using a custom
command-line parser must account for their own conventions; a Windows command
line does not expose a universal native argv array.

Windows overrides use a Unicode environment block sorted with ordinal,
case-insensitive name comparison. Inherited per-drive entries are preserved.
Override names that differ only in case are rejected to avoid ambiguous updates.
Parent environment enumeration uses `GetEnvironmentStringsW`; no parent
`SetEnvironmentVariable` calls are made.

macOS passes the array directly to `posix_spawn`. Working directories use
`posix_spawn_file_actions_addchdir_np` (macOS 10.15 or later). Environment
overrides are case-sensitive and merge into a child-owned snapshot obtained
through `_NSGetEnviron`. No parent `chdir`, `setenv` or `unsetenv` is performed.
Callers must synchronize unrelated concurrent parent environment mutations
while a launch snapshots or inherits it.

`LaunchProcess` reports successful creation and returns without waiting for
exit. Native handles are closed on Windows; a detached waiter reaps the child
on macOS. It does not report a later child exit status.
`ExecuteProcess` waits and returns an exit code as a successful value, including
nonzero codes. macOS signal termination maps to `128 + signal`. Waiting errors
remain distinct from child exit codes. Neither operation captures output or
provides timeout, cancellation or an owned child-process handle.

## Consumers and Verification

Editor relaunch and Tracy helpers construct argument arrays. macOS `OpenPath`
passes `--` and the path as separate `/usr/bin/open` arguments. The former
platform-dependent command-string parser is removed.

`PlatformProcessContractTests` launches an independent executable which records
the received arguments, directory and environment. Coverage includes quoting
boundaries, empty arguments, Unicode executable paths and environment names,
inheritance, replacement, deletion, empty values, parent isolation, asynchronous
launch, invalid options, nonexistent directories, and nonzero exit status.
