# Code Modules

Summary: Route a feature or bug to the smallest likely source-module working set.

Keywords: code module ownership source component subsystem editor runtime

Use this document only when the affected module is not already evident from the
task, symbol, or file path. Select the smallest plausible row set, search those
roots, and expand only when a direct dependency or observed call path crosses a
module boundary.

[`Engine/Engine.dproject`](../../Engine/Engine.dproject) is the authoritative
mapping from module names to directories. Each module's `.dmodule` file is the
authoritative dependency and reflection-input descriptor. This document adds
semantic routing only; it does not redefine build membership or dependency
direction.

## Runtime Modules

| Module | Primary responsibility | Source root |
| --- | --- | --- |
| `Core` | Platform abstraction, containers, threading, logging, math, modules, serialization primitives, and low-level utilities | [source](../../Engine/Source/Runtime/Core) |
| `AetherCore` | Engine-independent collision shapes, handles, filters, hits, validation, and reference geometry math | [source](../../Engine/Source/Runtime/AetherCore) |
| `Aether` | World-independent physics scene body storage and synchronous query orchestration | [source](../../Engine/Source/Runtime/Aether) |
| `CoreDObject` | Managed objects, reflection, properties, garbage collection, and object serialization foundations | [source](../../Engine/Source/Runtime/CoreDObject) |
| `ApplicationCore` | Native application, window, input-message, GLFW, and file-dialog integration | [source](../../Engine/Source/Runtime/ApplicationCore) |
| `AssetCore` | Asset paths, registry, packages, DAST serialization, dependencies, redirectors, derived data, cooking, and mutation transactions | [source](../../Engine/Source/Runtime/AssetCore) |
| `Engine` | World, actors, components, levels, runtime assets, materials, meshes, textures, input, and render-facing engine objects | [source](../../Engine/Source/Runtime/Engine) |
| `RHI` | Backend-neutral GPU resources, command lists, contexts, feature levels, shader parameters, and RHI-thread contracts | [source](../../Engine/Source/Runtime/RHI) |
| `VulkanRHI` | Vulkan instance/device selection, queues, resources, pipelines, descriptors, swapchains, and backend diagnostics | [source](../../Engine/Source/Runtime/VulkanRHI) |
| `RenderCore` | Rendering thread, render resources, shaders, vertex factories, scene views, and renderer-module interfaces | [source](../../Engine/Source/Runtime/RenderCore) |
| `Renderer` | Renderer scene representation, view preparation, render passes, feature renderers, and frame execution | [source](../../Engine/Source/Runtime/Renderer) |
| `MonaCore` | UI application, widgets, events, windows, layout, and renderer abstraction | [source](../../Engine/Source/Runtime/MonaCore) |
| `Mona` | Higher-level Mona runtime and viewport widgets | [source](../../Engine/Source/Runtime/Mona) |
| `MonaImGui` | ImGui integration, backend rendering, property tables, widgets, icons, and third-party ImGui boundary | [source](../../Engine/Source/Runtime/MonaImGui) |
| `Launch` | Runtime bootstrap, command-line launch, project selection, and application lifetime entrypoints | [source](../../Engine/Source/Runtime/Launch) |

## Editor Modules

| Module | Primary responsibility | Source root |
| --- | --- | --- |
| `DurinEd` | Shared editor services: workspaces, reflected property editing, transactions, previews, thumbnails, source references, and editor UI infrastructure | [source](../../Engine/Source/Editor/DurinEd) |
| `MainFrame` | Editor host frame, project browser, profiling integration, compatibility tools, and top-level editor startup UI | [source](../../Engine/Source/Editor/MainFrame) |
| `LevelEditor` | Level-editor workspace, scene viewport, Content Browser presentation, panels, documents, selection, and editor settings | [source](../../Engine/Source/Editor/LevelEditor) |
| `MaterialEditor` | Material asset editor and material-specific editing UI | [source](../../Engine/Source/Editor/MaterialEditor) |
| `TextureEditor` | Texture asset editor, import/build-setting UI, preview, and texture-specific diagnostics | [source](../../Engine/Source/Editor/TextureEditor) |
| `StaticMeshEditor` | Static-mesh inspector, preview, material overrides, and mesh-specific editor tools | [source](../../Engine/Source/Editor/StaticMeshEditor) |
| `AssetImportCore` | Format-neutral asset-import requests, policies, results, and extension interfaces | [source](../../Engine/Source/Editor/AssetImportCore) |
| `StandardAssetImport` | Built-in image, mesh, material, texture, and skeletal import implementations | [source](../../Engine/Source/Editor/StandardAssetImport) |
| `DurinLauncher` | Minimal executable entrypoint for the configured editor or game runtime variant | [source](../../Engine/Source/Editor/DurinLauncher) |

## Developer Modules

Developer modules are target-selected authoring/tool infrastructure. Their
physical root communicates ownership but does not select them for a target.

| Module | Primary responsibility | Source root |
| --- | --- | --- |
| `AssetBuildCore` | Provider-neutral immutable Build definitions/values, explicit policy, local function/request registration, opaque cache access, service contributions, and authoring-host lifecycle | [source](../../Engine/Source/Developer/AssetBuildCore) |
| `TextureBuild` | Texture2D/TextureCube recipes, private offline compression, diagnostics, and asynchronous coordination | [source](../../Engine/Source/Developer/TextureBuild) |
| `GeometryBuild` | StaticMesh/collision, skeletal/animation, and terrain recipes, keys, DDC policy, diagnostics, and Runtime registration adapters | [source](../../Engine/Source/Developer/GeometryBuild) |

## Project Modules

Game projects own their own module mapping. For the repository sample project,
[`Sandbox/Sandbox.dproject`](../../Sandbox/Sandbox.dproject) maps the `Sandbox`
gameplay module to [`Sandbox/Source/Runtime/Sandbox`](../../Sandbox/Source/Runtime/Sandbox).

## Common Cross-Module Routes

| Task language | Start with | Expand only when needed |
| --- | --- | --- |
| asset package, registry, redirector, DDC, cook | `AssetCore` | `Engine` for asset-type policy; editor modules for UI |
| actor, component, level, world, runtime asset type | `Engine` | `CoreDObject`, `AssetCore`, or rendering modules at their owned boundary |
| shader, render resource, vertex factory, scene view | `RenderCore` | `RHI` for GPU abstraction; `Renderer` for frame use |
| render pass, visibility, draw preparation, renderer scene | `Renderer` | `RenderCore`, `Engine`, then `RHI` |
| Vulkan capability, device, pipeline, descriptor, swapchain | `VulkanRHI` | `RHI` for backend-neutral contract; `Renderer` only for consumer behavior |
| editor workspace, reflected details, thumbnail service | `DurinEd` | The owning feature editor or `LevelEditor` |
| Content Browser | `LevelEditor`, `DurinEd`, `AssetCore` | Asset-type editor/import modules for extensions |
| importing assets | `AssetImportCore`, `StandardAssetImport` | `AssetBuildCore` for generic mechanics, `TextureBuild` or `GeometryBuild` for typed recipes, plus `AssetCore` and the destination runtime asset type |

After selecting modules, prefer targeted symbol searches such as:

```powershell
rg -n "FAssetRegistry|ContentBrowser" Engine/Source/Runtime/AssetCore Engine/Source/Editor/LevelEditor
```

Do not read every module descriptor or scan every source root merely to confirm
the mapping.
