# Runtime Lifecycle

This document defines Durin's process startup, runtime ownership, world tick
lifecycle, logging guarantees, module loading, and validation expectations.

## Boot Flow

Process entry is `Engine/Source/Runtime/Launch/Private/Launch.cpp`.

`main()` drives `FEngineLoop` through:

- `PreInit()`
- `Init()`
- `Tick()`
- `Exit()`

`Launch.cpp` stays thin. Concrete engine selection happens inside
`FEngineLoop::Init()`.

## Startup Responsibilities

`FEngineLoop::PreInit()` handles early process setup, DLL search paths, config
loading, path mount points, `RenderCore` loading, and reflected object
initialization.

`FEngineLoop::Init()` handles common runtime startup, including
`ApplicationCore`, `RHI`, the rendering thread, the `Mona` module, and
`GEngine`.
Render-command admission opens immediately after `RHIInit()` and before Mona,
the renderer, editor previews, or engine initialization can enqueue work.

Current engine selection is semantic:

- editor builds construct `DEditorEngine`
- non-editor builds construct `DGameEngine`

Host-specific startup then lives in the concrete engine overrides.

`DGameEngine` loads the project's configured default level and begins play after
creating its window and scene viewport.

## Frame And World Lifecycle

`FEngineLoop::Tick()` measures and clamps real frame delta time before calling
`DEngine::Tick()`. Active game worlds route that tick through actors and their
tick-enabled components.

The same function owns the CPU-profiler frame mark and stable top-level zones.
Core forwards engine-owned thread names and queued-task execution through the
profiler-neutral surface in `Profiling/Profiling.h`.

The runtime lifecycle is:

- component registration and initialization
- `DWorld::BeginPlay()`
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

World BeginPlay owns a forward-ordered snapshot of the Actors present at entry;
World EndPlay owns a reverse-ordered snapshot. A callback may Spawn, Destroy,
clear, or replace the current Level without invalidating the active traversal.
Before every callback, the World verifies the captured Level, structural
membership, retirement state, and Actor play state. A candidate destroyed
before its turn is skipped. A batch stops when its captured Level ceases to be
current.

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

Actor-owned Component Tick remains a direct Actor traversal. Mutation safety,
registration, ordering, dependencies, and scheduling for that high-frequency
path are intentionally deferred to a dedicated Tick scheduling contract.

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

## Engine Exit Protocol

`FEngineLoop::Exit()` is the single process-level ordering owner. Mona's module
shutdown callback runs during consumer detachment to close its windows and UI
backend, while the stopped module instance remains loaded until the ordinary
post-object-drain module pass. The function expresses the shutdown order
directly:

| Step | Boundary |
| --- | --- |
| Detach render consumers | Shut down Mona to destroy windows and viewports and detach world, preview, thumbnail, and scene consumers. |
| Stop CPU work | Stop accepting thread-pool work and drain accepted tasks. |
| Drain objects | Release roots, run `GC -> render flush -> GC`, and require zero deferred object destruction. |
| Unload modules | Run reverse-order module shutdown only after no deferred object's virtual cleanup can target an unloading module. |
| Close render admission | Enqueue the final RenderCore audit while admission is still open, then atomically close it. |
| Stop rendering | Stop the rendering thread after accepted commands, deferred C++ cleanup, and RHI deletion drain. |
| Close the application | Run `RHIExit()`, shut down the platform application, and publish successful process termination. |

There is no global exit phase or pre-exit callback registry. Each owner closes
its own admission as part of its ordinary shutdown and releases its own
references:

- resource-owning `DObject` instances submit release from `BeginDestroy()` and
  report incomplete local fences through destruction readiness;
- process, subsystem, and module-static owners release through subsystem
  shutdown or their owning module's `ShutdownModule()`;
- consumers retain stable counted RHI references or non-owning snapshots only
  while their component, viewport, scene, or preview owner remains attached.

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
   C++ cleanup queue, drain deferred RHI deletion, transition admission to
   `Stopped`, and exit. The game thread waits for completion before `RHIExit()`.

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
- Runtime path assumptions and output layout are documented in
  `Documentation/Development/Build/BuildAndRun.md`.

## Related Documentation

- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/RuntimeVariants.md`
- `Documentation/Development/Build/Profiling.md`
- `Documentation/Editor/Architecture/PlayInEditorArchitecture.md`
- `Documentation/Editor/Architecture/WorkspaceFramework.md`
- `Documentation/Plans/MultithreadingV1.md`
- `Documentation/Runtime/Rendering/ViewportRendering.md`
