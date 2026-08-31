# Build And Run

This is the complete operational guide for configuring, building, testing, and
debugging Durin locally. Agents performing routine task validation should first
use the short [Agent Build And Run Workflow](../../Agents/BuildAndRun.md). For
symptom-first fixes to common environment and command failures, see
[Troubleshooting Setup, Builds, And Runs](Troubleshooting.md).

## Setup

Install the prerequisites for the host, then run the host launcher once in the
main checkout: `.\DevTool.bat setup` on Windows or `./DevTool setup` on macOS.
Create linked worktrees with `DevTool worktree add`, or initialize an existing
linked worktree with `DevTool worktree prepare`.

### Windows prerequisites

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

### macOS prerequisites

The current M1 qualification baseline is Apple Silicon arm64 with macOS 26.6.1,
Xcode 26.6, Apple Clang 21, CMake 4.4.2, Ninja 1.13.2, Python 3.12, Git 2.55,
and LunarG Vulkan SDK 1.4.357. These are the qualified candidate versions, not
yet a declared minimum-version support matrix.

- Install full Xcode and select it with `xcode-select`; Command Line Tools alone
  are not the qualified compiler/SDK environment.
- Install arm64 CMake, Ninja, Python 3.10 or newer with `venv`, and Git. Homebrew
  is acceptable, but DevTool does not install packages or edit shell startup
  files.
- Install the LunarG Vulkan SDK with its Vulkan SDK core and
  KosmicKrisp/MoltenVK components. `VULKAN_SDK` must name the SDK's `macOS`
  directory and contain Vulkan headers, VMA, an arm64 Vulkan loader, and arm64
  MoltenVK libraries. Source the SDK's `setup-env.sh` before setup, or record
  that script as the optional machine-local environment override.
- Do not install Slang separately for Durin. The first configure or build
  downloads and validates the pinned official macOS arm64 Slang package in
  `Engine/External/Packages`.

Durin's macOS setup preflight validates native arm64 execution, selected Xcode,
Apple Clang and SDK discovery, CMake, Ninja, Python, Git, and the Vulkan SDK
layout before mutating repository state. It inherits the caller environment by
default and never installs Homebrew packages, changes `xcode-select`, or edits
shell profiles.

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
Preflight reports all detected prerequisite problems together so an old
toolchain, incomplete Vulkan SDK, or missing command does not first appear
halfway through bootstrap or during the main build. DurinDevTool separately
validates the Visual Studio English language pack when it first initializes
MSVC. Setup
initializes only the main checkout; a linked worktree exits with an error
directing the caller to `DevTool worktree prepare`.
On Windows, the first interactive setup shows automatic CMake and
`VsDevCmd.bat` detection for confirmation and can save validated absolute paths
in `.agents/DevTool.user.json`. On macOS, setup automatically selects the
`macos-xcode-arm64` profile and normally persists an inherited environment;
`toolchain.environmentScript` is needed only when a repeatable local SDK script
override is desired. Use `DevTool setup --non-interactive` for scripts or CI;
it accepts valid automatic or already-configured settings and fails with an
actionable message instead of waiting for input.
Because Setup must install DurinDevTool's Python packages, its terminal styling
uses a standard-library-only fallback until the prepared environment is ready.
`setup --plain` and `NO_COLOR` disable that styling as usual.

In a normal checkout, `DevTool setup` creates `.venv` and installs the pinned
Python dependencies from `requirements.txt` (including the `clang.cindex`
bindings and native `libclang` library). It does not download or compile
repository-managed third-party dependencies. The first command that must
configure a CMake tree prepares the ordinary and test dependencies required by
that preset, and builds shared-install packages only for its effective Debug or
Release configuration. Shipping reuses Release packages. A profiling preset
also prepares the pinned Tracy client source, but never downloads the optional
Tracy host tools.

The confirmed Windows Visual Studio environment or validated macOS inherited
environment is passed to every lazy third-party CMake configure and build
subprocess; explicit `DevTool dependency prepare` calls recreate the saved
selection. Both flows are idempotent and can be rerun after an interrupted
download or build. `DevTool worktree prepare` owns linked-worktree
preparation: it links `.agents`, `.vscode`, `Engine/External`, and `.venv` from
the prepared source worktree before running preflight. Preparation refuses a
linked worktree that already has a non-empty local directory at any shared
path; move or remove that directory explicitly before retrying.

