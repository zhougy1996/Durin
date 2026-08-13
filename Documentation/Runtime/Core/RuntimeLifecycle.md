# Runtime Lifecycle

Summary: Define application startup, frame execution, project admission, and shutdown ownership.

Modules: Launch, ApplicationCore, Engine

This document defines Durin's process startup, runtime ownership, world tick
lifecycle, logging guarantees, module loading, and validation expectations.

Runtime lifecycle never discovers or pumps asset-authoring capabilities.
Editor and headless authoring roots explicitly select `AssetBuildCore` plus the
required `TextureBuild`/`GeometryBuild` recipe modules, start one generic host
after the task system, pump completions while dependent objects are alive, and
drain it before providers, objects, modules, and tasks are torn down. Recipe
modules contribute through registration and never own the process host.
`DEngine::PrepareForShutdown()` is the generic consumer-detachment hook;
Launch does not name an import or build module, and game products initialize
none of this authoring lifecycle.

TextureCube uncooked loading uses one narrow handler selected by
StandardAssetImport; validation, import, source mutation, and Build invocation
are direct StandardAssetImport/TextureBuild calls and are not registered in
Runtime Engine. Provider shutdown removes the uncooked handler only after the
generic authoring host has drained accepted work.

## Boot Flow

Process entry is the minimal C runtime `main()` in
`Engine/Source/Editor/DurinLauncher/Private/Main.cpp`. It delegates directly to
the exported `Durin::RunApplicationProcess()` boundary in Launch.

Before profiling or subsystem startup, the process runner initializes Core's
bounded crash state and installs the Windows process crash owner. It then parses
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
`ApplicationCore`, `RHI`, the rendering thread, the `Mona` module, and
`GEngine`.
Render-command admission opens immediately after `RHIInit()` and before Mona,
the renderer, editor previews, or engine initialization can enqueue work.

Current engine selection is semantic:

- editor builds construct `DEditorEngine`
- non-editor builds construct `DGameEngine`

Host-specific startup then lives in the concrete engine overrides.

Concrete engine initialization returns an owning
`FEngineInitializationResult` that distinguishes success, user cancellation,
and failure. Launch supplies an `FEngineInitContext` with only a startup-frame
pump and the headless policy; editor modules do not depend on Launch internals.
A failed result enters the partial-startup unwind before the normal main loop.
A close request while the editor is initializing is cancellation, uses the same
exact-once unwind, and maps to a clean process result.

`DEngine::Init()` initializes the default-material service after Engine Content
mounts, AssetCore availability, RHI, and render-command admission, but before
the renderer scene and world can create scene proxies. The service performs one
synchronous game-thread load of `/Engine/Materials/DefaultMaterial`; failure is
non-fatal and leaves the code-constructed ErrorMaterial available without a
second asset lookup.

`DGameEngine` loads the project's `Game` settings and configured default level
after creating its window and scene viewport. It resolves the optional exact
`Game.NativeModule` and fully qualified `Game.GameModeClass` pair, then begins
play with an explicit World request. A missing pair selects lifecycle-only
play. A partial or invalid configured pair, or a native bootstrap failure,
produces an actionable startup error and leaves the World stopped.

`DEditorEngine::Init()` constructs the production MainFrame shell and drives
workspace activation plus default-Level opening to a terminal result before it
returns. Visible startup first pumps a real Mona/ImGui frame and waits for the
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

## Frame And World Lifecycle

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

The same function owns the CPU-profiler frame mark and stable top-level zones.
Core forwards engine-owned thread names and queued-task execution through the
profiler-neutral surface in `Profiling/Profiling.h`.

The runtime lifecycle is:

- component registration and initialization
- `DWorld::BeginPlay(const FWorldPlayRequest&)`
- actor and component `BeginPlay()`
- actor and component `Tick()` while enabled
- actor and component `EndPlay()`
- component uninitialization and unregistration

### World Play State

`DWorld` publishes one authoritative game-thread play state:

```text
Stopped -> BeginningPlay -> Playing -> EndingPlay -> Stopped
```

`HasBegunPlay()` is a compatibility query. It is true while beginning or
playing and false while ending, so EndPlay callbacks cannot create a newly
playing Actor. Spawn is accepted while stopped, beginning, or playing. A Spawn
while beginning or playing dispatches Actor BeginPlay exactly once through the
Spawn path. Spawn is rejected before allocation while the World is ending.
Repeated or recursive World BeginPlay and EndPlay calls are idempotent.

