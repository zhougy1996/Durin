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
| `Core` | Platform abstraction, containers, threading, logging, math, modules, serialization primitives, domain-neutral structured diagnostics, and low-level utilities | [source](../../Engine/Source/Runtime/Core) |
| `PhysicsCore` | Engine-independent collision shapes, handles, filters, hits, validation, and reference geometry math | [source](../../Engine/Source/Runtime/PhysicsCore) |
| `Physics` | World-independent physics scene body storage and synchronous query orchestration | [source](../../Engine/Source/Runtime/Physics) |
| `CoreDObject` | Managed objects, reflection, properties, garbage collection, object serialization, format-neutral package linker tables, canonical reflected Map-key tokens, and canonical DAST v9 read/write | [source](../../Engine/Source/Runtime/CoreDObject) |
| `ApplicationCore` | Native application, window, input-message, GLFW, and file-dialog integration | [source](../../Engine/Source/Runtime/ApplicationCore) |
| `AssetRegistry` | Mounted canonical-v9 package discovery, bounded Registry projection, immutable package metadata/dependency snapshots and queries, revisions, and one rebuildable registry cache | [source](../../Engine/Source/Runtime/AssetRegistry) |
| `Engine` | Asset package construction, residency, live graph capture/linker application, transient exact package inspection, cooking, and mutation; plus worlds, actors, components, levels, runtime asset types, editor-only Texture DDC orchestration and completion application, Material/Texture compilation, input, render-facing engine objects, and producer-facing primitive draw submission | [source](../../Engine/Source/Runtime/Engine) |
| `RHI` | Backend-neutral GPU resources, command lists, contexts, feature levels, shader parameters, and RHI-thread contracts | [source](../../Engine/Source/Runtime/RHI) |
| `VulkanRHI` | Vulkan instance/device selection, queues, resources, pipelines, descriptors, swapchains, and backend diagnostics | [source](../../Engine/Source/Runtime/VulkanRHI) |
| `RenderCore` | Rendering thread/resources, Global/Material/mesh-Material registration and typed maps, source-independent Shader request/output values, `DSHD` codec, `DSLB` cooked-library reader, immutable authored/cooked domain selection, stable Vertex Factory descriptors, scene views, simple-element vocabulary, and renderer-module interfaces | [source](../../Engine/Source/Runtime/RenderCore) |
| `Renderer` | Renderer scene representation, accepted Material-result adaptation, bounded exact Material/mesh shader-map and pipeline storage, view preparation, render passes, feature renderers, simple-element collection/GPU execution, and frame execution | [source](../../Engine/Source/Runtime/Renderer) |
| `MonaCore` | Reusable widget, event, UI-backend, and viewport display-source contracts with no application, native-window, or presentation lifetime | [source](../../Engine/Source/Runtime/MonaCore) |
| `Mona` | Mona application lifetime, native windows, RHI-backed window presentation, frame facade, and higher-level viewport widgets | [source](../../Engine/Source/Runtime/Mona) |
| `MonaImGui` | ImGui integration, backend rendering, property tables, widgets, icons, and third-party ImGui boundary | [source](../../Engine/Source/Runtime/MonaImGui) |
| `Launch` | Runtime bootstrap, command-line launch, project selection, application lifetime, and runtime-variant UI-backend composition | [source](../../Engine/Source/Runtime/Launch) |

## Editor Modules

