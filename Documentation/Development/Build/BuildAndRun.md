# Build And Run

This is the operational guide for configuring, building, testing, and debugging Durin locally.

## Setup

Install the following Windows prerequisites, then run
`.\DevTool.bat setup` once in the main checkout. Create
linked worktrees with `DevTool worktree add`, or initialize an existing linked
worktree with `DevTool worktree prepare`:

- Python 3.10 or newer, including `venv`.
- Windows 10 version 1607 or newer with **Enable Win32 long paths** enabled under
  `Computer Configuration > Administrative Templates > System > Filesystem`.
  This policy sets
  `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled` to
  `REG_DWORD 1`; restart Windows after changing it. Setup and DurinDevTool report a
  missing policy but never change machine state.
- Visual Studio 2022 or newer with the **Desktop development with C++** workload, x64 MSVC tools, a Windows SDK, and the English language pack.
- MSVC Build Tools 14.44 or newer (Visual Studio 2022 17.14). Durin uses C++20 standard-library features including `std::format_string`, `std::format`, and `std::source_location`.
- Git, CMake 3.24 or newer, and Ninja. The Ninja bundled with Visual Studio is accepted.
- The LunarG Vulkan SDK. `VULKAN_SDK` must name an installation containing `Include/vulkan/vulkan.h`, `Include/vma/vk_mem_alloc.h`, and `Lib/vulkan-1.lib`. Current SDK releases include VMA. For an older SDK, either update it or download `vk_mem_alloc.h` from VulkanMemoryAllocator and place it under that SDK's `Include/vma` directory; Durin does not bootstrap a second VMA copy.

In a normal checkout, `DevTool setup` runs a non-mutating prerequisite check,
then creates `.agents/DevTool.user.json` from
`Templates/DurinDevTool/DevTool.user.json` when the
local file is missing. It also copies missing VS Code `settings.json` and
`extensions.json` files from `Templates/VSCode` into the ignored local
`.vscode` directory. When `launch.json` is missing, Setup generates one entry
for every preset registered to the selected Agent Build Profile, deriving each
runtime executable from the resolved CMake preset. Existing local configuration
files are never replaced; only the confirmed toolchain fields may be updated.
If preflight reports a tool that automatic detection cannot find, edit the
configuration and rerun `DevTool setup`.
Preflight reports all detected prerequisite problems together so an old MSVC
toolset, incomplete Vulkan SDK, or missing command does not first appear halfway
through bootstrap or during the main build. DurinDevTool separately validates
the Visual Studio English language pack when it first initializes MSVC. Setup
initializes only the main checkout; a linked worktree exits with an error
directing the caller to `DevTool worktree prepare`.
On the first interactive setup, automatic CMake and `VsDevCmd.bat` detection is
shown for confirmation. If detection is incomplete or the proposed settings are
declined, Setup prompts for the CMake executable, environment script, and script
arguments, validates them, and saves the confirmed absolute paths in
`.agents/DevTool.user.json`. Use `DevTool setup --non-interactive` for scripts
or CI; it accepts valid automatic or already-configured settings and fails with
an actionable message instead of waiting for input.
Because Setup must install DurinDevTool's Python packages, its terminal styling
uses a standard-library-only fallback until the prepared environment is ready.
`setup --plain` and `NO_COLOR` disable that styling as usual.

In a normal checkout, `DevTool setup` creates `.venv`, installs the pinned
Python dependencies from `requirements.txt` (including the `clang.cindex`
bindings and native `libclang` library), and prepares all repository-managed
third-party libraries, including development-only dependencies such as Tracy.
The confirmed Visual Studio environment is passed to every third-party CMake
configure and build subprocess, and later `DevTool dependency prepare` calls
recreate it from the saved local configuration.
The operation is idempotent and can be rerun after a failed prerequisite check
or interrupted download. `DevTool worktree prepare` owns linked-worktree
preparation: it links `.agents`, `.vscode`, `Engine/External`, and `.venv` from
the prepared source worktree before running preflight. If a linked worktree
already has a non-empty local `.agents` or `.vscode` directory, preparation
preserves it with a `.pre-link-backup` suffix before creating the shared link.

Dependency-backed DurinDevTool commands intentionally require `.venv`; they ask
for `DevTool setup` in the main checkout or `DevTool worktree prepare` in a
linked worktree rather than silently using a different system Python.