Dependency-backed DurinDevTool commands intentionally require `.venv`; they ask
for `DevTool setup` in the main checkout or `DevTool worktree prepare` in a
linked worktree rather than silently using a different system Python.

Machine-specific CMake, build-profile, environment, or job overrides belong in
`.agents/DevTool.user.json`. Normally, leave them empty and let the build driver
select the host profile, environment, and parallelism. Setup's preflight honors
both `cmake.command` and `toolchain.environmentScript`; the third-party
bootstrap also uses `cmake.command`, and `CMAKE_COMMAND` still takes precedence
during Setup. The file uses schema version 1; `null` means no path or profile
override, while `build.parallelJobs: "auto"` selects detected parallelism.
The generated file references
`Tools/DurinDevTool/DevTool.user.schema.json` for editor completion and field
documentation.

Tracked DurinDevTool repository structure lives in
`Tools/DurinDevTool/DevTool.json`. Its paths are repository-relative and are
validated to stay inside the checkout. `DevTool.schema.json` beside it documents
the supported fields. Module and project scaffolding assets live under
`Templates/Scaffolding`; the `paths.scaffoldingTemplates` setting selects that
root so the templates remain repository-owned assets independent of the Python
package layout. Do not place machine-local tool paths in this tracked
configuration.

## macOS Workflow

Use the extensionless root launcher. The qualified native build target is the
Debug Editor preset on Apple Silicon:

```bash
./DevTool setup
./DevTool status
./DevTool configure
./DevTool build --target all
./DevTool rebuild --target all
```

`setup` is idempotent. Once `.venv` and `.agents/DevTool.user.json` exist, later
commands use `.venv/bin/python` and the saved `macos-xcode-arm64` profile. If
the local configuration names the Vulkan SDK `setup-env.sh`, DevTool evaluates
it for each dependency/configure/build invocation; otherwise the current shell
must already expose the validated Vulkan environment.

The default `MacOS-arm64-Debug-DurinEditor` preset omits all native tests that
require LaunchServices application hosting. Run
`./DevTool configure -DDURIN_ENABLE_APPLICATION_TESTS=ON` in the main or
other designated validation checkout when those tests are required. This
reuses the ordinary build directory; a later `./DevTool configure` reapplies
the preset's explicit `OFF` value. External-volume checkouts may enable the
option after the one-time interactive macOS permission is approved; unattended
validation should prefer an already authorized checkout.

The same LaunchServices boundary applies to `DevTool run`, including
`--hidden-window`: that flag suppresses native windows but does not bypass
macOS application services. Do not run application smoke tests in a Codex
sandbox by default; build the target and report execution as not run. Run one
only when explicitly requested in an already authorized context. A restricted
sandbox may stall before the first tick, where `--exit-after-ticks` cannot help.
A successful hidden run is evidenced by DurinDevTool's zero exit receipt.

The current native baseline qualifies setup, dependency preparation, fresh
configuration, and the complete Debug Editor link closure. The generated
executables and dylibs are arm64, and shared libraries retain `@rpath` install
names. The ordinary Sandbox and Project Browser paths have also passed bounded
hidden-window startup, rendering, and clean-shutdown smoke tests.

This does not yet declare complete macOS product support. Visible Editor-window
input and close behavior, multi-monitor and multi-viewport interaction, the
full MoltenVK rendering and asset compatibility matrices, `.app` bundles,
signing, notarization, and distribution remain under qualification. Track the
current boundary in the [macOS Platform Enablement Roadmap](../../Roadmaps/Archive/2026-08/MacOSPlatformEnablement.md).

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

`worktree open` opens all registered worktrees in a maximized Windows Terminal
window, with up to four equal panes arranged as a stable 2 x 2 grid per tab. `add`
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
.\DevTool.bat test affected
.\DevTool.bat test affected --base origin/main --explain
.\DevTool.bat test CoreConcurrencyTests FTaskSchedulerTests.*
.\DevTool.bat test all
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
`build` and executable `test` selections configure automatically when needed,
so an explicit first
`configure` is optional. Omit `--jobs` to use automatic parallelism; pass
`--jobs <count>` only when a local limit is required. From another batch file,
use `call DevTool.bat <arguments>`.

