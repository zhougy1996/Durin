# Runtime Architecture

This document summarizes the main runtime boot path and subsystem relationships in Durin.

## Boot Flow

Process entry is:

- `Engine/Source/Runtime/Launch/Private/Launch.cpp`

`main()` drives a custom `FEngineLoop` with the usual sequence:

- `PreInit()`
- `Init()`
- `Tick()` until exit
- `Exit()`

`Launch.cpp` is intentionally thin. It enters the runtime through `GEngineLoop`, and the concrete engine type is selected inside `FEngineLoop::Init()`.

## `FEngineLoop::PreInit()`

Early startup is responsible for:

- initializing global and thread state
- enabling DLL search paths for runtime third-party binaries
- loading the application config from the launcher output directory
- initializing names, logging, and path mount points
- loading `RenderCore`
- initializing reflected object processing

Relevant implementation:

- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`

## `FEngineLoop::Init()`

Common runtime startup is owned by `FEngineLoop::Init()`:

- selecting the concrete engine implementation for the active build
- initializing `ApplicationCore`
- initializing `RHI`
- initializing `Mona`, which installs the default developer-tools UI backend when enabled
- initializing `Mona`
- constructing and initializing `GEngine`
- starting the rendering thread

Current engine selection uses semantic compile-time branching:

- editor builds construct `DEditorEngine`
- non-editor builds construct `DGameEngine`

Host-specific startup then lives in the concrete engine `Init()` overrides:

- `DEditorEngine::Init()` loads `MainFrame` and creates the editor root window
- `DGameEngine::Init()` creates the standalone game window and binds the main scene viewport

Relevant implementation:

- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/GameEngine.cpp`

## Module System

The runtime module loader is implemented in:

- `Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp`
- `Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h`

Behavior summary:

- modules are loaded by logical name
- filenames are derived from the active profile name
- filenames follow the `<ProfileName>-<ModuleName>.dll` pattern
- modules are initialized through the `IMPLEMENT_MODULE(...)` export path
- shutdown order is reverse load order

When changing cross-module behavior, check both:

- CMake module dependencies
- runtime module loading order and expectations

## Rendering Stack

Main layers:

- `RHI`: abstract rendering interface
- `VulkanRHI`: concrete Vulkan backend
- `RenderCore`: rendering support layer above RHI
- `Renderer`: higher-level renderer systems

`VulkanRHI` currently owns:

- Vulkan instance creation
- physical-device selection
- device lifetime
- swapchain and viewport resources
- command buffers
- descriptor sets
- pipelines
- textures
- deferred deletion

The active rendering backend is effectively Vulkan-first today. Changes in `RHI` often require matching updates in `VulkanRHI` and sometimes in `MonaImGuiBackend`.

## UI And Windowing Stack

Main layers:

- `ApplicationCore`
- `MonaCore`
- `Mona`
- `MonaImGuiBackend`

Responsibilities:

- `Mona` and `MonaCore` provide the app, widget, window, and renderer integration layer
- `FMonaRHIRenderer` bridges Mona windows to `FRHIViewport` objects
- `Mona` owns the active UI backend lifecycle and frame dispatch
- `MonaImGuiBackend` integrates Dear ImGui on top of MonaCore and RHI when developer tools are enabled
- editor/tool modules that call Dear ImGui directly should explicitly depend on `MonaImGuiBackend`
- `MainFrame` creates the root `MWindow` and viewport for the editor shell
- `DGameEngine` creates the standalone runtime `MWindow` and scene viewport for non-editor startup

Window creation and resize events map directly to RHI viewport creation and resize calls.

## Runtime-Specific Notes

- `RenderCore` has a critical runtime dependency on Slang DLL deployment.
- The launcher creates a real windowed application, so UI and rendering changes should be validated by building and running `DurinEditor`, not only by compiling.
- Runtime path discovery assumes the executable stays in the repository-relative output layout documented in `Documentation/Setup/BuildAndRun.md`.

## Code Style Notes

- C++20 is the baseline language standard.
- Most standard library headers are already available through the shared precompiled header, so they usually do not need to be included again in individual source files.

## Related Docs

- Build system structure: `Documentation/Architecture/BuildSystem.md`
- Profile system: `Documentation/Architecture/Profiles.md`
- Build and run workflow: `Documentation/Setup/BuildAndRun.md`