Machine-specific CMake, build-profile, environment, or job overrides belong in
`.agents/DevTool.user.json`. Normally, leave them empty and let the build driver
detect the Visual Studio environment and parallelism. Setup's preflight honors
both `cmake.command` and `toolchain.environmentScript`; the third-party
bootstrap also uses `cmake.command`, and `CMAKE_COMMAND` still takes precedence
during Setup. The file uses schema version 1; `null` means no path or profile
override, while `build.parallelJobs: "auto"` selects detected parallelism.
The generated file references
`Tools/DurinDevTool/DevTool.user.schema.json` for editor completion and field
documentation.

Tracked DurinDevTool repository structure and command-group enablement live in
`Tools/DurinDevTool/DevTool.json`. Its paths are repository-relative and are
validated to stay inside the checkout. `DevTool.schema.json` beside it documents
the supported fields. Module and project scaffolding assets live under
`Templates/Scaffolding`; the `paths.scaffoldingTemplates` setting selects that
root so the templates remain repository-owned assets independent of the Python
package layout. Do not place machine-local tool paths in this tracked
configuration.

## Windows Workflow

A checkout has one source/build writer at a time. An Agent may own the current checkout; a separate worktree is needed only for concurrent Agents, branches, or human editing/build workflows. An IDE may observe and debug an Agent-owned checkout, but it must not build it.

Use the DurinDevTool worktree commands to create, inspect, open, and remove
linked worktrees:

```powershell
.\DevTool.bat worktree add ..\Durin-feature -b feature-branch
.\DevTool.bat worktree prepare ..\Durin-feature
.\DevTool.bat worktree list
.\DevTool.bat worktree open
.\DevTool.bat worktree remove ..\Durin-feature
```

`worktree open` opens all registered worktrees in Windows Terminal. `add`
creates and prepares a worktree. `prepare` is idempotent and can initialize
manually created worktrees or repair their shared links; omit its path to target
the current checkout, pass `--source` for a non-default prepared worktree, or
pass `--dry-run` to preview the operation. `remove` is the required deletion
path for prepared Windows worktrees because it validates and detaches the shared
`.agents`, `.vscode`, `.venv`, and `Engine/External` directory junctions before
invoking Git. Do not recursively delete a prepared worktree or call
`git worktree remove` directly: a deletion implementation that follows an NTFS
junction can erase the corresponding directory in the main worktree. Use
`remove --dry-run` to inspect the operation, and pass `--force` only to discard
modified or untracked files. `DevTool worktree` without a leaf command uses the
safe `list` default; opening terminals always requires `worktree open`.

Use the root wrapper for configuration, builds, and tests:

```powershell
.\DevTool.bat configure
.\DevTool.bat configure --fresh
.\DevTool.bat build
.\DevTool.bat build --target LevelEditor
.\DevTool.bat run
.\DevTool.bat test --target CoreConcurrencyTests --filter FTaskSchedulerTests.*
.\DevTool.bat test --target all
.\DevTool.bat clean
.\DevTool.bat recover
.\DevTool.bat rebuild --target all
.\DevTool.bat presets
.\DevTool.bat status
.\DevTool.bat path build
.\DevTool.bat path bin
.\DevTool.bat open runtime
.\DevTool.bat open configs
.\DevTool.bat open runtime-logs
.\DevTool.bat open logs # DurinDevTool command logs
.\DevTool.bat stop
```

Run each Python test suite explicitly through pytest:

```powershell
.\.venv\Scripts\python.exe -m pytest Tools\DurinDevTool\tests
.\.venv\Scripts\python.exe -m pytest Engine\Source\Programs\DurinHeaderTool\tests
```

Commands are case-insensitive for compatibility, but lowercase is canonical.
`build` and `test` configure automatically when needed, so an explicit first
`configure` is optional. Omit `--jobs` to use automatic parallelism; pass
`--jobs <count>` only when a local limit is required. From another batch file,
use `call DevTool.bat <arguments>`.

`build` and `rebuild` default to target `all`; native-test executables and their
test-only dependencies are excluded from that default target even when the
selected preset enables `BUILD_TESTING`. `recover` resumes the target recorded
by an interrupted operation; `test` always requires an explicit `--target`,
where `--target all` builds the `DurinNativeTests` aggregate and runs every
CTest-registered test. The interactive shell also accepts the compact
`test all` form. `presets`, `status`, `path`, and `open` are also available
directly, so preset discovery, context inspection, path capture, and artifact
directory access do not require entering the interactive shell.