`build` and `rebuild` default to target `all`; native-test executables and their
test-only dependencies are excluded from that default target even when the
selected preset enables `BUILD_TESTING`. `recover` resumes the target recorded
by an interrupted operation. `test affected` derives a bounded batch from Git
changes, while `test all` builds the `DurinNativeTests` aggregate and runs every
ordinary CTest-registered test. The interactive shell accepts the same forms.
`presets`, `status`, `path`, and `open` are also available
directly, so preset discovery, context inspection, path capture, and artifact
directory access do not require entering the interactive shell.

An ordinary `configure` preserves the existing CMake cache. Pass `--fresh` to
discard it explicitly, or repeat `-DNAME=VALUE` to override CMake cache
values for that configuration. `rebuild` and automatic recovery from an
unusable or incompatible build tree always fresh-configure before building.

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
.\DevTool.bat test CoreConcurrencyTests --output full
```

Compact native-test runs also enable GoogleTest's brief output mode. Test
failures and the final test summary remain in the captured output and failure
excerpt; application or library messages are always preserved in the full log.
In styled terminal output, GoogleTest and CTest running, passed, skipped, and
failed statuses are colored consistently even when the child process disables
its own terminal colors.
`--plain` controls styling independently and does not select an output volume.

On macOS, native targets whose registry metadata reports `host=application`
are admitted through the repository's internal LaunchServices `.app` host.
Use the same `DevTool test` commands as for direct targets; do not assemble a
bundle or invoke `open` manually. The graphical login session must be active,
and terminal or automation sandbox policy must allow LaunchServices application
launch. Application binaries and runtime dependencies remain below the owning
test's output root. Admission, test, crash, timeout, cancellation, and cleanup
failures retain a bounded evidence directory below that test's
`Work/ApplicationHost` directory and print its exact path. Product application
packaging, signing, and installation are unrelated to this internal test
artifact.

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

The registered Windows build environment defaults to
`Win64-Debug-DurinEditor`. Every registered runtime and profiling preset enables
native-test configuration, while test executables remain excluded from the
default `all` build target. The same preset and build tree therefore support
ordinary runtime builds and on-demand `test` commands. Use the corresponding
Release or Shipping runtime preset when configuration-parity qualification is
required; no separate test preset is needed.

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
.\DevTool.bat run --preset Win64-Debug-DurinGame --project Sandbox\Sandbox.dproject --args --hidden-window
```

Launch accepts only its documented option grammar. Unknown options, repeated
flags or scalar options, mixed `--project` spellings, empty values, malformed or
overflowing positive integers, incompatible startup-command modes, and
diagnostic options unavailable in the selected build are rejected before
engine startup. These command-line contract failures print one diagnostic to
stderr and return status `2`; bootstrap/runtime failures return `1`. Only
`--startup-command-arg=<value>` is repeatable. Both `--project=<path>` and
`--project <path>` remain accepted, but a process may supply exactly one of
them.

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

Run `--editor-pie-lifecycle-smoke` with a visible DurinEditor window when
qualifying PIE host restoration and mouse capture. After editor initialization
and default-level activation, the diagnostic exercises embedded and new-window
destinations with both Level Start and Play From Camera. Each combination
starts, pauses, single-steps, stops, and verifies restoration. The diagnostic
requires a real active native window and must not be combined with
`--hidden-window`; use a separate hidden `--exit-after-ticks` run for headless
startup readiness. It is ignored by DurinGame.

Run `--renderer-contact-runtime-smoke` with a visible Debug DurinEditor window
when qualifying the directional-contact compute integration. The diagnostic
drives the main and an independent auxiliary offscreen view through Auto,
Compute, Fragment, disabled, and contribution-diagnostic routes, queues shader
reload and renderer-resource retry, resizes and restores the application
window, observes stable frames, and releases retained viewport/window
references before normal shutdown. Pair it with a bounded
`--exit-after-ticks=<positive-count>` and enable the backend validation layer
for qualification runs. Camera Preview client behavior remains covered by its
native viewport tests; this process diagnostic does not synthesize an editor
selection to activate that UI-only preview.

Append `--native-gameplay-lifecycle-smoke` to either runtime variant to qualify
the generic native session inside a fully initialized process. The diagnostic
temporarily activates an isolated World with one `APlayerStart`, starts the
base `AGameMode`, ticks, pauses and single-steps, restarts the pawn, stops, and
restores the host's original World before normal process exit.

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

