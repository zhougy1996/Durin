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
`ApplicationCore`, `RHI`, `Mona`, `GEngine`, and the rendering thread.

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

The runtime lifecycle is:

- component registration and initialization
- `DWorld::BeginPlay()`
- actor and component `BeginPlay()`
- actor and component `Tick()` while enabled
- actor and component `EndPlay()`
- component uninitialization and unregistration

Editor-hosted runtime sessions follow the same world lifecycle but add isolation
and restoration rules documented in
`Documentation/Editor/Systems/PlayInEditorArchitecture.md`.

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
- filenames derive from the active profile name
- filenames follow `<ProfileName>-<ModuleName>.dll`
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

## Validation Expectations

- `RenderCore` has a critical runtime dependency on Slang DLL deployment.
- UI or rendering changes require a successful DurinEditor build and runtime
  smoke test, not compilation alone.
- Runtime path assumptions and output layout are documented in
  `Documentation/Development/Build/BuildAndRun.md`.

## Related Documentation

- `Documentation/Development/Build/BuildSystem.md`
- `Documentation/Development/Build/Profiles.md`
- `Documentation/Editor/Systems/PlayInEditorArchitecture.md`
- `Documentation/Editor/Systems/WorkspaceFramework.md`
- `Documentation/Plans/MultithreadingV1.md`
- `Documentation/Runtime/Rendering/ViewportRendering.md`