An ordinary `configure` preserves the existing CMake cache. Pass `--fresh` to discard it explicitly. `rebuild` and automatic recovery from an unusable or incompatible build tree always fresh-configure before building.

DurinDevTool separates its resolved context, execution stages, raw CMake/Ninja output, and final result so failures remain identifiable in long logs. Styled output is enabled for interactive terminals. Pass `--plain`, set `NO_COLOR`, or redirect the output to select stable text-only output without ANSI sequences:

```powershell
.\DevTool.bat build --target all --plain
```

Child-process output has four modes, selected with
`--output auto|compact|progress|full`. The default `auto` mode selects progress
output in an interactive terminal and compact output when stdout is redirected
or consumed by an Agent. Progress mode updates routine Ninja `[n/total]` status
in place and hides DHT DEBUG/INFO lines from the terminal while preserving DHT
warnings, other child output, compiler diagnostics, and the complete command
log. Empty child-output records do not finalize an active Ninja progress line.
Use `--output full` when routine DHT diagnostics must also be streamed.
When explicitly requested without an interactive terminal, progress mode falls
back to compact output.
Compact mode keeps stage boundaries, command lines, heartbeats, and final
results, but suppresses routine CMake, Ninja, and successful GoogleTest lines.
It writes the complete raw output under `Build/.agent-state/logs/`, reports the
log path after each successful child command, and prints a bounded diagnostic
excerpt plus the log path when a child fails. The newest 40 command logs are
retained. Use `--output full` to stream every child-output line, or
`--output compact` to suppress routine child output in an interactive terminal:

```powershell
.\DevTool.bat build --target all --output progress
.\DevTool.bat build --target all --output compact
.\DevTool.bat test --target CoreConcurrencyTests --output full
```

Compact native-test runs also enable GoogleTest's brief output mode. Test
failures and the final test summary remain in the captured output and failure
excerpt; application or library messages are always preserved in the full log.
In styled terminal output, GoogleTest and CTest running, passed, skipped, and
failed statuses are colored consistently even when the child process disables
its own terminal colors.
`--plain` controls styling independently and does not select an output volume.

Agents invoke toolchain-backed commands with `--agent`. This preset selects
plain compact output and emits a short heartbeat every 30 seconds while a
configure, build, clean, or test child command remains alive. An explicit
`--output` value overrides the compact-output part of the preset. Complete raw
child output remains available in the command log, including DHT cache and
generation summaries suppressed by compact mode:

```powershell
.\DevTool.bat build --target all --agent
```

Ordinary human-driven commands do not emit liveness heartbeats. The interactive
`run` command also suppresses them because the runtime is expected to remain
open until the user exits it.

On Windows, the first toolchain-backed command captures and validates the Visual Studio environment. DurinDevTool caches that environment delta under `Build/.agent-state/` so later invocations avoid rerunning `VsDevCmd.bat` and the compiler language probe. The cache refreshes automatically when the setup script, its arguments, or `cl.exe` changes, while caller-provided environment values and `PATH` changes remain live.

The registered Windows build environment defaults to `Win64-Debug-DurinEditor-Tests`, allowing the same output set to run the editor and native tests. Before launching the editor for a smoke test or final validation, build the complete runtime:

```powershell
.\DevTool.bat build --target all
.\DevTool.bat run
.\DevTool.bat run --project Sandbox\Sandbox.dproject
```

`run` launches the existing runtime executable selected by the preset, such as
`DurinEditor.exe` or `DurinGame.exe`; it does not build implicitly. On Windows,
DurinDevTool keeps relaunched runtime descendants in the same tracked process job,
so opening another editor project does not return from `run` or release the
checkout lock until the final editor instance exits.

When the selected runtime is `DurinGame` and no project selector is supplied,
DurinDevTool launches `Sandbox\Sandbox.dproject`. DurinGame does not use recent
project history for this development-time default. Pass `--project` explicitly
to launch another project. DurinEditor and direct executable launches retain
their existing project-selection behavior.

Use `--project <descriptor>` to select an existing `.dproject` explicitly. A
relative descriptor is resolved from the workspace root; an absolute path is
also accepted, including projects outside the workspace. DurinDevTool rejects a
missing file or another extension, normalizes the path to an absolute path, and
passes one `--project=<absolute-path>` argument to the launcher. This validation
does not initialize CMake, Visual Studio, or the compiler toolchain.

Pass other runtime arguments after the final `--args` option. The typed project
argument is always forwarded before them:

