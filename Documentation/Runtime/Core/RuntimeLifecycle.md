# Runtime Lifecycle

Summary: Define application startup, frame execution, project admission, and shutdown ownership.

Modules: Launch, ApplicationCore, Engine, MonaCore, Mona, MonaImGui

Last reviewed: 2026-08-26

This document defines Durin's process startup, frame entry, lifecycle
integration boundaries, and explicit process-exit ordering.

Engine starts its object-aware asset-compilation aggregate after the task
system. Material compilation is a built-in runtime domain; editor and headless
authoring roots may add module-owned providers such as TextureBuild. Launch
pumps the aggregate while dependent objects are alive and shuts it down before
providers, objects, modules, and tasks are torn down. Detailed aggregation and
provider lifetime are defined by [Asset Compilation](../Assets/AssetCompilation.md),
while payload ownership remains in
[Asset Data Lifecycle and Storage](../Assets/AssetDataLifecycle.md).
`DEngine::PrepareForShutdown()` remains the generic consumer-detachment hook;
Launch does not name an import or build module.

## Boot Flow

Process entry is the minimal C runtime `main()` in
`Engine/Source/Editor/DurinLauncher/Private/Main.cpp`. It delegates directly to
the exported `Durin::RunApplicationProcess()` boundary in Launch.

Before profiling or subsystem startup, the process runner initializes Core's
bounded crash state and installs the platform process-crash owner. It then parses
all arguments into one owned request and validates the complete command-line
contract before waiting, publishing diagnostic state, configuring automation,
or starting the engine. Runtime storage preparation later publishes
`Saved/Crashes` without leaving a partially visible path. A lifetime guard
restores the prior handlers exactly once on every ordinary return. Native fault
ownership, phase values, and artifact safety are defined by
[Native Crash Diagnostics](NativeCrashDiagnostics.md).

`ApplicationRunner.cpp` constructs one private, local `FEngineLoop` and drives
it through:

- `PreInit()`
- `Init()`
- `Tick()`
- `Exit()`

The runner owns process coordination, startup-command dispatch, automated exit,
editor relaunch, conditional logger finalization, and process result selection.
Concrete engine selection remains inside `FEngineLoop::Init()`; no global engine
loop or public engine-loop header exists.

## Startup Responsibilities

`FEngineLoop::PreInit()` handles early engine setup, DLL search paths, config
loading, path mount points, `RenderCore` loading, and reflected object
initialization. Its private runtime-storage component creates Saved directories,
migrates the legacy application configuration and log directory, and returns
the selected app-config path plus pass-local warnings. Feature-owned legacy
settings migrate immediately before their ImGui, MainFrame, LevelEditor, or
ProjectHistory owner loads the new path. `PreInit()` loads the app config before
logger startup and emits the returned warnings only after the logger is ready.

The loop publishes coarse crash phases around pre-initialization, engine
initialization, running, consumer detachment, service and task shutdown, asset
manager shutdown, object collection, module/render/RHI shutdown, and
application shutdown. Typed breadcrumbs distinguish the two shutdown
collections and their adjacent drain boundaries without changing the direct
shutdown protocol.

After worker-scheduler startup, `PreInit()` installs the bounded
`GameThreadDeferred` executor. Failure to install either executor aborts
startup; the engine does not expose a partially initialized task system.

`FEngineLoop::Init()` handles common runtime startup, including
`ApplicationCore`, the platform half of `Mona`, the hidden primary window,
`RHI`, the rendering thread, Mona rendering, and `GEngine`. Launch creates the
final primary `MWindow` and its native window before `RHIInit()`, then passes an
explicit presentation `FRHIInitializationContext` containing the native handle.
Vulkan creates and owns a startup presentation candidate before device
selection. Launch carries one explicit adoption intent into Mona rendering, and
the startup viewport transfers that candidate exactly once; later viewports
create their own surfaces. Headless startup instead uses the explicit headless
context and creates no surface. Render-command admission opens immediately
after `RHIInit()` and before Mona rendering, editor previews, or engine
initialization can enqueue work.

Mona rendering initialization is backend-neutral. In an editor runtime, Launch
then loads MonaImGui and requires it to install the active backend before
`GEngine` initialization can submit UI frames. A missing or failed editor
backend aborts initialization through the ordinary partial-startup unwind. A
game runtime selects no backend: Mona still owns application, window, input,
resize, and RHI presentation, while its UI `NewFrame` and `Render` calls are
explicit no-ops.

