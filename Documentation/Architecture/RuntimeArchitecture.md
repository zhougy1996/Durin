# Runtime Architecture

This document explains the startup path, module loading rules, render stack ownership, UI stack ownership, and the runtime validation expectations for Durin changes.

## Boot Flow

Process entry is `Engine/Source/Runtime/Launch/Private/Launch.cpp`.

`main()` drives `FEngineLoop` through:

- `PreInit()`
- `Init()`
- `Tick()`
- `Exit()`

`Launch.cpp` stays thin. Concrete engine selection happens inside `FEngineLoop::Init()`.

## Startup Responsibilities

`FEngineLoop::PreInit()` handles early process setup, DLL search paths, config loading, path mount points, `RenderCore` loading, and reflected object initialization.

`FEngineLoop::Init()` handles common runtime startup, including `ApplicationCore`, `RHI`, `Mona`, `GEngine`, and the rendering thread.

Current engine selection is semantic:

- editor builds construct `DEditorEngine`
- non-editor builds construct `DGameEngine`

Host-specific startup then lives in the concrete engine overrides.

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
release reliable producers. Shutdown drains accepted records in sequence,
wakes producers waiting for capacity or durability, flushes sinks, and then
ends the session history. Calls after shutdown are fallback-only and are not
inserted into retained history.

`FEngineLoop::Tick()` measures and clamps real frame delta time before calling
`DEngine::Tick()`. Active game worlds route that tick through actors and their
tick-enabled components. The runtime lifecycle is:

- component registration and initialization
- `DWorld::BeginPlay()`
- actor and component `BeginPlay()`
- actor and component `Tick()` while enabled
- actor and component `EndPlay()`
- component uninitialization and unregistration

`DGameEngine` loads the project's configured default level and begins play after
creating its window and scene viewport.

## Play In Editor

`DEditorEngine` keeps the persistent editor world separate from a transient PIE
world. Starting Play duplicates only the level's owned object tree; references to
assets outside that tree remain shared. The editor level is detached from the
active scene without being destroyed, the PIE level is registered and begun, and
the viewport falls back to the PIE level's primary camera. Stopping reverses the
transition after draining scene-removal render commands, then marks the complete
transient PIE World hierarchy as garbage through the Outer index. Objects intended
to survive the session must be explicitly reparented before retirement.

PIE supports Playing and Paused states plus single-frame stepping. Runtime changes
are discarded with the transient world and do not dirty the editor level package
unless the user explicitly applies reflected editable values back through the
session's source/runtime object map. Structural ownership and runtime-only objects
are intentionally excluded from Apply.

Play can use the level's primary camera or a transient camera built from the editor
view. Rendering can target the embedded scene viewport or a dedicated Mona window;
the engine retains and restores the editor viewport across the latter session.
During Play, Outliner and Details bind to the runtime world in read-only mode.

`DPhysicsComponent` is the initial runtime physics layer. It integrates linear
velocity and gravity and resolves a horizontal ground plane. `DWorld` owns the
simulation enable flag so pause, single-step, PIE, standalone games, and console
control all share the same lifecycle. This is intentionally a foundation rather
than a general collision backend.

Gameplay code reads the current key, mouse-button, mouse-position, mouse-delta,
and wheel state from `GEngine->GetGameInputState()`. Standalone games receive the
native window input stream; PIE enables that stream only while its embedded scene
viewport is focused.

## Module Loader

The runtime module loader lives in:

- `Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp`
- `Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h`

Behavior summary:

- modules load by logical name
- filenames derive from the active profile name
- filenames follow `<ProfileName>-<ModuleName>.dll`
- shutdown order is reverse load order

When changing cross-module behavior, verify both CMake dependencies and runtime load order expectations.

## Rendering And UI Stacks

Main render layers:

- `RHI`
- `VulkanRHI`
- `RenderCore`
- `Renderer`

The active backend is effectively Vulkan-first today, so `RHI` changes often require matching work in `VulkanRHI` and sometimes `MonaImGui`.

### Scene post-processing and editor assistance

The renderer has two distinct composition domains:

- **Scene post-processing** transforms scene-owned image data. Anti-aliasing, tone mapping, temporal history, and future scene effects belong here.
- **Editor assistance** adds editor-only world visualization after scene post-processing. The world grid, selection and camera overlays, icons, and transform gizmos belong here.

The adopted ordering is:

```text
Opaque scene
  -> scene post-processing and scene anti-aliasing
  -> depth-aware editor assistance
  -> viewport presentation or offscreen sampling
  -> application UI
```

Scene post-processing does not consume editor assistance as source image data or accumulate it into temporal history. The scene pass stores its depth, FXAA or the disabled copy path writes post-processed scene color, and the editor-assistance pass then loads that color together with the preserved depth. Meshes therefore continue to occlude the grid and visible overlay variants, while X-Ray variants remain depth-independent. Assistance pipelines keep depth writes disabled and produce neither motion vectors nor temporal-history input. Each assistance primitive remains responsible for its own local edge treatment, such as derivative-based grid-line antialiasing.

The final assistance pass owns the output transition: window-backed viewports finish in Present, while offscreen viewports finish ShaderReadOnly for Mona composition. Present and offscreen assistance pipelines are stored as output variants of the same renderer responsibility rather than as scene-pass pipelines.

Main UI and windowing layers:

- `ApplicationCore`
- `MonaCore`
- `Mona`
- `MonaImGui`

`MainFrame` owns the editor root window. `DGameEngine` owns the standalone runtime window and scene viewport for non-editor startup.

`MainFrame` also owns the stable application menu structure. The shell keeps File, Edit, Window,
and Help as the compact top-level surface, including application-owned commands such as About.
Registered workspaces may contribute File, Edit, and Window subcommands, but cannot add or replace
top-level menus. Activating a document changes only the target of Save, Undo, and Redo. Workspace-
local actions belong in that editor's toolbar or panels rather than replacing the application menu
bar.

## Validation Expectations

- `RenderCore` has a critical runtime dependency on Slang DLL deployment.
- UI or rendering changes should be validated by building and running `DurinEditor`, not only by compiling.
- Runtime path assumptions and output layout are documented in `Documentation/Setup/BuildAndRun.md`.

## Related Docs

- `Documentation/Architecture/EditorUIStyle.md`
- `Documentation/Architecture/BuildSystem.md`
- `Documentation/Architecture/Profiles.md`
- `Documentation/Plans/MultithreadingV1.md`
- `Documentation/Architecture/ViewportRendering.md`
- `Documentation/Plans/Archive/2026-07/ScenePostProcessEditorAssistanceBoundary.md`
- `Documentation/Setup/BuildAndRun.md`