`FWorldPlayRequest` explicitly distinguishes lifecycle-only play from one
native local-player session. A null game-mode class performs only the Actor and
Component lifecycle. A non-null class transactionally creates the World-owned
gameplay roles through the active Level, validates and publishes them before
Actor BeginPlay, and returns a categorized `FWorldPlayResult`. Failure rolls
back every partial runtime Actor and never publishes a playing session.

The World's private gameplay session caches its game mode, local player
controller, default pawn, and runtime-created Actor set; Level membership and
Outer ownership remain authoritative. `RestartPlayer` replaces only the pawn
while preserving the controller. World EndPlay routes the normal reverse Actor
snapshot before destroying the session-created Actors and clearing role state.
Replacing a Level first ends play and detaches any remaining controller/pawn
pair, including Actors that never began play.

The opt-in `--native-gameplay-lifecycle-smoke` process diagnostic exercises
this generic native start/tick/pause-step/restart/stop sequence in an isolated
temporary World after full host initialization, then restores the original
World. Ordinary startup never enables it.

World BeginPlay owns a forward-ordered snapshot of the Actors present at entry;
World EndPlay owns a reverse-ordered snapshot. A callback may Spawn or Destroy
without invalidating the active traversal. Gameplay code requests Level replacement
through the deferred World transition boundary; direct Level activation is limited
to a stopped World so an active lifecycle callback cannot invalidate its caller.
Before every callback, the World verifies the captured Level, structural
membership, retirement state, and Actor play state. A candidate destroyed
before its turn is skipped. A batch stops when its captured Level ceases to be
current.

`SetCurrentLevel` is an immediate stopped-World operation. `RequestLevelTransition`
retains the requested Level and, when applicable, the active native game-mode
class. The next World tick ends the old play session, activates the requested
Level, and resumes play. Only one pending transition is applied per tick, so a
transition requested by EndPlay or BeginPlay remains deferred to a later safe
point. A pending transition suppresses the remainder of the current gameplay
tick.

### Actor And Component Dispatch

Engine-owned code enters Actor lifecycle through non-virtual
`DispatchBeginPlay()` and `RouteEndPlay()`. The virtual `BeginPlay()` and
`EndPlay()` functions remain user extension points; derived implementations
call their base implementation when they want the base Component routing.
Actor state is published before virtual code runs and distinguishes not begun,
beginning, playing, and ending.

Destroy requested during Actor BeginPlay or EndPlay is recorded and completed
after the active callback unwinds. Actor membership remains visible throughout
its EndPlay callback; owner-controlled removal and garbage marking follow it.
Destroying an Actor that is already being destroyed succeeds without repeating
EndPlay, Component teardown, or Level removal. An Actor destroyed before it has
begun play receives no synthetic EndPlay.

Component BeginPlay and EndPlay use equivalent engine-owned dispatch and
destruction states. Actor Component BeginPlay uses a forward snapshot and
EndPlay uses a reverse snapshot. Owner, membership, registration, retirement,
and play state are revalidated before publication. Components added while an
Actor is beginning or playing begin exactly once through the add path.
Components added while the Actor is ending remain registered and owned but do
not begin in that ending lifetime. Self-destruction during Component BeginPlay
or EndPlay completes after the active callback returns and removes owned and
instance membership exactly once.

Actor and Component Tick use stable primary Tick functions registered with the
active Level. Component Tick is independent of Actor Tick enablement, and no
Tick path traverses Actor or Component ownership arrays as an execution list.
Serial groups, same-frame mutation, cancellation, ordering, World admission,
and lifetime rules are defined by
[Tick Scheduling](../World/TickScheduling.md).

Editor-hosted runtime sessions follow the same world lifecycle but add isolation
and restoration rules documented in
`Documentation/Editor/Architecture/PlayInEditorArchitecture.md`.

## Logging Pipeline

`FLogger` owns log ordering, sink delivery, and bounded structured history for
the current process session. Every accepted record receives one monotonically
increasing, nonzero sequence before it enters ordered dispatch. Sequence order,
not timestamp order, is authoritative across concurrent producers. Bootstrap
records share the same sequence domain and are transferred into normal dispatch
without renumbering.

The asynchronous producer queue and retained history are independent bounded
resources. Trace and Debug records may be dropped immediately when the queue is
full; Info and Warn wait only for a bounded interval. Error and Fatal wait for
queue admission while the logger is running. Dropped lower-priority records are
coalesced into an ordered Warn summary when queue capacity becomes available.
History retains accepted records after they reach dispatch, independently of
terminal and file sink thresholds. Oldest-history eviction is normal and cursor
readers receive an explicit gap count instead of a silent discontinuity.

