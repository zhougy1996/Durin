# Build And Run

This is the operational guide for configuring, building, testing, and debugging Durin locally.

## Setup

Install the following Windows prerequisites, then run `Setup.bat` once in the
main checkout. Create linked worktrees with `WorktreeTool add`, or initialize an
existing linked worktree with `WorktreeTool prepare`:

- Python 3.10 or newer, including `venv`.
- Visual Studio 2022 or newer with the **Desktop development with C++** workload, x64 MSVC tools, a Windows SDK, and the English language pack.
- MSVC Build Tools 14.44 or newer (Visual Studio 2022 17.14). Durin uses C++20 standard-library features including `std::format_string`, `std::format`, and `std::source_location`.
- Git, CMake 3.24 or newer, and Ninja. The Ninja bundled with Visual Studio is accepted.
- The LunarG Vulkan SDK. `VULKAN_SDK` must name an installation containing `Include/vulkan/vulkan.h`, `Include/vma/vk_mem_alloc.h`, and `Lib/vulkan-1.lib`. Current SDK releases include VMA. For an older SDK, either update it or download `vk_mem_alloc.h` from VulkanMemoryAllocator and place it under that SDK's `Include/vma` directory; Durin does not bootstrap a second VMA copy.

In a normal checkout, `Setup.bat` first creates `.agents/build-config.json` from its tracked template when the local file is missing, then runs a non-mutating prerequisite check before it creates a virtual environment, downloads packages, or builds third-party libraries. Existing local configuration is never overwritten, so if preflight reports a tool that automatic detection cannot find, edit the configuration and rerun `Setup.bat`. Preflight reports all detected prerequisite problems together so an old MSVC toolset, incomplete Vulkan SDK, or missing command does not first appear halfway through bootstrap or during the main build. BuildTool separately validates the Visual Studio English language pack when it first initializes MSVC. Setup initializes only the main checkout; a linked worktree exits with an error directing the caller to `WorktreeTool prepare`.

In a normal checkout, `Setup.bat` creates `.venv`, installs the pinned Python dependencies from `requirements.txt` (including the `clang.cindex` bindings and native `libclang` library), and prepares all repository-managed third-party libraries. The operation is idempotent and can be rerun after a failed prerequisite check or interrupted download. `WorktreeTool prepare` owns linked-worktree preparation: it links `.agents`, `Engine/External`, and `.venv` from the prepared source worktree before running preflight.

`BuildTool.bat` intentionally requires `.venv`; it will ask for `Setup.bat` in
the main checkout or `WorktreeTool prepare` in a linked worktree rather than
silently using a different system Python.

Machine-specific CMake, profile, environment, or job overrides belong in `.agents/build-config.json`. Normally, leave them empty and let the build driver detect the Visual Studio environment and parallelism. Setup's preflight honors both `cmakeCommand` and `environmentSetup`; the third-party bootstrap also uses `cmakeCommand`, and `CMAKE_COMMAND` still takes precedence during Setup.

## Windows Workflow

A checkout has one source/build writer at a time. An Agent may own the current checkout; a separate worktree is needed only for concurrent Agents, branches, or human editing/build workflows. An IDE may observe and debug an Agent-owned checkout, but it must not build it.

Use the root WorktreeTool to create, inspect, open, and remove linked worktrees:

```powershell
.\WorktreeTool add ..\Durin-feature -b feature-branch
.\WorktreeTool prepare ..\Durin-feature
.\WorktreeTool list
.\WorktreeTool
.\WorktreeTool remove ..\Durin-feature
```

With no arguments, WorktreeTool opens all registered worktrees in Windows
Terminal. `add` creates and prepares a worktree. `prepare` is idempotent and can
initialize manually created worktrees or repair their shared links; omit its path
to target the current checkout, pass `--source` for a non-default prepared
worktree, or pass `--dry-run` to preview the operation. `remove` is the required
deletion path for prepared Windows worktrees because it validates and detaches
the shared `.agents`, `.venv`, and `Engine/External` directory junctions before
invoking Git. Do not recursively delete a prepared worktree or call
`git worktree remove` directly: a deletion implementation that follows an NTFS
junction can erase the corresponding directory in the main worktree. Use
`remove --dry-run` to inspect the operation, and pass `--force` only to discard
modified or untracked files.

