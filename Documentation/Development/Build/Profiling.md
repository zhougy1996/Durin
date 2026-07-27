# CPU Profiling

Durin provides opt-in Tracy CPU instrumentation through isolated Release
profiling presets. Ordinary Debug, Release, Shipping, setup, and worktree
workflows do not require or link Tracy.

## Preparation And Presets

`DevTool setup` prepares the pinned Tracy `v0.13.1` source by default. To
prepare or repair Tracy without running the full setup flow:

```powershell
.\Tools\DurinDevTool\DevTool.bat dependency prepare --libs tracy,tracy-tools
```

The supported profiling presets are:

- `Win64-Release-DurinEditor-Profiling`
- `Win64-Release-DurinGame-Profiling`

Build them through DurinDevTool:

```powershell
.\Tools\DurinDevTool\DevTool.bat build --preset Win64-Release-DurinEditor-Profiling --target all
.\Tools\DurinDevTool\DevTool.bat build --preset Win64-Release-DurinGame-Profiling --target all
```

Both use `CMAKE_BUILD_TYPE=Release`, set `DURIN_PRESET_ROLE=Profiling`, enable
`DURIN_ENABLE_TRACY`, and write binaries beneath
`Engine/Binaries/Win64/Release-Profiling/`. Shipping rejects Tracy during
configuration. Their ordinary third-party runtime DLLs are shared with standard
Release builds beneath `Engine/Binaries/Win64/ThirdParty/Release/`; the
profiling-only Tracy runtime remains in the selected profiling runtime directory.

The client is configured for on-demand, localhost-only capture. Capturing is
optional at runtime. The repository-managed profiler and capture tool always
match the pinned Tracy `v0.13.1` client.

## Editor Tool Workflow

Root setup installs the matching Tracy host tools at
`Engine/External/Packages/tracy-tools/0.13.1/Win64/`. Repair both Tracy client
source and host tools with:

```powershell
.\Tools\DurinDevTool\DevTool.bat dependency prepare --libs tracy,tracy-tools
```

After opening a project in DurinEditor, use `Tools > Profiling`:

- `Launch Tracy Profiler` starts the managed official profiler as an independent
  process.
- `Open Tracy Capture...` selects a `.tracy` file and opens it explicitly in the
  matching managed profiler.
- `Open Capture Directory` creates, when needed, and opens the ignored
  workspace-local `Build/Profiling/Tracy/` directory.
- `Tool Status...` reports the expected/client versions, resolved managed path,
  missing required files, and focused repair command.

A normal Editor may launch Tracy to inspect a separate Release Profiling Game.
Capturing the current Editor requires running the
`Win64-Release-DurinEditor-Profiling` output. The Editor does not claim a
connection when the external process launches: Tracy's discovery screen owns
target selection and displays each advertised data port.

If the managed tools are missing, malformed, or version-mismatched, actions that
need the profiler are disabled and show the status reason. A launch failure
reports the resolved executable, readable Windows error, expected version, and
repair command. Once launched, the profiler is not owned by Editor shutdown, so
an interactive profiler with an unsaved capture remains open.

## Process Identity And Ports

Profiling processes publish a Tracy program name in this format:

```text
<RuntimeVariant> | <ProjectName-or-No Project> | PID <ProcessId>
```

For example, two Editors opening Sandbox appear as
`DurinEditor | Sandbox | PID 15584` and
`DurinEditor | Sandbox | PID 10848`. The PID is the final discriminator when
runtime variant and project are otherwise identical. An Editor that selects a
project without relaunching republishes the identity with the selected project.

Durin does not set `TRACY_PORT`. With no developer override, Tracy searches
ports 8086 through 8105 and advertises the selected data port through discovery.
Use the discovered port instead of assuming every process is on 8086. An
explicit `TRACY_PORT` remains a developer-owned override and disables the
automatic search for that process. Profiling runtimes and the Editor tool-status
dialog warn when the variable is present. Two processes that inherit the same
fixed port remain alive, but only the first can listen; remove the override or
give each process a distinct value.

## Command-Line Capture

Use Tracy discovery to identify the target label and its advertised port, then
run the managed capture tool from the workspace root:

```powershell
Engine\External\Packages\tracy-tools\0.13.1\Win64\tracy-capture.exe `
  -a 127.0.0.1 `
  -p <discovered-port> `
  -o Build\Profiling\Tracy\capture.tracy `
  -f `
  -s 10
```

`-s` stops the capture after the specified number of seconds. Omit it for an
interactive capture and stop the tool with Ctrl+C. `-f` permits replacement of
the selected output file. The capture directory is ignored by Git.

On-demand clients remain available after a capture disconnects. Durin applies a
build-local compatibility fix for Tracy v0.13.1 when call-stack support is
disabled, allowing another profiler or capture process to reconnect without
restarting Editor or Game. The prepared upstream source remains unchanged.

If a connection fails:

1. Confirm the target is a Release Profiling build and is still running.
2. Select the port advertised for that exact runtime, project, and PID rather
   than assuming 8086.
3. Remove a shared `TRACY_PORT` override or assign unique fixed ports.
4. Run
   `.\Tools\DurinDevTool\DevTool.bat dependency prepare --libs tracy,tracy-tools`
   if tool status reports a missing or mismatched installation.

## Instrumentation Surface

Repository C++ call sites include `Profiling/Profiling.h`, not Tracy headers.
The supported operations are:

- `DURIN_PROFILE_CPU_ZONE()`
- `DURIN_PROFILE_CPU_ZONE_NAMED("Stable.Name")`
- `DURIN_PROFILE_FRAME_MARK()`
- `DURIN_PROFILE_THREAD(Name)`
- `DURIN_PROFILE_PROGRAM_IDENTITY(RuntimeVariant, ProjectName, ProcessId)`

Zone names use bounded, stable strings that describe an owned operation, such as
`EngineLoop.GameLogic` or `QueuedTask.Execute`. Do not include asset paths,
object names, task ids, or other unbounded per-item data in zone names.

When `DURIN_WITH_TRACY=0`, these macros do not require Tracy headers or symbols
and do not evaluate their profiling-only arguments. Tracy types must not appear
in Durin function signatures, reflected declarations, or public data contracts.
Repository call sites do not invoke `TracySetProgramName` directly. Core's
profiling adapter formats and retains program-name storage because Tracy may
consume the supplied character pointer asynchronously.

## Runtime Ownership

Profiling builds produce one shared `TracyClient.dll` in the selected runtime
variant directory. Every instrumented module links that same process-wide
runtime. Ordinary output directories contain no Tracy runtime.

The initial integration covers CPU frame, thread, task, renderer, and asset
boundaries. GPU, allocation, lock, frame-image, sampling, and call-stack
instrumentation remain outside this workflow.