| Module | Primary responsibility | Source root |
| --- | --- | --- |
| `DurinEd` | Shared editor services: generic object factories, reimport handlers, workspaces, reflected property editing, transactions, previews, thumbnails, source references, and editor UI infrastructure | [source](../../Engine/Source/Editor/DurinEd) |
| `AssetTools` | Reusable editor policy and orchestration for package-backed creation, import, duplicate, save/resave, relocation, deletion, and redirector fix-up; built on Engine mechanisms and DurinEd factories/history | [source](../../Engine/Source/Editor/AssetTools) |
| `MainFrame` | Editor host frame, project browser, profiling integration, compatibility tools, and top-level editor startup UI | [source](../../Engine/Source/Editor/MainFrame) |
| `ContentBrowser` | Project-wide browser model, presentation, operations, settings, extensions, and asset thumbnails | [source](../../Engine/Source/Editor/ContentBrowser) |
| `LevelEditor` | Level workspace, scene viewport, panels, documents, selection, Level settings, and editor visualization producers built on Engine primitive drawing | [source](../../Engine/Source/Editor/LevelEditor) |
| `MaterialEditor` | Material asset editor and material-specific editing UI | [source](../../Engine/Source/Editor/MaterialEditor) |
| `TextureEditor` | Texture asset editor, import/build-setting UI, preview, and texture-specific diagnostics | [source](../../Engine/Source/Editor/TextureEditor) |
| `StaticMeshEditor` | Static-mesh inspector, preview, material overrides, and mesh-specific editor tools | [source](../../Engine/Source/Editor/StaticMeshEditor) |
| `AssetForgeBuiltins` | Direct built-in texture, mesh, Terrain, Scene, skeletal, animation, and material import implementations | [source](../../Engine/Source/Editor/AssetForgeBuiltins) |
| `DurinLauncher` | Minimal executable entrypoint for the configured editor or game runtime variant | [source](../../Engine/Source/Editor/DurinLauncher) |

## Developer Modules

Developer modules are target-selected authoring/tool infrastructure. Their
physical root communicates ownership but does not select them for a target.

| Module | Primary responsibility | Source root |
| --- | --- | --- |
| `DerivedDataCache` | Backend-neutral synchronous bucket/key Get/Put cache, concurrent bucket-scoped access, immutable returned bytes, and private local persistence; depends only on `Core` and owns no build orchestration | [source](../../Engine/Source/Developer/DerivedDataCache) |
| `ShaderBuild` | Module-owned live Shader provider: Slang compiler/resolver, dependency manifests, source fingerprints, single-flight workers/LRU, Shader DDC orchestration, generated-source handling, and deterministic cooked-library production; excluded from DurinGame | [source](../../Engine/Source/Developer/ShaderBuild) |
| `TextureBuild` | Pure Texture2D/TextureCube/VolumeTexture normalized-value recipes, panorama normalization, offline compression, recipe metrics and versions, and three typed synchronous providers; no DDC, Build Framework, key, payload codec, live Texture object, PostLoad, scheduler, or result-application authority | [source](../../Engine/Source/Developer/TextureBuild) |
| `StaticMeshBuild` | Pure canonical-geometry reconciliation, render/collision recipes, producer versions, and one bounded typed provider registration; Engine owns keys, caching, PostLoad, and application | [source](../../Engine/Source/Developer/StaticMeshBuild) |
| `SkeletalBuild` | Pure typed SkeletalMesh/AnimationClip canonical-payload recipes and one module-owned provider; Engine owns keys, cache policy, diagnostics, and asset application | [source](../../Engine/Source/Developer/SkeletalBuild) |
| `TerrainBuild` | Pure Heightmap sample and Terrain World composition/product recipes with typed provider registration; Engine owns private keys, cache orchestration, generation application, Cook, manifests, and runtime loads | [source](../../Engine/Source/Developer/TerrainBuild) |
| `AssetMaintenance` | UI-neutral project asset compatibility batches, mounted-package snapshots, deterministic reports, and canonical-v9-resave orchestration; selected by authoring and tool targets but excluded from game Runtime | [source](../../Engine/Source/Developer/AssetMaintenance) |

## Project Modules

Game projects own their own module mapping. For the repository sample project,
[`Sandbox/Sandbox.dproject`](../../Sandbox/Sandbox.dproject) maps the `Sandbox`
gameplay module to [`Sandbox/Source/Runtime/Sandbox`](../../Sandbox/Source/Runtime/Sandbox).

