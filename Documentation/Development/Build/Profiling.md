# CPU Profiling

Durin includes Tracy CPU instrumentation by default in Debug and Release builds
of Editor and Game. Game Shipping builds exclude Tracy at compile and link time.
There is no separate profiling preset or output directory.

## Preparation And Presets

Configure and build automatically prepare the pinned Tracy `v0.13.1` client
source when instrumentation is enabled. The optional host tools remain an
explicit installation:

```powershell
.\DevTool.bat dependency prepare --libs tracy,tracy-tools
```

Use the ordinary Release builds for representative performance captures:

```powershell
.\DevTool.bat build --preset Win64-Release-DurinEditor --target all
.\DevTool.bat build --preset Win64-Release-DurinGame --target all
```

`DURIN_ENABLE_TRACY=AUTO` is the default and enables instrumentation for every
non-Shipping configuration. Advanced builds can explicitly set it to `OFF` for
an uninstrumented baseline or `ON` to enable it. Shipping rejects `ON` during
configuration. Registered presets explicitly select `AUTO`, so reconfiguration
also updates older build trees that cached the former `OFF` default; apply
explicit overrides on the configure command. Client DLLs live alongside the
selected runtime in the ordinary Debug or Release output directory. Host tools
are not required to run an instrumented application.

The client is configured for on-demand, localhost-only capture. Capturing is
optional at runtime. The repository-managed profiler and capture tool always
match the pinned Tracy `v0.13.1` client.

## Editor Tool Workflow

Install the matching Tracy host tools explicitly at
`Engine/External/Packages/tracy-tools/0.13.1/Win64/` with:

```powershell
.\DevTool.bat dependency prepare --libs tracy,tracy-tools
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

An Editor may launch Tracy to inspect itself or a separate development Game.
Both Debug and Release builds support capture by default. The Editor does not claim a
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
run the managed capture tool from the workspace root. Capture names use
`<runtime>-<workload>-<scenario>-<yyyyMMdd-HHmmss>.tracy` so repeated evidence
sorts chronologically and does not silently replace an earlier run:

```powershell
$CaptureDirectory = "Build\Profiling\Tracy"
New-Item -ItemType Directory -Path $CaptureDirectory -Force | Out-Null
$Timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$CapturePath = Join-Path $CaptureDirectory "DurinEditor-task-owner-normal-$Timestamp.tracy"

Engine\External\Packages\tracy-tools\0.13.1\Win64\tracy-capture.exe `
  -a 127.0.0.1 `
  -p <discovered-port> `
  -o $CapturePath `
  -s 10
