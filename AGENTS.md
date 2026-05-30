# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Read first

- For configure, build, run, and binary layout, read `Documentation/Setup/BuildAndRun.md`.
- For third-party bootstrap and dependency deployment, read `Documentation/Setup/ThirdPartyBootstrap.md`.
- For native tests, read `Documentation/Setup/NativeTests.md`.
- For profile semantics and generated metadata flow, read `Documentation/Architecture/Profiles.md`.
- If present, read `LOCAL_ENV.md` for machine-specific tool paths and non-portable command examples.

## Build system shape

- The repo is driven by CMake at the root, which delegates into `Engine/CMakeLists.txt`.
- `CMake/Modules.cmake` is the key file for understanding builds:
  - `durin_add_project(...)` prepares project metadata by invoking the Python-based DurinHeaderTool (`Engine/Source/Programs/DurinHeaderTool/main.py`).
  - `durin_add_module(...)` includes generated per-module CMake, wires reflection/generated sources, sets precompiled headers, and emits shared libraries named `DurinEditor-<ModuleName>`.
  - `durin_add_test_target(...)` is the helper for native test executables.
- Most engine/editor code is built as modules loaded at runtime through the custom module system, not linked monolithically.

## Runtime architecture

### Boot flow

- Process entry is `Engine/Source/Runtime/Launch/Private/Launch.cpp`.
- `main()` drives a custom `FEngineLoop` with the usual sequence: `PreInit() -> Init() -> Tick() until exit -> Exit()`.
- `FEngineLoop::PreInit()` does the early system setup:
  - initializes global/thread state and config loading,
  - enables DLL search paths for third-party runtime binaries,
  - initializes core systems such as names, logging, path mount points,
  - loads `RenderCore`,
  - initializes reflected object processing.

### Module system

- `Core/Modules/ModuleManager.*` implements the engine's runtime module loader.
- Modules are loaded by name, translated to shared library filenames like `DurinEditor-<Module>.dll`, and initialized through the `IMPLEMENT_MODULE(...)` export path.
- Shutdown order is reverse load order.
- When you add or change cross-module behavior, check both the CMake module dependencies and the runtime `FModuleManager` loading path.

### Rendering stack

- `RHI` is the abstract rendering interface, defining the API and resource types for viewports, command buffers, pipelines, textures, etc.
- `VulkanRHI` owns the concrete backend: instance creation, physical-device selection, device lifetime, swapchain/viewport resources, command buffers, descriptor sets, pipelines, textures, and deferred deletion.

### UI / windowing stack

- `Mona` and `MonaCore` provide the app, widget, window, and renderer integration layer.
- `FMonaRHIRenderer` bridges Mona windows to `FRHIViewport` objects. Window creation/resizing maps directly to RHI viewport creation and resize calls.
- `MonaImGuiBackend` integrates Dear ImGui on top of Mona + RHI. It snapshots ImGui draw data and submits rendering work to the render thread.
- The current editor shell is module-driven:
  - `MainFrame` creates the root `MWindow` and viewport.

### Code style and conventions

- C++20 is the baseline language standard.
- Most standard libraries are available and already included in the precompiled header. So do not need to include them in individual files, but can be used freely.

## Important repo-specific behaviors

- This codebase relies on generated build metadata and generated reflection/export files. If a module looks incomplete from static files alone, inspect the generated/intermediate CMake and DHT outputs before assuming the source is missing.
- Shared library naming matters: runtime module loading expects the `DurinEditor-<Module>` naming convention established in `CMake/Modules.cmake`.
- The active rendering backend is effectively Vulkan-first today; changes in `RHI` often need matching updates in `VulkanRHI` and sometimes in the Mona ImGui backend.
- Because the launcher creates a real windowed application, UI/rendering changes should be validated by building and running `DurinEditor`, not only by compiling.