```powershell
.\DevTool.bat run --preset Win64-Debug-DurinGame --project Sandbox\Sandbox.dproject --args -ExampleArgument
```

For non-interactive lifecycle smoke tests, pass
`--args --hidden-window --exit-after-ticks=<positive-count>`. The runtime still
completes initialization, executes the requested number of engine ticks, and
uses the normal `FEngineLoop::Exit` path; this option does not impose a
wall-clock timeout or force-terminate the process.

Append `--task-scheduler-lifecycle-smoke` to that argument list when qualifying
the process task scheduler. This diagnostic-only workload starts controlled
short, long, dependent, failed, canceled, waiting, and parallel CPU tasks at
exit, then audits admission close, drain outcomes, and final scheduler
diagnostics before rendering shutdown continues.

Do not repeat `--project` or `--project=...` after `--args` when the typed
`--project` option is present; DurinDevTool rejects the two project selectors as
ambiguous. Raw project selection after `--args` remains accepted for backwards
compatibility when the typed option is absent.

For unattended runtime smoke tests, pass `--hidden-window` to suppress every
native application window, including secondary UI viewports:

```powershell
.\DevTool.bat run --project Sandbox\Sandbox.dproject --args --hidden-window
```

An interactive `DurinDevTool run` can be stopped with Ctrl+C; DurinDevTool terminates
the tracked application job and any relaunched descendants. For a timed Windows
smoke test, launch the runtime with PowerShell `Start-Process`, pass
`--hidden-window`, retain the process returned by `-PassThru`, and stop that
process after verification. `-WindowStyle Hidden` only affects process startup
and is not a substitute for the application argument.

## Asset Audit And Migration

Audit the engine, active project, and configured auto-scan mounts without
starting an editor workspace:

```powershell
.\DevTool.bat build --target DurinAssetTool
.\DevTool.bat asset baseline --project Sandbox\Sandbox.dproject
.\DevTool.bat asset audit --project Sandbox\Sandbox.dproject
.\DevTool.bat asset audit --project Sandbox\Sandbox.dproject --format json
.\DevTool.bat asset audit --project Sandbox\Sandbox.dproject --fail-on incompatible --fail-on unsupported --fail-on error
.\DevTool.bat asset migrate --project Sandbox\Sandbox.dproject
.\DevTool.bat asset migrate --project Sandbox\Sandbox.dproject --mount /Game --format json
.\DevTool.bat asset migrate --project Sandbox\Sandbox.dproject --package /Game/Levels/NewLevel --report Saved\migration-plan.json
.\DevTool.bat asset migrate --project Sandbox\Sandbox.dproject --apply --report Saved\migration-apply.json
```

The default human output groups incompatible, unsupported, failed, and stale
records. `--format json` emits the versioned schema in
`Tools/DurinDevTool/schemas/asset-audit-v1.schema.json`; packages are ordered by
virtual path and enum values use stable names. The three `--fail-on` options are
independent and repeated options combine by logical OR. With no policy option,
incompatible and unsupported packages are reported but do not fail the command.

`asset baseline` is the repository and CI gate. It performs the same read-only
discovery and compatibility probing, then succeeds only when every discovered
authored package is the current DAST v3 format with no schema finding. It
returns `0` for a current baseline, `3` for any older, newer, incompatible,
unsupported, failed, or stale package, `1` for operational failure, and `130`
for cancellation. Run it after changing authored packages and in repository
validation before obsolete format support is removed.

Exit status `0` means the scan and serialization completed and no selected
policy matched. Status `3` means a selected policy matched. Status `1` is an
operational scan/process/schema or command-line validation failure, and
Ctrl+C/cancellation uses status `130`. Audit and migration planning initialize
only project paths, mount definitions, reflected types, and streaming AssetCore
probing. They do not enter Launch, create an editor or renderer, load package
objects or expose a source mutation path.

`asset migrate` is a read-only dry-run. It resolves exact, stable migration
handler edges and reports only fully lossless chains; mount and package filters
are repeatable, and a selected package is blocked if an unselected authored
dependency also requires migration. JSON uses
`Tools/DurinDevTool/schemas/asset-migration-v1.schema.json`, while `--report`
writes the same canonical report to the explicitly selected path. A ready plan
returns `0`, a blocked plan returns `3`, cancellation returns `130`, and invalid
selectors or native/schema failures return an operational or command-line
error.