## Common Cross-Module Routes

| Task language | Start with | Expand only when needed |
| --- | --- | --- |
| generic DDC bucket/key get or put | `DerivedDataCache` | Engine owns asset policy and orchestration; pure recipe modules own transformations |
| asset catalog, registry scan/cache, dependency or referencer query | `AssetRegistry` | `Engine` only for loading, mutation, package writing, or Cook |
| package linker tables, serialized type identity, canonical Map-key tokens, DAST v9 codec | `CoreDObject` | `AssetRegistry` for bounded metadata projection; `Engine` only for live graph capture/application |
| asset package, redirector, loading, mutation, cook | `Engine` | `AssetRegistry` for persistent metadata; `CoreDObject` for package/object link identity; editor modules for UI |
| actor, component, level, world, runtime asset type | `Engine` | `CoreDObject` or rendering modules at their owned boundary |
| debug line, point, sprite, primitive draw submission | `Engine` | `RenderCore` for copied value vocabulary; `Renderer` only for collection and GPU execution; `LevelEditor` only for editor-specific producers |
| Global/Material/mesh shader category, typed shader map, render resource, stable Vertex Factory type, scene view | `RenderCore` | `Engine` for accepted Material compiler results; `RHI` for GPU abstraction; `Renderer` for adaptation, bounded exact-set storage, frame use, and explicit generation fan-out |
| authored non-Material Shader compilation, dependency manifests, Shader DDC, Slang, or cooked Shader-library production | `ShaderBuild` | `RenderCore` only for public request/value types and codecs; `Engine` for Cook transaction integration |
| render pass, visibility, draw preparation, renderer scene | `Renderer` | `RenderCore`, `Engine`, then `RHI` |
| Vulkan capability, device, pipeline, descriptor, swapchain | `VulkanRHI` | `RHI` for backend-neutral contract; `Renderer` only for consumer behavior |
| editor workspace, reflected details, thumbnail manager/pool | `DurinEd` | The owning feature editor for concrete renderers; `ContentBrowser` for presentation |
| Content Browser | `ContentBrowser`, `MainFrame`, `DurinEd`, `Engine` | `LevelEditor`, `TextureEditor`, and `StaticMeshEditor` for finite built-in import dispatch; feature modules for scoped create/details/context extensions |
| importing assets | `AssetForgeBuiltins`, `AssetTools`, `DurinEd` | Engine provider contracts for Texture recipes; `StaticMeshBuild`, `SkeletalBuild`, or `TerrainBuild` for their typed recipes; plus `Engine` and the destination runtime asset type |
| local asset DDC request flow for StaticMesh, Texture2D/TextureCube/VolumeTexture, skeletal/animation, or Terrain | `Engine` | Engine owns keys, Get/Put, validation, fallback, and application; Developer build modules supply pure typed recipes |
| project compatibility audit and canonical-resave batch | `AssetMaintenance` | `Engine` for per-package schema/load validation and atomic package mechanisms; `MainFrame` for private Editor task state and presentation; `AssetTools` for editor save policy |

Engine public headers are a repository-owned module contract rather than an
installed external SDK. They must include what they use and resolve through
Engine's declared public dependencies. Native application/window integration
is an Engine implementation dependency; consumers needing `ApplicationCore`
declare it directly. `DMeshComponent` is the reflected superclass for
mesh-backed primitive components and exposes their common indexed material-slot
contract without owning reflected material state. Render-state recreate
contexts are Engine-private implementation details and are not part of the
public surface.

After selecting modules, prefer targeted symbol searches such as:

```powershell
rg -n "FAssetRegistry|ContentBrowser" Engine/Source/Runtime/AssetRegistry Engine/Source/Runtime/Engine Engine/Source/Editor/ContentBrowser
```

Do not read every module descriptor or scan every source root merely to confirm
the mapping.