## Preset Selection

### Routine Coverage

Do not configure or build every registered preset in every worktree. An
ordinary feature worktree maintains only the selected host profile's default
preset and uses the smallest build target and native-test selection that cover
the change. The current defaults are `Win64-Debug-DurinEditor` on Windows and
`MacOS-arm64-Debug-DurinEditor` on macOS. A user-visible Editor change still
requires the full default Editor `all` build at handoff, as described in the
agent workflow.

Add another preset only when it is registered for the selected host profile and
the changed behavior needs its distinct configuration or runtime graph:

| Change or gate | Additional preset coverage |
| --- | --- |
| Optimized behavior, configuration-dependent code, concurrency, memory layout, undefined-behavior risk, or a merge/release qualification gate | Corresponding Release Editor preset |
| Game startup, cooked runtime behavior, runtime-only modules, or an Editor/Game dependency boundary | Corresponding Debug or Release Game preset |
| Shipping-only macros, logging removal, packaging behavior, or release qualification | Corresponding Shipping Game preset |
| Profiling integration or instrumentation | Corresponding Profiling preset |

A Release Editor preset is therefore a periodic or risk-triggered lane, not a
mandatory companion to every Debug Editor build. Broad runtime and
configuration matrices belong in a designated integration or qualification
checkout after the changes under test coexist. Feature worktrees should not
retain non-default build trees merely because the presets are registered.

### Selecting a Non-Default Preset

Select another registered configure preset explicitly when the default profile
is not the intended target:

```powershell
.\DevTool.bat build --preset Win64-Release-DurinEditor --target all
.\DevTool.bat rebuild --preset Win64-Shipping-DurinGame --target all
.\DevTool.bat build --preset Win64-Release-DurinEditor-Profiling --target all
./DevTool build --preset MacOS-arm64-Release-DurinEditor --target all
```

`CMakePresets.json` remains the configuration source of truth.
`Tools/DurinDevTool/DevTool.json` selects it together with the tracked
`AgentBuildProfiles.json` ownership manifest. The IDE-only
`Win64-Debug-DurinEditor-FastConfigure` preset is excluded from build ownership;
its generated targets reject DHT, compilation, and linking and exist only for
IDE indexing.

## Asset Maintenance

`asset` is the developer-facing entry point for authored package maintenance.
It defaults to the configured game project and to the safe, read-only `check`
command:

```powershell
.\DevTool.bat asset
.\DevTool.bat asset check
.\DevTool.bat asset resave /Game/Characters
.\DevTool.bat asset resave /Game/Characters --apply
.\DevTool.bat asset resave --all --apply
.\DevTool.bat asset storage
```

Pass `--project <descriptor>` only to override the configured default. `check`
never writes and reports schema, canonicalization, and corruption findings.
`resave` accepts one or more virtual scopes, each matching both an exact package
and descendants, or the mutually exclusive `--all`. It is a preview unless
`--apply` is explicit. Human output is the default; `--json` selects stable
machine-readable output. `storage` writes its detailed qualification artifacts
below `Saved/AuthoredPackageStorageQualification`.

`DurinAssetTool` is the lower-level host and uses the same compact grammar:

```text
DurinAssetTool check --project=<project.dproject> [--json]
DurinAssetTool resave --project=<project.dproject> <scope>... [--apply] [--json]
DurinAssetTool resave --project=<project.dproject> --all [--apply] [--json]
DurinAssetTool storage-inventory --project=<project.dproject>
```

Package compatibility semantics are defined by [Asset
Packages](../../Runtime/Assets/AssetPackages.md); the user-facing rewrite
procedure is [Canonical Resave](../../Editor/Guides/CanonicalResave.md).

## DurinDevTool Command Reference

Interactive-shell behavior, repository path discovery, documentation lifecycle
commands, and module/project scaffolding are defined by
[DurinDevTool Command Interface](../Tooling/DurinDevTool.md). Build, run, clean,
recovery, and output ownership remain in this document.

## Clean And Purge

`clean` invokes the CMake clean target for the selected preset. It removes outputs known to that generated build graph, but keeps the configured CMake tree and may leave copied runtime files or generated metadata that CMake does not own.