`asset migrate --apply` is the only writable asset-tool operation. It repeats
the full plan, rejects blocked or stale inputs, constructs every selected
package in isolation, serializes deterministic current-format bytes, and stages
the complete set before publication. Sibling rollback images and atomically
updated transaction manifests cover the multi-file publish window. A failed
publish or verification restores the complete selected set; an interrupted or
incomplete rollback leaves explicit recovery state that the next apply restores
before planning. Success is reported only after loaded objects are released and
a new streaming compatibility audit finds current versions with no findings.
Apply returns `0` on verified success, `3` when policy blocks the complete plan,
`1` after rollback or another operational failure, and `130` when cancellation
is honored before publication begins.

Select another registered configure preset with `--preset`:

```powershell
.\DevTool.bat build --preset Win64-Release-DurinEditor --target all
.\DevTool.bat rebuild --preset Win64-Shipping-DurinGame --target all
.\DevTool.bat build --preset Win64-Release-DurinEditor-Profiling --target all
```

`CMakePresets.json` remains the source of truth for preset configuration.
`Tools/DurinDevTool/DevTool.json` selects that file and the tracked
`AgentBuildProfiles.json` manifest, which controls which presets DurinDevTool may
own for each host environment. The IDE-only
`Win64-Debug-DurinEditor-FastConfigure` preset is intentionally excluded.

FastConfigure is marked code-model-only. Its generated build targets fail before
DHT, compilation, or linking can start; it may only configure for IDE indexing.

## Interactive Shell

Run `DurinDevTool` without arguments, or pass `shell`, to open the human-oriented command shell:

```powershell
.\DevTool.bat
.\DevTool.bat shell
```

Opening the shell loads repository configuration, build profiles, and registered
presets, but does not initialize CMake, Visual Studio, or the compiler toolchain.
The first `configure`, `build`, `clean`, `rebuild`, or `test` command resolves and
validates the toolchain once; later commands and preset switches reuse that
environment for the rest of the session. Read-only and artifact commands remain
available when the compiler toolchain is unavailable.

If Ctrl+C is not forwarded by the terminal or batch wrapper, run `.\DevTool.bat stop` from a second terminal, or enter `stop` in another already-open DurinDevTool shell. It stops the active DurinDevTool process and its complete CMake/Ninja child process tree for this checkout. The foreground shell cannot accept `stop` while it is waiting for its own operation, so stopping that operation requires another process.

The selected preset is session-local and does not modify `.agents/DevTool.user.json`:

```text
DurinDevTool> presets
   1  Win64-Debug-DurinEditor-Tests [default, current]
   2  Win64-Debug-DurinEditor
   3  Win64-Release-DurinEditor
   4  Win64-Release-DurinEditor-Profiling
   5  Win64-Debug-DurinGame
   6  Win64-Release-DurinGame
   7  Win64-Release-DurinGame-Profiling
   8  Win64-Shipping-DurinGame
DurinDevTool> preset 3
DurinDevTool> preset
CMake preset: "Win64-Release-DurinEditor"
DurinDevTool> preset Win64-Debug-DurinGame
DurinDevTool> configure --fresh
DurinDevTool> build
DurinDevTool> recover
DurinDevTool> rebuild --target DurinLauncher
DurinDevTool> test --target CoreConcurrencyTests --filter FTaskSchedulerTests.* --timeout 300
DurinDevTool> run --project Sandbox\Sandbox.dproject --args --hidden-window
DurinDevTool> path runtime
DurinDevTool> open runtime
DurinDevTool> status
DurinDevTool> stop
DurinDevTool> exit
```

`presets` only displays the registered list and immediately returns to the
normal prompt. `preset` without an argument displays the current preset;
`preset <number>` selects the corresponding displayed entry, and `preset
<full-name>` selects a preset by its case-insensitive full name. Direct and
interactive selection resolve the same entry, while only an interactive shell
retains it for later commands. `build` and `rebuild` default to target `all`.
Shell commands accept the same named options as their direct forms. The compact
forms `build <target>`, `rebuild <target>`, `test <target>
[filter]`, and `run [arguments...]` remain accepted for compatibility, while help
shows the canonical named syntax. `run` launches the current preset's existing
runtime executable and returns to the shell when it exits. Its typed
`--project <descriptor>` option follows the same normalization and conflict
rules as the direct command; place it before compact runtime arguments. `path`
prints a registered absolute path and `open` opens the same registered location
in the platform file manager. The deprecated `open-runtime` spelling remains a
hidden compatibility alias for `open runtime` and prints a migration warning.
`status` reports the host build profile, preset, runtime variant, build
directory, configuration, recovery
state, and whether CMake, parallelism, and the toolchain environment are resolved
or still deferred. `stop` stops an operation held by another DurinDevTool process.
Use `help` for the complete command list, `help <command>` for a command, and
`help <group> <command>` for a nested command such as `help worktree add`. A
leading slash remains accepted for compatibility but is not required. A bare
group without a selected safe default displays its group help and returns to the
interactive prompt.

