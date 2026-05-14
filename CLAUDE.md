# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and run

- Initial setup on Windows: `./Setup.bat`
  - This runs `Engine/Scripts/Bootstrap/Bootstrap.bat` to prepare dependencies.
- Configure with CMake presets:
  - Debug: `cmake --preset x64-Debug`
  - Release: `cmake --preset x64-Release`
- Build with CMake:
  - Debug launcher: `cmake --build --preset x64-Debug --target DogeLauncher`
  - Release launcher: `cmake --build --preset x64-Release --target DogeLauncher`
  - Build a single module target: `cmake --build --preset x64-Debug --target VulkanRHI`
- Run the editor/launcher after build:
  - `./Build/Doge/x64/Debug/Doge.exe`
  - The launcher target is `DogeLauncher`, but its output name is `Doge`.
- Config file behavior:
  - Building `DogeLauncher` copies `TP_DogeConfig.yaml` into the output directory as `DogeConfig.yaml` if it is missing.
- There is no verified repo-wide lint command or test harness configured in the top-level CMake files.
  - `CMakePresets.json` defines configure presets only, not build/test presets.
  - I did not find `enable_testing()` / `add_test()` in the main project files.

## Build system shape

- The repo is driven by CMake at the root, which delegates into `Engine/CMakeLists.txt`.
- `CMake/Modules.cmake` is the key file for understanding builds:
  - `doge_add_project(...)` prepares project metadata by invoking the Python-based DogeHeaderTool (`Engine/Source/Programs/DogeHeaderTool/main.py`).
  - `doge_add_module(...)` includes generated per-module CMake, wires reflection/generated sources, sets precompiled headers, and emits shared libraries named `DogeEditor-<ModuleName>`.
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
- Modules are loaded by name, translated to shared library filenames like `DogeEditor-<Module>.dll`, and initialized through the `IMPLEMENT_MODULE(...)` export path.
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
  - `LevelEditor` is loaded as another editor module from `MainFrame`.

### Assets and shaders

- `LaunchEngineLoop.cpp` is also a useful integration sample: it shows the end-to-end path for shader compilation, asset import, render resource creation, pipeline setup, uniform buffer updates, and draw submission.
- Shader compilation goes through `RenderCore`/Slang rather than raw Vulkan-only shader setup.

## Important repo-specific behaviors

- This codebase relies on generated build metadata and generated reflection/export files. If a module looks incomplete from static files alone, inspect the generated/intermediate CMake and DHT outputs before assuming the source is missing.
- Shared library naming matters: runtime module loading expects the `DogeEditor-<Module>` naming convention established in `CMake/Modules.cmake`.
- The active rendering backend is effectively Vulkan-first today; changes in `RHI` often need matching updates in `VulkanRHI` and sometimes in the Mona ImGui backend.
- Because the launcher creates a real windowed application, UI/rendering changes should be validated by building and running `Doge`, not only by compiling.