```

`-s` stops the capture after the specified number of seconds. Omit it for an
interactive capture and stop the tool with Ctrl+C. Keep runtime, workload, and
scenario labels short, stable, lowercase, and free of per-request identity; for
example, `DurinEditor-task-owner-saturated-20260807-172812.tracy`. The timestamp
uses local time and Windows-safe characters. The command intentionally omits
`-f`, so an existing path is an error instead of being overwritten. Add `-f`
only when replacement of one explicitly selected capture is intended. The
capture directory is ignored by Git.

On-demand clients remain available after a capture disconnects. Durin applies a
build-local compatibility fix for Tracy v0.13.1 when call-stack support is
disabled, allowing another profiler or capture process to reconnect without
restarting Editor or Game. The prepared upstream source remains unchanged.

If a connection fails:

1. Confirm the target has Tracy enabled and is still running.
2. Select the port advertised for that exact runtime, project, and PID rather
   than assuming 8086.
3. Remove a shared `TRACY_PORT` override or assign unique fixed ports.
4. Run
   `.\DevTool.bat dependency prepare --libs tracy,tracy-tools`
   if tool status reports a missing or mismatched installation.

## Instrumentation Surface

Repository C++ call sites include `Profiling/Profiling.h`, not Tracy headers.
The supported operations are:

- `DURIN_PROFILE_CPU_ZONE()`
- `DURIN_PROFILE_CPU_ZONE_NAMED("Stable.Name")`
- `DURIN_PROFILE_CPU_ZONE_TEXT(Text)` attaches bounded text to the current zone;
  its argument is evaluated only while that zone is active.
- `DURIN_PROFILE_FRAME_MARK()`
- `DURIN_PROFILE_THREAD(Name)`
- `DURIN_PROFILE_PROGRAM_IDENTITY(RuntimeVariant, ProjectName, ProcessId)`

Zone names use bounded, stable strings that describe an owned operation, such as
`EngineLoop.GameLogic` or `QueuedTask.Execute`. Do not include asset paths,
object names, task ids, or other unbounded per-item data in zone names.

Renderer view zones separate visibility, mesh collection, transform preparation,
input validation, material resolution, sorting, and draw recording. Mesh view
zone text reports candidate and prepared draw counts and the shadow route.
`RDG.Compile` reports declared pass and resource counts, with child zones for
validation, range tracking, hazard dependencies, culling, and execution planning.
`RDG.RecordPass` and `RDG.PassCallback` attach the pass name (up to 128 bytes),
while `RDG.AllocateResources` reports request, reuse, and failure counts.
These annotations describe each invocation, not frame-wide totals; shadow
cascades and multiple views may prepare the same primitive repeatedly. Detailed
mesh zones add profiling overhead while capturing, so compare like-for-like
captures when assessing improvements.

When `DURIN_WITH_TRACY=0`, these macros do not require Tracy headers or symbols
and do not evaluate their profiling-only arguments. Tracy types must not appear
in Durin function signatures, reflected declarations, or public data contracts.
Repository call sites do not invoke `TracySetProgramName` directly. Core's
profiling adapter formats and retains program-name storage because Tracy may
consume the supplied character pointer asynchronously.

The main-thread UI path is split into `EngineLoop.UIFrameBuild`,
`EngineLoop.SceneSubmission`, `EngineLoop.UISubmission`, and
`EngineLoop.RenderSyncWait`. UI frame construction contains
`MonaImGui.PlatformNewFrame`, `MonaImGui.NewFrame`, and `Mona.DrawWindows`;
window drawing includes `LevelEditor.DrawWorkspace`, its SceneViewport,
WorldOutliner and Details zones, and `ContentBrowser.PrepareForDraw` and
`ContentBrowser.HostPresenters`. UI submission separates draw-data finalization,
main-viewport submission, platform-window updates, and platform-window submission
under `MonaImGui.*`. These are CPU elapsed-time zones; frame synchronization may
include downstream backlog. There is no separate UI tick phase.

### Task Correlation And Owner Plots

Profiling builds correlate task phases with the same process-unique nonzero task
id, optional fixed-width numeric scope id, and bounded attribution exposed by
task diagnostics. Unscoped work reports scope id zero. Admission emits an
`enqueue` message after aggregate charging. A winning start opens the stable
`Task.Execute` CPU zone and attaches one zone-text record containing phase, task
id, scope id, owner, category, target, terminal reason, and the bounded task
debug name. The winning terminal transition emits one `terminal` message after
aggregate accounting. The pinned Tracy API has no native cross-thread flow
primitive, so task and scope ids are correlation values; Durin does not retain
task history to draw synthetic arrows.

The engine frame boundary publishes six fixed plots per registered pair:
`QueueDepth`, `Running`, `Rejected`, `PayloadBytes`,
`ResultBytes`, and `RetainedResultBytes`, under
`Tasks.<Owner>.<Category>.<Measurement>`. Plot names are built once from the
bounded attribution registry. Dynamic task names, asset paths, ids, and request
values never create plots or source locations. Scope ids likewise never create
dynamic zones, plots, source locations, or registry slots.

`FEngineLoop::Tick()` explicitly publishes those fixed aggregates immediately
before its CPU frame mark. Task and scheduler diagnostic getters never publish
plots, so observation frequency cannot change profiler output or scheduling
contention.

The adapter has 1,024 fixed slots matching the task attribution bound. With
Tracy disabled its entry points are inline no-ops accepting only fixed-width
values; they perform no label resolution, formatting, allocation, or Tracy
call. Profiler instrumentation runs only after winning state transitions and
outside task-state, scheduler, and executor queue locks. High-frequency task
message formatting and aggregate plot preparation are skipped while disconnected;
execution-zone text is formatted only for an active zone. Attribution registration
remains available for later connections. Capture connection
or failure cannot control scheduling.

## Runtime Ownership

Instrumented builds produce one shared `TracyClient.dll` in the selected runtime
variant directory. Every instrumented module links that same process-wide
runtime. Shipping outputs contain no Tracy runtime.

The initial integration covers CPU frame, thread, task, renderer, and asset
boundaries. GPU, allocation, lock, frame-image, sampling, and call-stack
instrumentation remain outside this workflow.
