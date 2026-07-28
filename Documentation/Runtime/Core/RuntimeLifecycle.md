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
`ApplicationCore`, `RHI`, the rendering thread, `Mona`, and `GEngine`.
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
3. Under the command-pipe mutex, enqueue the final render-resource/RHI audit and
   atomically transition admission from `Running` to `Draining`.
4. Reject new submissions, drain every accepted queued and active command, and
   verify the command pipe, render-resource registry, and deferred C++ cleanup
   queue are empty.
5. Drain deferred RHI deletion, verify its queue is empty, stop the rendering
   thread, transition admission to `Stopped`, and only then call `RHIExit()`.

The final audit reports a live render resource's type, owner, revision,
lifecycle phase, initialization phase, and pending queue. Shutdown does not
clear unexplained entries to make the audit pass; any live registry object,
deferred cleanup owner, accepted command, or pending RHI deletion is a
lifecycle error that must be resolved before `RHIExit()`.

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
