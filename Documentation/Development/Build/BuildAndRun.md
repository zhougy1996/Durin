# Build And Run

This is the operational guide for configuring, building, testing, and debugging Durin locally.

## Setup

Install the following Windows prerequisites, then run `Setup.bat` once in the
main checkout. Create linked worktrees with `WorktreeTool add`, or initialize an
existing linked worktree with `WorktreeTool prepare`:

- Python 3.10 or newer, including `venv`.
- Windows 10 version 1607 or newer with **Enable Win32 long paths** enabled under
  `Computer Configuration > Administrative Templates > System > Filesystem`.
  This policy sets
  `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled` to
  `REG_DWORD 1`; restart Windows after changing it. Setup and BuildTool report a
  missing policy but never change machine state.
- Visual Studio 2022 or newer with the **Desktop development with C++** workload, x64 MSVC tools, a Windows SDK, and the English language pack.
- MSVC Build Tools 14.44 or newer (Visual Studio 2022 17.14). Durin uses C++20 standard-library features including `std::format_string`, `std::format`, and `std::source_location`.
- Git, CMake 3.24 or newer, and Ninja. The Ninja bundled with Visual Studio is accepted.
- The LunarG Vulkan SDK. `VULKAN_SDK` must name an installation containing `Include/vulkan/vulkan.h`, `Include/vma/vk_mem_alloc.h`, and `Lib/vulkan-1.lib`. Current SDK releases include VMA. For an older SDK, either update it or download `vk_mem_alloc.h` from VulkanMemoryAllocator and place it under that SDK's `Include/vma` directory; Durin does not bootstrap a second VMA copy.

In a normal checkout, `Setup.bat` first creates `.agents/build-config.json` from its tracked template when the local file is missing, then runs a non-mutating prerequisite check before it creates a virtual environment, downloads packages, or builds third-party libraries. Existing local configuration is never overwritten, so if preflight reports a tool that automatic detection cannot find, edit the configuration and rerun `Setup.bat`. Preflight reports all detected prerequisite problems together so an old MSVC toolset, incomplete Vulkan SDK, or missing command does not first appear halfway through bootstrap or during the main build. BuildTool separately validates the Visual Studio English language pack when it first initializes MSVC. Setup initializes only the main checkout; a linked worktree exits with an error directing the caller to `WorktreeTool prepare`.

In a normal checkout, `Setup.bat` creates `.venv`, installs the pinned Python dependencies from `requirements.txt` (including the `clang.cindex` bindings and native `libclang` library), and prepares all repository-managed third-party libraries. The operation is idempotent and can be rerun after a failed prerequisite check or interrupted download. `WorktreeTool prepare` owns linked-worktree preparation: it links `.agents`, `Engine/External`, and `.venv` from the prepared source worktree before running preflight.

`BuildTool.bat` intentionally requires `.venv`; it will ask for `Setup.bat` in
the main checkout or `WorktreeTool prepare` in a linked worktree rather than
silently using a different system Python.

Machine-specific CMake, build-profile, environment, or job overrides belong in
`.agents/build-config.json`. Normally, leave them empty and let the build driver
detect the Visual Studio environment and parallelism. Setup's preflight honors
both `cmakeCommand` and `environmentSetup`; the third-party bootstrap also uses
`cmakeCommand`, and `CMAKE_COMMAND` still takes precedence during Setup.

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

Child-process output has four modes, selected with
`--output auto|compact|progress|full`. The default `auto` mode selects progress
output in an interactive terminal and compact output when stdout is redirected
or consumed by an Agent. Progress mode updates routine Ninja `[n/total]` status
in place while preserving other child output, compiler diagnostics, and the
complete command log. When explicitly requested without an interactive terminal,
progress mode falls back to compact output.
Compact mode keeps stage boundaries, command lines, heartbeats, and final
results, but suppresses routine CMake, Ninja, and successful GoogleTest lines.
It writes the complete raw output under `Build/.agent-state/logs/`, reports the
log path after each successful child command, and prints a bounded diagnostic
excerpt plus the log path when a child fails. The newest 40 command logs are
retained. Use `--output full` to stream every child-output line, or
`--output compact` to suppress routine child output in an interactive terminal:

```powershell
.\BuildTool build --target all --output progress
.\BuildTool build --target all --output compact
.\BuildTool test --target CoreTests --output full
```

Compact native-test runs also enable GoogleTest's brief output mode. Test
failures and the final test summary remain in the captured output and failure
excerpt; application or library messages are always preserved in the full log.
`--plain` controls styling independently and does not select an output volume.

While a build, configure, clean, or test child command is alive, BuildTool emits a short heartbeat every 30 seconds until the child produces a final result. This distinguishes a genuinely running operation from a completed command without requiring a second status or build invocation. The interactive `run` command suppresses this heartbeat because the runtime is expected to remain open until the user exits it.

