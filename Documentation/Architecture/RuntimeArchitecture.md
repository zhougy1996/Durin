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

The active backend is effectively Vulkan-first today, so `RHI` changes often require matching work in `VulkanRHI` and sometimes `MonaImGuiBackend`.

Main UI and windowing layers:

- `ApplicationCore`
- `MonaCore`
- `Mona`
- `MonaImGuiBackend`

`MainFrame` owns the editor root window. `DGameEngine` owns the standalone runtime window and scene viewport for non-editor startup.

## Validation Expectations

- `RenderCore` has a critical runtime dependency on Slang DLL deployment.
- UI or rendering changes should be validated by building and running `DurinEditor`, not only by compiling.
- Runtime path assumptions and output layout are documented in `Documentation/Setup/BuildAndRun.md`.

## Related Docs

- `Documentation/Architecture/BuildSystem.md`
- `Documentation/Architecture/Profiles.md`
- `Documentation/Architecture/MultithreadingRoadmap.md`
- `Documentation/Architecture/ViewportRendering.md`
- `Documentation/Setup/BuildAndRun.md`