## Repository Locations

`path <location>` writes one absolute native path without a label, so scripts
can capture it directly. `path --all --plain` writes one stable tab-separated
`name<TAB>path` record per canonical location. Location names are
case-insensitive:

| Location | Resolved directory |
| --- | --- |
| `root` | Current repository checkout root |
| `build` | Selected CMake preset's `binaryDir` |
| `binaries` | Configured runtime-binaries root, currently `Engine/Binaries` |
| `output` | Selected platform and configuration below `binaries` |
| `runtime` | Selected runtime variant directory containing its launcher |
| `tests` | Selected runtime variant's native-test `Bin` directory |
| `logs` | DurinDevTool command-log directory below the configured state directory |

`bin` is the documented compact alias for `binaries`. `path` resolves and
prints a registered location even before its directory has been created. `open`
requires an existing directory and otherwise reports the resolved path plus the
command expected to create it. Neither command initializes the compiler
toolchain or acquires the checkout operation lock; only `open` launches an
external process. Arbitrary filesystem paths are intentionally not accepted.

## Documentation Operations

DurinDevTool exposes repository documentation through the `doc` command group.
Ordinary discovery excludes `Documentation/Plans/Archive`; add
`--include-archive` only when historical plans are intentionally part of the
request:

```powershell
.\DevTool.bat doc list --under Documentation\Runtime
.\DevTool.bat doc find "asset package" --kind contract
.\DevTool.bat doc list --kind task
.\DevTool.bat doc refs Documentation\Runtime\Assets\AssetPackages.md
.\DevTool.bat doc validate --scope changed
.\DevTool.bat doc validate --scope all --format json
```

`list`, `find`, `refs`, and `validate` accept terminal, Markdown, or
schema-versioned JSON output. Direct calls default to Markdown and the
interactive shell defaults to terminal output. Validation checks mechanical
repository contracts such as UTF-8 Markdown, top-level titles, local-link
targets, required investigation metadata, and open-task structure. It does not
replace human/Agent review of the document ownership boundaries in
`Documentation/AGENTS.md`.

Create ordinary documents with a type, repository-relative destination, and
title. Move operations repair Markdown links and explicit repository-relative
paths in tracked and untracked Markdown files:

```powershell
.\DevTool.bat doc create contract Documentation\Runtime\Example.md --title "Example"
.\DevTool.bat doc create contract Documentation\Runtime\Example.md --title "Example" --apply
.\DevTool.bat doc move Documentation\Runtime\Old.md Documentation\Runtime\New.md
.\DevTool.bat doc move Documentation\Runtime\Old.md Documentation\Runtime\New.md --apply
```

Both operations are dry runs unless `--apply` is present. Applying verifies
that every previewed source still has the same content, writes atomically,
validates the resulting documentation tree, and rolls back every affected file
if the transaction fails. Task, plan, investigation, and policy creation remains
owned by their nearest authoring rules rather than a generic template.

Open-task commands are nested under `doc task`. The list is derived directly
from task files and uses the first Outcome paragraph as its compact summary; no
separate task index is maintained:

```powershell
.\DevTool.bat doc task list
.\DevTool.bat doc task list --query "HeaderTool" --format json
.\DevTool.bat doc task validate
.\DevTool.bat doc task remove Documentation\Tasks\CompletedTask.md
.\DevTool.bat doc task remove Documentation\Tasks\CompletedTask.md --apply
```

Task removal is a dry run unless `--apply` is present. Applying verifies the
previewed task fingerprint, rejects remaining inbound repository-Markdown
references, deletes the task, validates the resulting documentation tree, and
restores the task if validation fails. Completion and cancellation history
remains in Git; tasks have no status field or archive.

Implementation-plan lifecycle commands are nested under `doc plan`:

```powershell
.\DevTool.bat doc plan list
.\DevTool.bat doc plan validate --scope all
.\DevTool.bat doc plan archive 2026-07
.\DevTool.bat doc plan archive 2026-07 --apply
```

## Creating Modules