On Windows, the first toolchain-backed command captures and validates the Visual Studio environment. BuildTool caches that environment delta under `Build/.agent-state/` so later invocations avoid rerunning `VsDevCmd.bat` and the compiler language probe. The cache refreshes automatically when the setup script, its arguments, or `cl.exe` changes, while caller-provided environment values and `PATH` changes remain live.

The registered Windows build environment defaults to `Win64-Debug-DurinEditor-Tests`, allowing the same output set to run the editor and native tests. Before launching the editor for a smoke test or final validation, build the complete runtime:

```powershell
.\BuildTool build --target all
.\BuildTool run
.\BuildTool run --project Sandbox\Sandbox.dproject
```

`run` launches the existing runtime executable selected by the preset, such as
`DurinEditor.exe` or `DurinGame.exe`; it does not build implicitly. On Windows,
BuildTool keeps relaunched runtime descendants in the same tracked process job,
so opening another editor project does not return from `run` or release the
checkout lock until the final editor instance exits.

Use `--project <descriptor>` to select an existing `.dproject` explicitly. A
relative descriptor is resolved from the workspace root; an absolute path is
also accepted, including projects outside the workspace. BuildTool rejects a
missing file or another extension, normalizes the path to an absolute path, and
passes one `--project=<absolute-path>` argument to the launcher. This validation
does not initialize CMake, Visual Studio, or the compiler toolchain.

Pass other runtime arguments after the final `--args` option. The typed project
argument is always forwarded before them:

```powershell
.\BuildTool run --preset Win64-Debug-DurinGame --project Sandbox\Sandbox.dproject --args -ExampleArgument
```

Do not repeat `--project` or `--project=...` after `--args` when the typed
`--project` option is present; BuildTool rejects the two project selectors as
ambiguous. Raw project selection after `--args` remains accepted for backwards
compatibility when the typed option is absent.

For unattended runtime smoke tests, pass `--hidden-window` to suppress every
native application window, including secondary UI viewports:

