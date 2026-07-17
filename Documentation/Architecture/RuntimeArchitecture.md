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
transition after draining scene-removal render commands.

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

Main UI and windowing layers:

- `ApplicationCore`
- `MonaCore`
- `Mona`
- `MonaImGui`

`MainFrame` owns the editor root window. `DGameEngine` owns the standalone runtime window and scene viewport for non-editor startup.

## Validation Expectations

- `RenderCore` has a critical runtime dependency on Slang DLL deployment.
- UI or rendering changes should be validated by building and running `DurinEditor`, not only by compiling.
- Runtime path assumptions and output layout are documented in `Documentation/Setup/BuildAndRun.md`.

## Related Docs

- `Documentation/Architecture/EditorUIStyle.md`
- `Documentation/Architecture/BuildSystem.md`
- `Documentation/Architecture/Profiles.md`
- `Documentation/Architecture/MultithreadingRoadmap.md`
- `Documentation/Architecture/ViewportRendering.md`
- `Documentation/Setup/BuildAndRun.md`