In particular, clean removes CMake-owned DHT exports, generated sources/headers,
and stamps, but preserves each module's versioned phase state under
`<Project>/Intermediate/Build/<Platform>/<RuntimeVariant>/<Module>/DHTState/`. An
unchanged `rebuild --target all` can therefore reconstruct those outputs from
validated state records without invoking libclang. DHT's INFO summaries report
parsed and rematerialized header counts; an unchanged warm reconstruction should
report zero parses.

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
`Binaries/<Platform>/<CMakeConfig>/ThirdParty/` roots, and project
`Intermediate/Build/<Platform>/<RuntimeVariant>/` roots. It also removes legacy
`Binaries/<Platform>/ThirdParty/<CMakeConfig>/` runtime roots left by the former
output layout. It intentionally preserves bootstrapped dependencies such as
`Build/ThirdParty` and `Engine/External`.

Because each `DHTState` directory is inside its registered module's
runtime-variant intermediate directory, purge removes it. The next DHT
generation must therefore take the cold path and reseed phase state. Do not
delete individual state files as a routine recovery action: invalid or
interrupted records fall back to parsing and are atomically replaced by an
ordinary build rerun.

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
- Immutable runtime configuration template: `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/Templates/TP_<RuntimeVariant>.yaml`
- Runtime configuration and writable state: `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/Saved/Configs/`
- Runtime logs: `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/Saved/Logs/`
- Complete native crash artifacts: `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/Saved/Crashes/<CrashId>/`
- Pre-initialization fallback crash artifacts: `Engine/Binaries/<Platform>/<Config>/Runtime/<RuntimeVariant>/Crashes/<CrashId>/`
- Third-party runtime DLLs: `Engine/Binaries/<Platform>/<Config>/ThirdParty/`
- Native tests: `Engine/Binaries/<Platform>/<Config>/Tests/<RuntimeVariant>/Bin/`

The launcher target is `DurinLauncher`, while the executable name follows the
active runtime variant. Runtime path discovery assumes the executable remains in
this repository-relative layout. If editor startup reports a missing DLL, check
the active runtime directory and the shared configuration-specific `ThirdParty`
directory. On first startup, Launch copies the active immutable template into
`Saved/Configs/`; later builds and launches preserve the writable configuration.

## Native crash analysis

On a recognized Windows native crash, `DevTool run` formats the unsigned status
and stable exception name, then searches only the launched runtime variant,
process id, and launch interval. It reports whether the matching directory is
complete, plus the context, dump, process phase, and faulting thread. Older or
unrelated directories are not attached to the run.

If CDB exists at `DURIN_CDB_PATH` or in the x64 Windows SDK Debuggers directory,
DurinDevTool analyzes the dump after the process exits. Its symbol path starts
with only the exact runtime directory and adjacent PDBs; it does not silently
contact a symbol server. Complete debugger output is stored in the DurinDevTool
command-log directory and a bounded Durin stack excerpt appears in the failure
summary. Missing CDB or a missing dump is non-fatal to context reporting and
prints an equivalent manual command.

Manual offline analysis uses the exact binaries and PDBs from the crashing
build:

```powershell
& $env:DURIN_CDB_PATH -z <Crash.dmp> -y <RuntimeBinaryDirectory> -c ".lines -e; .ecxr; .exr -1; ln @rip; kpn 40; !analyze -v; q"
```

Do not replace adjacent PDBs with a later build. A module-offset-only trace is
not a source-symbolized result. Crash directories are local sensitive data: a
normal minidump can contain stack memory, project paths, identifiers, and
project fragments. Retention keeps 16 complete directories for 30 days and
eligible partial directories for 7 days, and runs only on later healthy startup.

DHT intermediate paths are described in `Documentation/Development/Build/BuildSystem.md` and
`Documentation/Development/Build/RuntimeVariants.md`.
The opt-in Release profiling workflow is documented in
`Documentation/Development/Build/Profiling.md`.

## Related Docs

- `Documentation/Agents/BuildAndRun.md`
- `Documentation/Development/Build/ThirdPartyBootstrap.md`
- `Documentation/Development/Build/NativeTests.md`
- `Documentation/Development/Build/NativeTestAuthoring.md`
- `Documentation/Development/Tooling/IDECodeModel.md`
- `Documentation/Development/Tooling/DurinDevTool.md`
- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/RuntimeVariants.md`
- `Documentation/Development/Build/Profiling.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