```powershell
.\BuildTool run --project Sandbox\Sandbox.dproject --args --hidden-window
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
.\BuildTool build --preset Win64-Release-DurinEditor-Profiling --target all
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
   1  Win64-Debug-DurinEditor-Tests [default, current]
   2  Win64-Debug-DurinEditor
   3  Win64-Release-DurinEditor
   4  Win64-Release-DurinEditor-Profiling
   5  Win64-Debug-DurinGame
   6  Win64-Release-DurinGame
   7  Win64-Release-DurinGame-Profiling
   8  Win64-Shipping-DurinGame
Preset> 3
BuildTool> preset
CMake preset: "Win64-Release-DurinEditor"
BuildTool> preset Win64-Debug-DurinGame
BuildTool> configure --fresh
BuildTool> build
BuildTool> rebuild --target DurinLauncher
BuildTool> test --target CoreTests --filter FJsonDocumentTests.* --timeout 300
BuildTool> run --project Sandbox\Sandbox.dproject --args --hidden-window
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
runtime executable and returns to the shell when it exits. Its typed
`--project <descriptor>` option follows the same normalization and conflict
rules as the direct command; place it before compact runtime arguments.
`open-runtime` opens
the selected preset's existing runtime directory in the platform file manager.
`status` reports the host build profile, preset, runtime variant, build
directory, configuration, recovery
state, and whether CMake, parallelism, and the toolchain environment are resolved
or still deferred. `stop` stops an operation held by another BuildTool process.
Use `help` for the complete command list. A leading slash remains accepted for
compatibility but is not required.

## Creating Modules

Create a workspace module with one BuildTool command. The project descriptor
may be relative to the workspace root or absolute:

```powershell
.\BuildTool create module Gameplay --project Sandbox\Sandbox.dproject --kind runtime --private-dependency Core --private-dependency Engine
.\BuildTool create module SceneEditor --project Sandbox\Sandbox.dproject --kind editor --private-dependency DurinEd
```

Without `--path`, runtime modules are created under
`Source/Runtime/<ModuleName>` and added to `BaseModules`; editor modules are
created under `Source/Editor/<ModuleName>` and added to
`ExtraModules.DurinEditor.Modules`. The directory names are scaffolding
defaults, not build-system classification. Use `--path <ProjectRelativePath>`
to place the module elsewhere inside its owning project:

```powershell
.\BuildTool create module Combat --project MyGame\MyGame.dproject --path Source\Game\Combat --private-dependency Core
.\BuildTool create module WorldTools --project MyGame\MyGame.dproject --path "Source\Tools\World Tools" --kind editor --private-dependency Core
```

An absolute `--path` is also accepted when it resolves inside the owning
project. The destination must be new and cannot contain, be contained by, or
otherwise overlap an existing module root. `--kind` still selects the default
path when `--path` is omitted and controls the default enablement; it does not
otherwise classify the custom directory.

Use `--enable none`, `--enable base`, or repeat
`--enable <RuntimeVariant>` to replace the default enablement. Dependency options
are repeatable:

```text
--public-dependency
--private-dependency
--optional-public-dependency
--optional-private-dependency
```

Shared linkage and a self PCH are the defaults. `--link static` selects static
linkage, while `--pch <Name>` selects an existing shared PCH. A self-PCH module
also receives `Private/PCH.<ModuleName>.h`. The minimal generated entry point
uses Core's module interface, so include `Core` directly or through a dependency
whose public interface exposes Core.

Within the selected module root, ordinary source discovery currently uses the
generated `Public/` and `Private/` directories recursively. A project may choose
any higher-level organization—such as `Source/Game`, `Source/Features`, or
`Source/Tools`—while retaining those module-internal visibility directories.

Preview the complete operation without creating directories, temporary files,
or descriptor edits:

```powershell
.\BuildTool create module Gameplay --project Sandbox\Sandbox.dproject --private-dependency Core --dry-run --plain
```

Creation validates names, workspace-wide dependencies, runtime variants, paths, and
CMake target collisions before writing. During mutation, new files and
directories are tracked and the prior project descriptor is backed up. Any
write or final descriptor-validation failure restores the original bytes and
removes only paths created by that invocation. Repeating a successful request
reports an existing-name or destination conflict instead of overwriting it.
The same `create module` syntax is available in the interactive BuildTool shell.

## Creating Workspace Projects

Create and register a workspace-local project with one command:

```powershell
.\BuildTool create project MyGame --path MyGame
```

The project path may be relative to the workspace root or an absolute path
resolving to the same location. In the current workspace-local workflow, it
must be a new direct child of the workspace root. Project names must be valid
C++ identifiers and case-insensitively unique among projects, modules, and
CMake targets.

The command creates `MyGame.dproject`, the project `CMakeLists.txt`,
`CMake/MyGameSetup.cmake`, empty `Configs/` and `Content/` roots, and a
same-named runtime module under `Source/Runtime/MyGame`. The initial module is
enabled in `BaseModules`, uses the self PCH and shared-linkage defaults, and
depends privately on `Core`. BuildTool also appends one quoted
`add_subdirectory(...)` registration after the existing workspace project
registrations in the root `CMakeLists.txt`.

Preview the complete operation without changing the workspace:

```powershell
.\BuildTool create project MyGame --path MyGame --dry-run --plain
```

Project creation validates containment, project/module/target name collisions,
existing and overlapping destinations, and the root CMake registration before
writing. The project tree and root CMake edit are one transaction: a failure
restores the previous root file byte-for-byte and removes only paths created by
that invocation. Installed-engine projects, external project roots, and nested
workspace project paths are not supported by this command. The same syntax is
available in the interactive BuildTool shell.

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

Preset build trees are isolated, third-party runtime DLLs are shared by
platform/configuration, and DHT metadata is shared by platform/runtime variant.
Purging one preset therefore also invalidates those shared artifacts for other
presets using the same configuration or runtime variant. A subsequent build
regenerates them normally.

Purge only removes registered preset trees under `Build/` and `Install/`,
project `Binaries/<Platform>/<OutputConfig>/` roots, shared
`Binaries/<Platform>/ThirdParty/<CMakeConfig>/` roots, and project
`Intermediate/Build/<Platform>/<RuntimeVariant>/` roots. It intentionally
preserves bootstrapped dependencies such as `Build/ThirdParty` and
`Engine/External`.

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
- Runtime launcher and modules: `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/`
- Third-party runtime DLLs: `Engine/Binaries/<Platform>/ThirdParty/<Config>/`
- Native tests: `Engine/Binaries/<Platform>/<Config>/Tests/<RuntimeVariant>/Bin/`

The launcher target is `DurinLauncher`, while the executable name follows the
active runtime variant. Runtime path discovery assumes the executable remains in
this repository-relative layout. If editor startup reports a missing DLL, check
the active runtime directory and the shared configuration-specific `ThirdParty`
directory.

DHT intermediate paths are described in `Documentation/Development/Build/BuildSystem.md` and
`Documentation/Development/Build/RuntimeVariants.md`.
The opt-in Release profiling workflow is documented in
`Documentation/Development/Build/Profiling.md`.

## Related Docs

- `Documentation/Development/Build/ThirdPartyBootstrap.md`
- `Documentation/Development/Build/NativeTests.md`
- `Documentation/Development/Tooling/IDECodeModel.md`
- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/RuntimeVariants.md`
- `Documentation/Development/Build/Profiling.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