Create a workspace module with one DurinDevTool command. The project descriptor
may be relative to the workspace root or absolute:

```powershell
.\DevTool.bat create module Gameplay --project Sandbox\Sandbox.dproject --kind runtime --private-dependency Core --private-dependency Engine
.\DevTool.bat create module SceneEditor --project Sandbox\Sandbox.dproject --kind editor --private-dependency DurinEd
```

Without `--path`, runtime modules are created under
`Source/Runtime/<ModuleName>` and added to `BaseModules`; editor modules are
created under `Source/Editor/<ModuleName>` and added to
`ExtraModules.DurinEditor.Modules`. The directory names are scaffolding
defaults, not build-system classification. Use `--path <ProjectRelativePath>`
to place the module elsewhere inside its owning project:

```powershell
.\DevTool.bat create module Combat --project MyGame\MyGame.dproject --path Source\Game\Combat --private-dependency Core
.\DevTool.bat create module WorldTools --project MyGame\MyGame.dproject --path "Source\Tools\World Tools" --kind editor --private-dependency Core
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
.\DevTool.bat create module Gameplay --project Sandbox\Sandbox.dproject --private-dependency Core --dry-run --plain
```

Creation validates names, workspace-wide dependencies, runtime variants, paths, and
CMake target collisions before writing. During mutation, new files and
directories are tracked and the prior project descriptor is backed up. Any
write or final descriptor-validation failure restores the original bytes and
removes only paths created by that invocation. Repeating a successful request
reports an existing-name or destination conflict instead of overwriting it.
Generated files are rendered from the reviewed templates under
`Templates/Scaffolding/module`.
The same `create module` syntax is available in the interactive DurinDevTool shell.

## Creating Workspace Projects

Create and register a workspace-local project with one command:

```powershell
.\DevTool.bat create project MyGame --path MyGame
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
depends privately on `Core`. DurinDevTool also appends one quoted
`add_subdirectory(...)` registration after the existing workspace project
registrations in the root `CMakeLists.txt`.

Preview the complete operation without changing the workspace:

```powershell
.\DevTool.bat create project MyGame --path MyGame --dry-run --plain
```

Project creation validates containment, project/module/target name collisions,
existing and overlapping destinations, and the root CMake registration before
writing. The project tree and root CMake edit are one transaction: a failure
restores the previous root file byte-for-byte and removes only paths created by
that invocation. Installed-engine projects, external project roots, and nested
workspace project paths are not supported by this command. The same syntax is
available in the interactive DurinDevTool shell. Project-specific files are
rendered from `Templates/Scaffolding/project`, while the initial module reuses
the module templates.

## Clean And Purge

`clean` invokes the CMake clean target for the selected preset. It removes outputs known to that generated build graph, but keeps the configured CMake tree and may leave copied runtime files or generated metadata that CMake does not own.

In particular, clean removes CMake-owned DHT exports, manifests, generated
sources/headers, and stamps, but preserves the versioned per-header cache under
`<Project>/Intermediate/Build/<Platform>/<RuntimeVariant>/DHTCache/`. An
unchanged `rebuild --target all` can therefore reconstruct those outputs from
validated cache entries without invoking libclang. DHT's INFO summaries report
`hits`, `misses`, `materialized`, and `parsed`; an unchanged warm reconstruction
should report zero parses.

`purge` removes the selected preset's configured build and install trees plus its project output and generated-metadata roots:

```powershell
.\DevTool.bat purge --preset Win64-Release-DurinEditor
.\DevTool.bat purge --preset Win64-Release-DurinEditor --yes
```

Inside the interactive shell:

```text
DurinDevTool> purge
DurinDevTool> purge --yes
DurinDevTool> purge --all-presets
```

Purge asks for explicit confirmation unless `--yes` is supplied: enter `PURGE` for the current preset or `PURGE ALL` for the all-presets scope. Use `--all-presets` to remove artifacts for every preset registered to the selected Agent host profile:

```powershell
.\DevTool.bat purge --all-presets
.\DevTool.bat purge --all-presets --yes
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

Because `DHTCache` is inside the registered project runtime-variant intermediate
root, purge removes it. The next DHT generation must therefore report cold
`not-found` misses and reseed the cache. Do not delete individual cache files as
a routine recovery action: invalid or interrupted entries fall back to parsing
and are atomically replaced by an ordinary build rerun.