Structured-log consumers use `FLogger::ReadRecords` with the next desired
sequence. Reads return ascending records in a bounded batch, the retained
oldest/newest sequences, the next cursor, and any history-eviction count. Reads
copy owned records while holding only the history lock and never execute
consumer code inside the logger. UI visibility and consumer speed therefore do
not participate in producer admission or sink completion.

The dispatcher owns terminal and file sink writes. Error and Fatal calls return
after active sink attempts and the intentional flush path complete; they never
wait for editor UI work. Sink failures use the fallback stderr path and still
release reliable producers. Shutdown drains accepted records in sequence, wakes
producers waiting for capacity or durability, flushes sinks, and then ends the
session history. Calls after shutdown are fallback-only and are not inserted
into retained history.

## Module Loader

The runtime module loader lives in:

- `Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp`
- `Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h`

Behavior summary:

- modules load by logical name
- filenames derive from the active runtime variant
- filenames follow `<RuntimeVariant>-<ModuleName>.dll`
- shutdown order is reverse load order
- before a ready module's shutdown callback, Core invokes the CoreDObject
  pre-shutdown hook to release and drain class defaults owned by that module's
  `/Cpp/<Module>` package; a failed drain rejects shutdown/unload
- a module may run its shutdown callback early while its instance remains
  available through the object drain; the final module pass releases instances,
  while native libraries remain mapped until process exit

When changing cross-module behavior, verify both CMake dependencies and runtime
load order expectations.

## Rendering And UI Boundaries

The runtime rendering layers are `RHI`, `VulkanRHI`, `RenderCore`, and
`Renderer`. The active backend is Vulkan-first, so `RHI` changes normally
require matching work in `VulkanRHI` and sometimes `MonaImGui`.

Windowing and application UI use `ApplicationCore`, `MonaCore`, `Mona`, and
`MonaImGui`. Standalone runtime window ownership belongs to `DGameEngine`;
editor host and workspace ownership belongs to the editor systems.

Detailed viewport and composition contracts are documented in
`Documentation/Runtime/Rendering/ViewportRendering.md`.
RHI recording, dedicated-thread replay, flush, completion, and batch ownership are
documented in
`Documentation/Runtime/Rendering/RHICommandExecution.md`.

## Engine Exit Protocol

`FEngineLoop::Exit()` is the single process-level ordering owner. Mona's module
shutdown callback runs during consumer detachment to close its windows and UI
backend, while the stopped module instance remains loaded until the ordinary
post-object-drain module pass. The function expresses the shutdown order
directly:

| Step | Boundary |
| --- | --- |
| Detach render consumers | Shut down Mona to destroy windows and viewports and detach world, preview, thumbnail, and scene consumers. |
| Release Engine defaults | After Engine consumer detachment, stop default-material bindings and release the retained asset/proxy before AssetCore shutdown. |
| Release class defaults | Clear `DClass` ownership derived-first before the first GC; the later module pre-shutdown hooks normally validate an already-empty batch. |
| Stop CPU work | Close root task admission, drain workers while explicitly pumping accepted GameThread deferred continuations to graph quiescence, then uninstall the deferred executor. |
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

An owner that launches a descendant CPU graph may hold an `FTaskScope`, but the
scope does not replace the owner's publication or resource protocol. The owner
first rejects new domain requests and closes result publication under its own
lock, then releases that lock, closes the scope in drain or cancel mode, and
waits for quiescence only from a supported thread. It releases mailboxes,
caches, providers, objects, render resources, and RHI references in their
existing domain order after the selected CPU boundary is satisfied. Scope
destruction is never a hidden wait.

AssetImport request scopes retain request-table, provider-lease, latest-wins,
mailbox, and explicit result-taking policy. Source-image thumbnail decode uses
one cache-lifetime scope while upload throttling, serial validation, weak result
publication, and render/RHI command ownership remain cache-owned. Both owners
close publication before canceling and wait without holding their owner or
async-state locks.

The process worker scheduler and GameThread deferred executor start once in
`PreInit()` and stop once here; the normal engine never restarts them. Shutdown
keeps internal continuation dispatch open after root admission closes, because
an accepted worker may still release an accepted GameThread node. The owning
game thread pumps without the frame budget until every accepted graph node is
terminal, then closes and uninstalls the adapter. Task admission, dependencies,
cancellation, waiting, diagnostics, worker helping, and CPU-side ownership
rules are defined in
`Documentation/Runtime/Core/TaskSystem.md`.

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