Use the root wrapper for configuration, builds, and tests:

```powershell
.\BuildTool configure
.\BuildTool configure --fresh
.\BuildTool build
.\BuildTool build --target LevelEditor
.\BuildTool run
.\BuildTool test --target CoreTests --filter FJsonDocumentTests.*
.\BuildTool clean
.\BuildTool rebuild --target all
.\BuildTool presets
.\BuildTool status
.\BuildTool open-runtime
.\BuildTool stop
```

Commands are case-insensitive for compatibility, but lowercase is canonical. `build` and `test` configure automatically when needed, so an explicit first `configure` is optional. Omit `--jobs` to use automatic parallelism; pass `--jobs <count>` only when a local limit is required. From another batch file, use `call BuildTool.bat <arguments>`.

`build` and `rebuild` default to target `all`; `test` always requires an explicit
`--target`. `presets`, `status`, and `open-runtime` are also available directly,
so preset discovery, context inspection, and runtime-directory access do
not require entering the interactive shell.

An ordinary `configure` preserves the existing CMake cache. Pass `--fresh` to discard it explicitly. `rebuild` and automatic recovery from an unusable or incompatible build tree always fresh-configure before building.

BuildTool separates its resolved context, execution stages, raw CMake/Ninja output, and final result so failures remain identifiable in long logs. Styled output is enabled for interactive terminals. Pass `--plain`, set `NO_COLOR`, or redirect the output to select stable text-only output without ANSI sequences:

```powershell
.\BuildTool build --target all --plain
```

While a build, configure, clean, or test child command is alive, BuildTool emits a short heartbeat every 30 seconds until the child produces a final result. This distinguishes a genuinely running operation from a completed command without requiring a second status or build invocation. The interactive `run` command suppresses this heartbeat because the runtime is expected to remain open until the user exits it.

On Windows, the first toolchain-backed command captures and validates the Visual Studio environment. BuildTool caches that environment delta under `Build/.agent-state/` so later invocations avoid rerunning `VsDevCmd.bat` and the compiler language probe. The cache refreshes automatically when the setup script, its arguments, or `cl.exe` changes, while caller-provided environment values and `PATH` changes remain live.

The registered Windows build environment defaults to `Win64-Debug-DurinEditor-Tests`, allowing the same output set to run the editor and native tests. Before launching the editor for a smoke test or final validation, build the complete runtime:

```powershell
.\BuildTool build --target all
.\BuildTool run
```

`run` launches the existing runtime executable selected by the preset, such as
`DurinEditor.exe` or `DurinGame.exe`; it does not build implicitly. On Windows,
BuildTool keeps relaunched runtime descendants in the same tracked process job,
so opening another editor project does not return from `run` or release the
checkout lock until the final editor instance exits. Pass runtime arguments after
the final `--args` option:

```powershell
.\BuildTool run --preset Win64-Debug-DurinGame --args -ExampleArgument
```

For unattended runtime smoke tests, pass `--hidden-window` to suppress every
native application window, including secondary UI viewports:

```powershell
.\BuildTool run --args --hidden-window
```

An interactive `BuildTool run` can be stopped with Ctrl+C; BuildTool terminates
the tracked application job and any relaunched descendants. For a timed Windows
smoke test, launch the runtime with PowerShell `Start-Process`, pass
`--hidden-window`, retain the process returned by `-PassThru`, and stop that
process after verification. `-WindowStyle Hidden` only affects process startup
and is not a substitute for the application argument.

Select another registered configure preset with `--preset`:

```powershell
.\BuildTool build --preset Win64-Release-DurinEditor --target all
.\BuildTool rebuild --preset Win64-Shipping-DurinGame --target all
```

`CMakePresets.json` remains the source of truth for preset configuration. `AgentBuildProfiles.json` controls which presets BuildTool may own for each host environment. The IDE-only `Win64-Debug-DurinEditor-FastConfigure` preset is intentionally excluded.

FastConfigure is marked code-model-only. Its generated build targets fail before
DHT, compilation, or linking can start; it may only configure for IDE indexing.

## Interactive Shell

Run `BuildTool` without arguments, or pass `shell`, to open the human-oriented command shell:

