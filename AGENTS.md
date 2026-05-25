# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Build and run
- Win64 as example platform; other platforms may have different setup steps.
- Machine-local overrides:
  - If present, read `LOCAL_ENV.md` for machine-specific tool paths or build commands that should not be shared in the repo.
- Initial setup on Windows: `./Setup.bat`
- Configure with CMake presets:
  - Debug: `cmake --preset x64-Debug`
  - Release: `cmake --preset x64-Release`
- Run the editor/launcher after build (Win64):
  - `./Engine/Binaries/Durin/Win64/Debug/Durin.exe`
  - The launcher target is `DurinLauncher`, but its output name is `Durin`.
- Config file: `DurinConfig.yaml` in the output directory.

## Build system shape

- The repo is driven by CMake at the root, which delegates into `Engine/CMakeLists.txt`.
- `CMake/Modules.cmake` is the key file for understanding builds:
  - `durin_add_project(...)` prepares project metadata by invoking the Python-based DurinHeaderTool (`Engine/Source/Programs/DurinHeaderTool/main.py`).
  - `durin_add_module(...)` includes generated per-module CMake, wires reflection/generated sources, sets precompiled headers, and emits shared libraries named `DurinEditor-<ModuleName>`.
- Most engine/editor code is built as modules loaded at runtime through the custom module system, not linked monolithically.
- `RenderCore` has an important post-build step that copies Slang DLLs into `Binaries/ThirdParty/...`; shader/compiler issues often come from this deployment path.

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

- `Core/Modules/ModuleManager.*` implements the engine’s runtime module loader.
- Modules are loaded by name, translated to shared library filenames like `DurinEditor-<Module>.dll`, and initialized through the `IMPLEMENT_MODULE(...)` export path.
- Shutdown order is reverse load order.
- When you add or change cross-module behavior, check both the CMake module dependencies and the runtime `FModuleManager` loading path.

### Rendering stack

- `RHI` is the renderer abstraction layer.
- `RHIGlobals.cpp` currently hardwires the dynamic RHI backend to `VulkanRHI` by loading the `VulkanRHI` module and calling `CreateRHI()`.
- `VulkanRHI` owns the concrete backend: instance creation, physical-device selection, device lifetime, swapchain/viewport resources, command buffers, descriptor sets, pipelines, textures, and deferred deletion.
- `FDynamicRHI::RHIEndFrame_RenderThread()` flushes command submission; Vulkan extends this to advance frame-based deferred deletion.

### UI / windowing stack

- `Mona` and `MonaCore` provide the app, widget, window, and renderer integration layer.
- `FMonaRHIRenderer` bridges Mona windows to `FRHIViewport` objects. Window creation/resizing maps directly to RHI viewport creation and resize calls.
- `MonaImGuiBackend` integrates Dear ImGui on top of Mona + RHI. It snapshots ImGui draw data and submits rendering work to the render thread.
- The current editor shell is module-driven:
  - `MainFrame` creates the root `MWindow` and viewport.

## Important repo-specific behaviors

- This codebase relies on generated build metadata and generated reflection/export files. If a module looks incomplete from static files alone, inspect the generated/intermediate CMake and DHT outputs before assuming the source is missing.
- Shared library naming matters: runtime module loading expects the `DurinEditor-<Module>` naming convention established in `CMake/Modules.cmake`.
- The active rendering backend is effectively Vulkan-first today; changes in `RHI` often need matching updates in `VulkanRHI` and sometimes in the Mona ImGui backend.
- Because the launcher creates a real windowed application, UI/rendering changes should be validated by building and running `Durin`, not only by compiling.
