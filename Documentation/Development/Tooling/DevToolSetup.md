# DevTool Setup Configuration

Machine-local configuration is the primary setup interface, including when an AI
assistant prepares the environment. Interactive selection is a fallback for
unconfigured machines. Setup never opens file browsing windows.

## Configure With An AI Assistant

1. Read [the local template](../../../Templates/DurinDevTool/DevTool.user.json)
   and [the schema](../../../Tools/DurinDevTool/DevTool.user.schema.json).
   Create `.agents/DevTool.user.json` from the template if missing; otherwise
   preserve existing settings and edit only the needed overrides. The local file
   is ignored by Git. Machine paths belong there, not in tracked presets or
   `Tools/DurinDevTool/DevTool.json`.
2. Discover installed tools on the machine. On Windows,
   `toolchain.environmentScript` selects the installation's
   `Common7/Tools/VsDevCmd.bat`; `cmake.command` selects the CMake executable.
   Empty `toolchain.environmentArguments` uses the Windows x64 defaults
   (`-arch=x64`, `-host_arch=x64`). Preserve intentional custom arguments.
3. Run non-interactive setup directly. It shares the same preparation stage as
   the optional check: read configuration, resolve and validate the toolchain,
   then check the remaining prerequisites before writing setup files or installing
   packages. Resolve reported errors and rerun the command.

```powershell
.\DevTool.bat setup --non-interactive
```

Use `.\DevTool.bat setup --check` only when you want to inspect the environment
without initializing it. It prints configuration, template and schema locations,
uses the same preparation stage, and stops before any setup writes or installs.
A separate check is not required before normal setup. Neither unattended command
prompts; a missing or ambiguous component reports the field to edit.

On macOS, use `./DevTool` instead. The normal environment is inherited from the
process, optionally supplemented by `toolchain.environmentScript`; see the
[macOS workflow](../Build/BuildAndRun.md#macos-workflow).
In linked worktrees, check the shared configuration with `setup --check`, then use
`worktree prepare` instead of `setup` to initialize the checkout.

An example request to an AI assistant:

> Inspect this machine and configure `.agents/DevTool.user.json` using the
> repository template and schema. Preserve unrelated settings. Select installed
> tools explicitly when discovery is ambiguous. Check `CMAKE_COMMAND`, which
> overrides `cmake.command`. Run `DevTool setup --non-interactive` in the main
> checkout, or `DevTool worktree prepare` in a linked worktree. Resolve reported
> errors and rerun until initialization succeeds. Use `DevTool setup --check`
> instead if only environment inspection is requested.

## Selection And Repair

Explicit configuration takes precedence over discovery. `CMAKE_COMMAND` takes
precedence over `cmake.command`; an obsolete environment override must be corrected
or unset. Non-interactive setup and `--check` reject an invalid explicit setting
instead of silently replacing it with another installation. Check failures do not
rewrite the configuration. Read the prerequisite diagnostics for missing SDKs or
system requirements; toolchain paths alone cannot repair them.

For unconfigured Windows components, setup discovers Visual Studio C++
installations and CMake executables from the selected Visual Studio and PATH.
Duplicate paths appear once. Every candidate is validated before selection;
unusable candidates are reported and excluded. A single usable candidate is
selected automatically. Multiple usable candidates require an explicit numbered
choice in an interactive terminal; unattended commands list them and fail so the
configuration can pin the intended installation.

If no usable candidate exists, interactive setup accepts a text path. Visual
Studio accepts either its installation directory or its environment script;
CMake accepts an executable path. Quoted paths and environment variables are
supported. A failed manual value retries that component. Selecting CMake preserves
the validated Visual Studio environment and arguments. Interactive setup can also
repair invalid configured settings through discovery or text input. Validated
paths are saved only after all prerequisite checks pass during normal setup.