```powershell
.\BuildTool
.\BuildTool shell
```

Opening the shell loads repository configuration, build profiles, and registered
presets, but does not initialize CMake, Visual Studio, or the compiler toolchain.
The first `configure`, `build`, `clean`, `rebuild`, or `test` command resolves and
validates the toolchain once; later commands and preset switches reuse that
environment for the rest of the session. Read-only and artifact commands remain
available when the compiler toolchain is unavailable.

If Ctrl+C is not forwarded by the terminal or batch wrapper, run `.\BuildTool stop` from a second terminal, or enter `stop` in another already-open BuildTool shell. It stops the active BuildTool process and its complete CMake/Ninja child process tree for this checkout. The foreground shell cannot accept `stop` while it is waiting for its own operation, so stopping that operation requires another process.

The selected preset is session-local and does not modify `.agents/build-config.json`:

```text
BuildTool> presets
   1  Win64-Debug-DurinEditor
   2  Win64-Debug-DurinEditor-Tests [default, current]
   3  Win64-Debug-DurinGame
   4  Win64-Release-DurinEditor
   5  Win64-Release-DurinGame
   6  Win64-Shipping-DurinGame
Preset> 4
BuildTool> preset
CMake preset: "Win64-Release-DurinEditor"
BuildTool> preset Win64-Debug-DurinGame
BuildTool> configure --fresh
BuildTool> build
BuildTool> rebuild --target DurinLauncher
BuildTool> test --target CoreTests --filter FJsonDocumentTests.* --timeout 300
BuildTool> run --args --hidden-window
BuildTool> open-runtime
BuildTool> status
BuildTool> stop
BuildTool> exit
```

`presets` displays the registered list and accepts a number on a distinct
`Preset>` prompt. Pressing Enter or Ctrl+C explicitly cancels selection without
changing the current preset; invalid numeric or non-numeric input is reported as
an invalid selection. `preset` without an argument displays the current preset;
with an argument it requires the full preset name. `build` and `rebuild` default
to target `all`. Shell commands accept the same named options as their direct
forms. The compact forms `build <target>`, `rebuild <target>`, `test <target>
[filter]`, and `run [arguments...]` remain accepted for compatibility, while help
shows the canonical named syntax. `run` launches the current preset's existing
runtime executable and returns to the shell when it exits. `open-runtime` opens
the selected preset's existing runtime directory in the platform file manager.
`status` reports the profile, preset, build directory, configuration, recovery
state, and whether CMake, parallelism, and the toolchain environment are resolved
or still deferred. `stop` stops an operation held by another BuildTool process.
Use `help` for the complete command list. A leading slash remains accepted for
compatibility but is not required.

## Clean And Purge

`clean` invokes the CMake clean target for the selected preset. It removes outputs known to that generated build graph, but keeps the configured CMake tree and may leave copied runtime files or generated metadata that CMake does not own.

`purge` removes the selected preset's configured build and install trees plus its project output and generated-metadata roots:

```powershell
.\BuildTool purge --preset Win64-Release-DurinEditor
.\BuildTool purge --preset Win64-Release-DurinEditor --yes
```

Inside the interactive shell:

```text
BuildTool> purge
BuildTool> purge --yes
BuildTool> purge --all-presets
```

Purge asks for explicit confirmation unless `--yes` is supplied: enter `PURGE` for the current preset or `PURGE ALL` for the all-presets scope. Use `--all-presets` to remove artifacts for every preset registered to the selected Agent host profile:

```powershell
.\BuildTool purge --all-presets
.\BuildTool purge --all-presets --yes
```

Preset build trees are isolated, but final binaries are shared by platform/configuration and DHT metadata is shared by platform/profile. Purging one preset therefore also invalidates those shared outputs for other presets using the same configuration or profile. A subsequent build regenerates them normally.

Purge only removes registered preset trees under `Build/` and `Install/`, project `Binaries/<Platform>/<Config>/` roots, and project `Intermediate/Build[-Identifier]/<Platform>/<Profile>/` roots. It intentionally preserves bootstrapped dependencies such as `Build/ThirdParty` and `Engine/External`.