Primary-window, platform-surface, and first-viewport ownership is defined by
[Viewport Rendering](../Rendering/ViewportRendering.md). Vulkan instance,
device, queue, portability, and presentation admission are defined by
[RHI Capabilities And Vulkan Startup](../Rendering/RHICapabilitiesAndVulkanStartup.md).

Current engine selection is semantic:

- editor builds construct `DEditorEngine`
- non-editor builds construct `DGameEngine`

Host-specific startup then lives in the concrete engine overrides.

Concrete engine initialization returns an owning
`FEngineInitializationResult` that distinguishes success, user cancellation,
and failure. Launch supplies an `FEngineInitContext` with the primary startup
window, a startup-frame pump, and the headless policy; editor modules do not
depend on Launch internals. Editor and Game adopt that same window rather than
creating a replacement after RHI initialization.
A failed result enters the partial-startup unwind before the normal main loop.
A close request while the editor is initializing is cancellation, uses the same
exact-once unwind, and maps to a clean process result.

`DEngine::Init()` places the default-material service after Engine Content,
Engine, RHI, and render-command admission but before scene proxies can be
created. Material loading, fallback, and render-proxy semantics are defined by
[Material System](../Rendering/MaterialSystem.md).

`DGameEngine` loads the project's `Game` settings and configured default level
after creating its window and scene viewport. It resolves the optional exact
`Game.NativeModule` and fully qualified `Game.GameModeClass` pair, then begins
play with an explicit World request. A missing pair selects lifecycle-only
play. A partial or invalid configured pair, or a native bootstrap failure,
produces an actionable startup error and leaves the World stopped.

`DEditorEngine::Init()` constructs the production MainFrame shell and drives
workspace activation plus default-Level opening to a terminal result before it
returns. The Editor loads `AssetForgeBuiltins` before the base Engine performs
its atomic catalog scan so concrete editor-only import data classes are
registered. Built-in
authoring UI starts with workspace activation. Visible startup first pumps a real Mona/ImGui frame and waits for the
Vulkan presentation path to publish `FirstPresent`; hidden startup skips that
presentation gate. Project Browser initialization completes after its visible
host frame, or immediately after shell construction in headless mode, without
loading project workspaces.

The startup pump processes application events and submits a loading-only frame
through the existing RHI begin/end, Mona render, frame synchronization, and
render-counter protocol. It does not call `DEngine::Tick()`, redraw engine
viewports, pump ordinary deferred or asset-completion budgets, collect garbage,
tick diagnostics or PIE, publish FPS/profiler frames, or increment the logic
frame counter. The editor loading view reports named phase progress; synchronous
workspace and Level operations may keep the last submitted frame static until
the operation returns.

## Frame Lifecycle

`FEngineLoop::Tick()` measures and clamps real frame delta time before calling
`DEngine::Tick()`. Active game worlds receive an `FWorldTickContext` containing
delta time and the Engine-owned raw input snapshot, then route admitted stable
Actor and Component Tick functions through serial PrePhysics, Physics, and
PostPhysics groups. The local player controller
is the only gameplay boundary that translates raw device identities into a
bounded pawn-control intent. Raw one-frame transitions are cleared only after
the World call, so one advancing tick or single-step can observe each edge at
most once.

Immediately after `DEngine::Tick()`, the engine pumps low-priority
`GameThreadDeferred` continuations using the configured item and time budgets.
It then calls the Engine-owned asset-service completion pump before incrementing
the logic-frame counter. Together these are the sole normal-frame GameThread
completion safe point, in this fixed order. Deferred callbacks are not
frame-critical synchronization, do not run inline at submission, and may be
carried into a later frame when the budget is exhausted. Asset-service mailboxes
retain their own admission, payload, frame-budget, explicit-wait, and shutdown
drain policies rather than storing large results in the deferred executor.

Launch keeps the frame render decision visible in `FEngineLoop::Tick()`, while
its private `EngineFrame` component owns begin/end render-thread callbacks, UI
and viewport submission, end-frame synchronization, and the render counter.
The same component has a startup mode that shares those frame mechanics while
submitting only Mona/ImGui. Startup mode is callable only through the narrow
engine-init context and ends before `FEngineLoop` publishes `Running`.
Responsibility-specific diagnostics own editor PIE, native gameplay, and
task-scheduler retained state. Typed native-crash phases are resolved at the
argument boundary rather than compared as arbitrary strings by the loop. These
components do not change
`FEngineLoop::Exit()` ownership of explicit process shutdown ordering.