## Render Resource and Shutdown Ordering

RenderCore command admission is observable as `Stopped`, `Running`, or
`Draining`. Normal initialization moves it to `Running`. `TryEnqueue` reports
whether work was accepted; the compatibility enqueue entry point turns
post-close submission into an immediate actionable check. Producers must stop
submitting before final shutdown begins.

`FRenderResource` owns registry membership and the rendering-thread
initialization, update, and release state machine. Producer-side begin
operations enqueue lifecycle work; render-thread operations assert affinity and
reject invalid double initialization or release. Released concrete C++ storage
is transferred to `FDeferredRenderResourceCleanup`, whose ordered render-thread
flush destroys it only after all earlier commands that could use its non-owning
pointer.

MonaImGui shutdown first flushes every previously accepted draw or upload that
can retain a non-owning pointer into viewport or texture backend state. It then
destroys platform and renderer viewport data, releases backend RHI ownership,
and performs a second flush for the newly queued release work before returning
from module shutdown.

Texture assets uniquely own their stable `FTextureReference` and current
concrete `FTextureResource`. Render consumers retain only counted
`FRHITextureReferenceRef` values. Concrete release detaches the stable target
before dropping its `FTextureRHIRef`; final RHI reference release places the RHI
object in the deferred-delete queue, which is drained by an RHI submit carrying
`DeleteResources`.

Game-thread owners queue resource transitions through
`FRenderResource::BeginInit_GameThread()`,
`BeginUpdateRHI_GameThread()`, and `BeginRelease_GameThread()`. These named
entry points assert the caller thread and preserve command ordering; the actual
`InitResource`, `UpdateRHI`, and `ReleaseResource` work remains rendering-thread
only.

`FEngineLoop::Exit()` uses this order:

1. Stop editor, preview, scene, renderer, asset, and module producers; release
   their consumer snapshots and asset-owned render resources.
2. Flush accepted teardown commands while command admission remains open.
3. Transition command admission from `Running` to `Draining` and reject new
   submissions.
4. Let the rendering thread finish every accepted queued and active command.
5. On the rendering thread, verify the render-resource registry and deferred
   C++ cleanup queue, submit and wait the final RHI deletion drain, transition
   render admission to `Stopped`, and exit.
6. On the game thread, install backend `Shutdown` as the terminal RHI queue
   marker in the same critical section that changes RHI admission to
   `Draining`. Wait that exact serial, switch the executor out of threaded mode,
   stop and join the RHI thread, then release the backend.

The terminal marker is ordered after all accepted RHI work and cannot be
overtaken by a late public producer. Producers blocked on queue capacity wake
when draining begins, retain rejected work, and cannot execute backend mutation
as a fallback. Backend `Init`, normal runtime mutation, and backend `Shutdown`
therefore all observe one explicit RHI ownership phase.

The final audit reports a live render resource's type and, in Debug builds, its
asset owner. Pending cleanup entries also report whether rendering-thread
release has completed. Shutdown does not clear unexplained entries to make the
audit pass; any live registry object, deferred cleanup owner, accepted command,
or pending RHI deletion is a lifecycle error that must be resolved before
`RHIExit()`. Debug builds reject live resources, pending cleanup, late commands,
and residual RHI deletes at the boundary where they occur. Non-Debug builds
execute the same drains and shutdown calls; control-flow side effects must never
be placed inside `check` or `checkf`.

## Validation Expectations

- `RenderCore` has a critical runtime dependency on Slang DLL deployment.
- UI or rendering changes require a successful DurinEditor build and runtime
  smoke test, not compilation alone.
- Rendering lifecycle validation must cover command-admission close, accepted
  work drain, resource registry and C++ cleanup drain, RHI deferred deletion,
  rendering-thread stop, and repeated clean process exit.
- Runtime validation covers both the normal threaded mode and the explicit
  `DURIN_RHI_EXECUTION=inline` diagnostic mode. Runtime path assumptions and
  output layout are documented in
  `Documentation/Development/Build/BuildAndRun.md`.

## Related Documentation

- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/RuntimeVariants.md`
- `Documentation/Development/Build/Profiling.md`
- `Documentation/Editor/Architecture/PlayInEditorArchitecture.md`
- `Documentation/Editor/Architecture/WorkspaceFramework.md`
- `Documentation/Plans/Archive/2026-08/MultithreadingV1.md`
- `Documentation/Runtime/Rendering/ViewportRendering.md`
- `Documentation/Runtime/Rendering/RHICommandExecution.md`
- `Documentation/Runtime/Core/TaskSystem.md`