On non-Windows hosts, invoke `.venv/bin/python Engine/Scripts/Build/durin_build_tool/__main__.py <arguments>` directly after preparing an equivalent virtual environment. Windows callers must use `BuildTool.bat`. BuildTool enforces `VSLANG=1033` after Visual Studio environment setup and verifies that MSVC actually emits English diagnostics. This keeps CMake's `/showIncludes` dependency prefix stable for both interactive terminals and Agent output pipes. If validation reports a localized prefix, add the English language pack through Visual Studio Installer. The next `configure`, `build`, or `test` refreshes any existing Ninja tree that does not already contain the English dependency prefix.

## IDE Code Model And Debugging

Use Visual Studio Code or CLion only for the code model, editing, and debugging.
Keep BuildTool as the checkout's only build owner. The complete setup for both
editors is documented in `Documentation/Development/Tooling/IDECodeModel.md`.

## Recovery

For Agent-driven `build` and `rebuild` commands, give the shell invocation a timeout of at least 10 minutes and raise it for a full build when prior measurements justify that. If the runner returns a running cell ID, wait on that same cell in intervals no longer than 60 seconds. Do not call `wait` after a final exit result. A runner yield, quiet output, or elapsed UI window alone does not mean that BuildTool stopped and must not trigger a second build or recovery-state inspection.

The recovery marker covers only operations that mutate configured or compiled build state. A normal compiler, linker, configuration, or clean failure removes the in-progress marker; fix the reported error and rerun the same command. For `test`, the marker is cleared as soon as its target finishes building, before the test executable starts. Failed assertions, test-process crashes, test timeouts, interrupted tests, and application exits therefore never require a rebuild.

Do not start a second build while an earlier CMake, Ninja, compiler, or linker process tree may still be running. If such a process is cancelled, externally terminated, or loses its controlling BuildTool process, wait for the process tree to exit and check `BuildTool status`. Run the following only when it reports `Recovery state: rebuild required`:

```powershell
.\BuildTool rebuild --target all
```

When the interrupted operation used a non-default preset, add `--preset <affected-preset>` or select that preset in the interactive shell before rebuilding.

Use the same recovery after an accidental IDE build. IDE outputs share the final binary directory, and their timestamps can make an incremental Agent build incorrectly report that everything is current. The driver also blocks unsafe incremental `build` and the build phase of `test` for the affected preset after a detected build-state interruption until a `rebuild` succeeds.

BuildTool serializes all registered presets with one checkout-level ownership lock because different CMake trees can still share final outputs and generated metadata. Do not mix direct CMake build operations with `BuildTool` ownership of the same checkout.

The checkout lock file normally remains on disk after BuildTool exits; the OS byte
lock, not the file's presence or its recorded PID, determines ownership. BuildTool
overwrites metadata when it acquires an unowned file and attempts to reset the
file ACL to inherit from `Build/.agent-locks`, so later invocations from another
Agent sandbox identity can reuse it. On Windows, BuildTool also attempts to replace an inaccessible stale
file; Windows refuses that replacement while a live BuildTool still has the file
open. If the directory ACL itself prevents recovery, the error distinguishes the
permission problem from a live lock and prints `icacls` and `Remove-Item` recovery
commands. Run those commands only after confirming that BuildTool, DurinEditor,
CMake, and Ninja have exited for the checkout.

## Output Layout

- Editor: `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`
- Runtime launcher and modules: `Engine/Binaries/<Platform>/<Config>/Runtime/<Profile>/`
- Third-party runtime DLLs: `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
- Native tests: `Engine/Binaries/<Platform>/<Config>/Tests/<Profile>/Bin/`

The launcher target is `DurinLauncher`, while the executable name follows the active profile. Runtime path discovery assumes the executable remains in this repository-relative layout. If editor startup reports a missing DLL, check the active runtime directory and the shared `ThirdParty` directory.

Build identifiers and DHT intermediate paths are described in `Documentation/Development/Build/BuildSystem.md` and `Documentation/Development/Build/Profiles.md`.

## Related Docs

- `Documentation/Development/Build/ThirdPartyBootstrap.md`
- `Documentation/Development/Build/NativeTests.md`
- `Documentation/Development/Tooling/IDECodeModel.md`
- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/Profiles.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