MonaCore owns only reusable widget, event, backend, and display-source
contracts. Mona owns the concrete application, native windows, window renderer,
and higher-level viewport widgets. Engine exposes MonaCore display-source
contracts publicly and links Mona privately for its concrete application and
window integration; MonaCore and Mona never depend on Engine or MonaImGui.

On Windows, an operating-system window move/resize loop may remain inside the
outer GLFW event pump while dispatching native messages. ApplicationCore
subclasses every GLFW window before later viewport hooks. `WM_MOVING` classifies
a pure move and keeps the window-owned continuation timer disabled, leaving the
UI thread and its native message queue available for pointer-driven position
updates while DWM moves the last presented surface. After each custom-frame
`WM_MOVING` update, ApplicationCore flushes that calling window's queued DWM
changes so visible movement is paced by desktop composition instead of
graphics-driver delivery jitter. Repeated min/max
validation during that move also stays on the native fast path instead of
querying shell app bars. `WM_SIZING` starts a 16 ms continuation timer that
requests Launch work. Launch accepts that non-owning callback only while the
ordinary frame is in its platform-event phase, runs the same game, deferred
work, UI, rendering, garbage-collection, statistics, and profiling body without
polling native events again, and rejects callbacks during that body or during
shutdown. Sizing and final continuations drain render and RHI work because
Windows cannot advance the surface extent until the callback returns. The exit
message stops any sizing timer and requests one final continuation; window
destruction removes the timer and WndProc hook before GLFW destroys the native
handle. Launch clears callback admission before consumer detachment.

The same function owns the CPU-profiler frame mark and stable top-level zones.
Core forwards engine-owned thread names and queued-task execution through the
profiler-neutral surface in `Profiling/Profiling.h`.

World play states, native gameplay roles, level transitions, Actor and
Component dispatch, and lifecycle mutation are defined by
[Level System](../World/LevelSystem.md). Tick groups, same-frame mutation,
cancellation, ordering, and World admission are defined by
[Tick Scheduling](../World/TickScheduling.md). Editor-hosted sessions add the
isolation and restoration rules in
[Play In Editor Architecture](../../Editor/Architecture/PlayInEditorArchitecture.md).

## Logging Integration

`PreInit()` loads application configuration before logger startup and emits
migration warnings only after the logger is ready. The process runner owns
conditional finalization after engine exit. Record ordering, bounded admission,
structured history, sink durability, and shutdown behavior are defined by
[Logging](Logging.md).

## Module Integration

Launch loads modules by logical name using
`<RuntimeVariant>-<ModuleName>.dll`. Cross-module changes must preserve both
CMake dependencies and runtime load-order expectations. Typed feature
invocation, owner-bound asynchronous drain, state transitions, failure
categories, and physical unload are defined by
[Modular Features And Module Retirement](ModularFeaturesAndModuleRetirement.md).
This document owns only the module pass's placement in startup and process exit.

## Rendering And UI Integration

RHI, VulkanRHI, RenderCore, Renderer, ApplicationCore, Mona, and an optional UI
backend participate in the startup and exit order defined here. Launch, rather
than Mona, owns concrete backend selection. Standalone runtime
window ownership belongs to `DGameEngine`; editor host and workspace ownership
belongs to editor systems. Viewport composition is defined by
[Viewport Rendering](../Rendering/ViewportRendering.md), and render/RHI command
recording, replay, flush, and completion are defined by
[RHI Command Execution](../Rendering/RHICommandExecution.md).

## Engine Exit Protocol

`FEngineLoop::Exit()` is the single process-level ordering owner. Launch first
unloads the selected UI backend, which unregisters its exact backend instance
and releases backend state. Mona's module shutdown callback then closes its
windows and renderer, while the stopped Mona module instance remains loaded
until the ordinary post-object-drain module pass. The function expresses the
shutdown order directly:

| Step | Boundary |
| --- | --- |
| Detach render consumers | Unload the selected UI backend, then shut down Mona to destroy windows and viewports and detach world, preview, thumbnail, and scene consumers. |
| Release Engine defaults | After Engine consumer detachment, stop default-material bindings and release the retained asset/proxy before Engine shutdown. |
| Release class defaults | Clear `DClass` ownership derived-first before the first GC; the later module pre-shutdown hooks normally validate an already-empty batch. |
| Stop asset compilation | Close every compile domain, finish accepted object publication in reverse dependency order, and release provider values before Core task admission closes. |
| Stop CPU work | After CPU producers close domain admission and publication, shut down the process [task system](TaskSystem.md) in `Drain` mode. |
| Drain objects | Release roots, run `GC -> render flush -> GC`, and require zero deferred object destruction. |
| Unload modules | Run reverse-order module shutdown only after no deferred object's virtual cleanup can target an unloading module. |
| Close render admission | Enqueue the final RenderCore audit while admission is still open, then atomically close it. |
| Stop rendering | Stop the rendering thread after accepted commands, deferred C++ cleanup, and its final RHI deletion submission drain. The RHI thread remains alive. |
| Stop RHI | Atomically install backend shutdown with RHI admission `Draining`, wait its exact serial, then stop and join the RHI thread. |
| Close the application | Shut down the platform application and publish successful process termination. |