On non-Windows hosts, invoke
`.venv/bin/python Tools/DurinDevTool/durin_dev_tool/__main__.py <arguments>`
directly after preparing an equivalent virtual environment. Windows callers
must use `DevTool.bat`. DurinDevTool enforces `VSLANG=1033`
after Visual Studio environment setup and verifies that MSVC actually emits
English diagnostics. This keeps CMake's `/showIncludes` dependency prefix
stable for both interactive terminals and Agent output pipes. If validation
reports a localized prefix, add the English language pack through Visual Studio
Installer. The next `configure`, `build`, or `test` refreshes any existing
Ninja tree that does not already contain the English dependency prefix.

## IDE Code Model And Debugging

Use Visual Studio Code or CLion only for the code model, editing, and debugging.
Keep DurinDevTool as the checkout's only build owner. The complete setup for both
editors is documented in `Documentation/Development/Tooling/IDECodeModel.md`.

## Recovery

For Agent-driven `build` and `rebuild` commands, give the shell invocation a timeout of at least 10 minutes and raise it for a full build when prior measurements justify that. If the runner returns a running cell ID, wait on that same cell in intervals no longer than 60 seconds. Do not call `wait` after a final exit result. A runner yield, quiet output, or elapsed UI window alone does not mean that DurinDevTool stopped and must not trigger a second build or recovery-state inspection.

The recovery marker covers only operations that mutate configured or compiled build state. A normal compiler, linker, configuration, or clean failure removes the in-progress marker; fix the reported error and rerun the same command. For `test`, the marker is cleared as soon as its target finishes building, before the test executable starts. Failed assertions, test-process crashes, test timeouts, interrupted tests, and application exits therefore never require a rebuild.

Do not start a second build while an earlier CMake, Ninja, compiler, or linker process tree may still be running. If such a process is cancelled, externally terminated, or loses its controlling DurinDevTool process, wait for the process tree to exit and check `DurinDevTool status`. Only when its recovery state is not `clean`, run the accompanying recovery command. DurinDevTool records a resumable interrupted target as `Recovery state: recover required` and normally reports:

```powershell
.\DevTool.bat recover
```

`recover` reuses the existing CMake/Ninja tree and incrementally builds the recorded target. It does not clean first, so completed object files and unrelated outputs remain available. If configuration is missing or unusable, the normal build path configures it before continuing. The recovery marker is cleared only after the incremental build succeeds; another interruption or an ordinary build failure preserves it so recovery can be retried.

DurinDevTool reports `Recovery state: rebuild required` and `rebuild --target all` instead when the marker is damaged, from an unsupported older format, or otherwise lacks enough information to resume safely. An explicit `rebuild` remains available as the conservative fallback: rebuilding the recorded target or `all` may clear a valid marker, while an unrelated target may not. Because native-test targets are excluded from `all`, an interrupted test-target build must be resumed with `recover` or rebuilt with its recorded target; `rebuild --target all` intentionally does not claim to recover that test build.

When the interrupted operation used a non-default preset, run `status --preset <affected-preset>`, then add `--preset <affected-preset>` to its reported recovery command or select that preset in the interactive shell before recovering.

Use the same recovery after an accidental IDE build. IDE outputs share the final binary directory, and their timestamps can make an incremental Agent build incorrectly report that everything is current. The driver also blocks ordinary `build` and the build phase of `test` for the affected preset after a detected build-state interruption until `recover` or `rebuild` succeeds.

DurinDevTool serializes all registered presets with one checkout-level ownership lock because different CMake trees can still share final outputs and generated metadata. Do not mix direct CMake build operations with `DurinDevTool` ownership of the same checkout.

The checkout lock file normally remains on disk after DurinDevTool exits; the OS byte
lock, not the file's presence or its recorded PID, determines ownership. DurinDevTool
overwrites metadata when it acquires an unowned file and attempts to reset the
file ACL to inherit from `Build/.agent-locks`, so later invocations from another
Agent sandbox identity can reuse it. On Windows, DurinDevTool also attempts to replace an inaccessible stale
file; Windows refuses that replacement while a live DurinDevTool still has the file
open. If the directory ACL itself prevents recovery, the error distinguishes the
permission problem from a live lock and prints `icacls` and `Remove-Item` recovery
commands. Run those commands only after confirming that DurinDevTool, DurinEditor,
CMake, and Ninja have exited for the checkout.

## Output Layout

- Editor: `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`
- Runtime launcher and modules: `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/`
- Runtime configuration and writable state: `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/Saved/Configs/`
- Runtime logs: `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/Saved/Logs/`
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