There is no global exit phase or pre-exit callback registry. Each owner closes
its own admission as part of its ordinary shutdown and releases its own
references:

- resource-owning `DObject` instances submit release from `BeginDestroy()` and
  report incomplete local fences through destruction readiness;
- process, subsystem, and module-static owners release through subsystem
  shutdown or their owning module's `ShutdownModule()`;
- consumers retain stable counted RHI references or non-owning snapshots only
  while their component, viewport, scene, or preview owner remains attached.

CPU-work owners close their domain admission and publication before the process
task-system boundary. A task scope is a safety mechanism, not a replacement for
domain mailbox, provider, cache, object, render, or RHI ownership. The
[CPU Task System](TaskSystem.md) defines scope closure, `Drain`/`Cancel`
behavior, continuation dispatch, pumping, waits, and diagnostics; this document
defines only where that process-level boundary occurs.

The shutdown object drain has exactly one release collection, one render flush,
and one finalization collection. It does not retry until an invalid owner
eventually becomes ready. The first collection starts ordinary object-owned
release, the flush completes accepted render work, and the second collection
physically destroys objects whose readiness fence completed. Any remaining
deferred object is reported with its path, class, and lifecycle flags before
module unload is rejected in Debug.

Module shutdown is a separate ownership channel from `DObject` destruction.
Modules unload in reverse load order only after the object channel is empty.
Their callbacks may still submit resource release commands because render
admission remains open until every module callback returns. The final RenderCore
command then validates the registry and deferred cleanup queue; it never sweeps
unknown resources to make shutdown pass.

CoreDObject installs both module-manager lifecycle callbacks during `DObjectInit`:
newly loaded reflected objects finalize their registration and CDO batch before
`StartupModule()`, while pre-shutdown releases that module's batch before
`ShutdownModule()`. Normal process exit releases all CDOs before the first
shutdown GC, so reverse module shutdown observes empty batches. A direct late
module unload performs the same derived-first release and synchronous drain; if
any template remains registered (including deferred destruction), the module
stays ready and the unload is rejected.

## Render And RHI Shutdown Integration

Generic RenderCore resource state, deferred C++ cleanup, producer teardown, and
registry auditing are defined by
[Render Resource Lifecycle](../Rendering/RenderResourceLifecycle.md).
Render-command admission, accepted-work drain, RHI deferred deletion, the
terminal backend marker, and render/RHI thread shutdown are defined by
[RHI Command Execution](../Rendering/RHICommandExecution.md). This document
owns their placement relative to object and module teardown through the exit
protocol above.

## Validation Expectations

- UI or rendering changes require a successful DurinEditor build and runtime
  smoke test, not compilation alone.
- Rendering lifecycle validation must cover command-admission close, accepted
  work drain, resource registry and C++ cleanup drain, RHI deferred deletion,
  rendering-thread stop, and repeated clean process exit.
- Runtime validation covers both the normal threaded mode and the explicit
  `DURIN_RHI_EXECUTION=inline` diagnostic mode. Runtime path assumptions and
  output layout are documented in [Build And Run](../../Development/Build/BuildAndRun.md).

## Related Documentation

- [Logging](Logging.md)
- [Native Crash Diagnostics](NativeCrashDiagnostics.md)
- [CPU Task System](TaskSystem.md)
- [Modular Features And Module Retirement](ModularFeaturesAndModuleRetirement.md)
- [Level System](../World/LevelSystem.md)
- [Viewport Rendering](../Rendering/ViewportRendering.md)
- [Render Resource Lifecycle](../Rendering/RenderResourceLifecycle.md)
- [RHI Command Execution](../Rendering/RHICommandExecution.md)
- [Play In Editor Architecture](../../Editor/Architecture/PlayInEditorArchitecture.md)
- [Build System](../../Development/Build/BuildSystem.md)
- [Runtime Variants](../../Development/Build/RuntimeVariants.md)
- [CPU Profiling](../../Development/Build/Profiling.md)
