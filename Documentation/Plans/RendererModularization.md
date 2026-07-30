# Renderer Modularization Plan

Summary: Replace Renderer module monoliths and anonymous global feature state with composed module-private renderer types whose files, ownership, lifecycle, and responsibilities align.

Last reviewed: 2026-07-30

Status: Active
Completed:

## Current Status

Stage 0 is complete from clean implementation baseline
`3092da1f58bd89126be49db2bc9a00d3c2673af9`. No source refactoring has begun.
The frozen owner inventory below assigns every current persistent state object,
creation slot, cache, callback, shader, helper, proxy traversal, and release
action to one Stage 1 through 6 destination. The target filenames and public
rename consumer list have no unresolved ownership decisions.

The adjacent-plan handoffs were reconciled against the current Git history:

- Static Mesh Render-Data Lifecycle Stage 3 recorded `7f9b6c3a`, while the
  current lineage contains the integrated implementation as
  `f92c06ff833bfb3677d10e082a7539f749fa381b`
  (`feat(staticmesh): establish render-data ownership lifecycle`).
- Static Mesh LOD Resources Refactor recorded the pre-rewrite Stage 1 baseline
  `2f7363d0`, which is not present in the current object database. Its current
  Stage 0 contract and Stage 1 named-buffer implementation are
  `9d0e228b4f7fa1c39d6ec7083b879a57b42ad8a3` and
  `04bb1a4c95b0950e0c96ac19dd59d48a47f2598e`.
- Material Render-Proxy Invalidation recorded the pre-rewrite Stage 2 baseline
  `610e43d2`, which is not present in the current object database. The
  corresponding current-lineage implementation is
  `551cbfe130d94ff82376bcbba6dc0377883ff902`
  (`feat(material): bind scene proxies to material proxies`).

Stage 1 is next. Its bounded working set is the existing default-texture,
resource-invalidation, fullscreen-geometry, and render-target-layout sources;
their focused tests; the Renderer module lifecycle adapter; the new
`Private/Resources/` owners; and this plan. It does not include feature shader,
draw, scene-proxy, StaticMesh render-data, or material render-proxy changes.

The current Renderer module has two dominant implementation units:
`RendererModule.cpp` combines module lifecycle, default textures, renderer
resource invalidation, scene orchestration, static-mesh rendering, skybox
rendering, TextureCube thumbnail rendering, and post-processing, while
`RendererEditorAssistanceRenderer.cpp` combines Grid, Gizmo, Overlay Line, and
Overlay Icon resource creation, geometry preparation, pipeline selection, and
drawing. Most persistent feature state is stored in anonymous-namespace
globals rather than in an object whose lifetime expresses ownership.

The refactoring must consume the established handoffs from the active
Static Mesh Render-Data Lifecycle, Static Mesh LOD Resources Refactor, and
Material Render-Proxy Invalidation plans. In particular,
`FStaticMeshRenderer` extraction must not redefine asset render-data
ownership, vertex-factory ownership, or material render-proxy invalidation.

### Stage 0 Frozen Baseline and Handoff

#### Current owner inventory

| Current state or behavior | Frozen destination |
| --- | --- |
| `GRendererResourceGeneration`, `GForceRecompileShaderGeneration`, `GRendererResourceInvalidationController`, controller shared admission state and command handles | `FRendererResourceCoordinator` |
| `renderer.reload-shaders changed/all`, `renderer.retry-resources`, internal device request, `QueueRequest`, `Start`, `Stop`, `Request`, `EnqueueRendererResourceInvalidation`, `ApplyRendererResourceInvalidation_RenderThread`, and generation snapshots | `FRendererResourceCoordinator`; `FSceneRenderer` performs explicit concrete-owner fan-out |
| `GDefaultTextures`, solid 2D/cube creation, render-thread initialization, default accessors, and texture-reference fallback resolution | `FDefaultTextureResources`; public accessors remain forwarding functions |
| `RendererFullscreenGeometry::GState`, device creation slot, fullscreen vertex/index buffers, lazy ensure/get, retry, and release | `FFullscreenGeometryResources` |
| `RendererRenderTargetLayouts` output enum, color/depth attachment builders, Scene Color layout, intermediate/final post-process layout, and assistance load layout | `Resources/RenderTargetLayouts.*` as pure helpers |
| StaticMesh shader classes, transform/lighting/material uniforms, base creation slot, material shader-map cache, material pipeline cache, diagnostic and identity formatting, compile options, resource creation, section draw, and StaticMesh proxy traversal | `FStaticMeshRenderer` |
| TextureCube thumbnail shader classes, transform uniform, creation slot and copied payload fields, sampler/declaration/pipeline creation, draw submission, and preview-proxy traversal | `FTextureCubeThumbnailRenderer`; copied payload fields collapse to the committed payload |
| SkyBox shader classes, creation slot and copied payload fields, sampler/declaration/pipeline/index resources, resource creation, validation, uniform preparation, and draw submission | `FSkyBoxRenderer`; `SkyBoxRendering::BuildUniform` and its finite/matrix helpers become owner methods or remain a narrow deterministic helper beside it |
| Post-process shader classes, FXAA uniform, creation slot and copied payload fields, copy/FXAA pipeline variants, sampler, size-keyed Scene Color/depth map, target creation, and post-process draw | `FPostProcessRenderer`; copied payload fields collapse to the committed payload |
| View/output rejection, output-kind derivation, assistance demand, target resolution, constrained viewport fitting, render-pass construction, pass ordering, viewport/scissor restoration, and final output transition selection | `FSceneRenderer` |
| `RendererEditorAssistance::AnalyzeRequest`, required-key calculation, drawable-operation filtering, immutable `FPrepared`, draw-order table, aggregate preparation, and aggregate draw dispatch | `FEditorAssistanceRenderer` |
| Gizmo shaders, uniforms, mesh ranges, base creation slot and copied payload fields, cylinder/cone/box/wire-box/plane/ring generation, pipeline identities, prepare, and draw | `FGizmoRenderer`; copied payload fields collapse to the committed payload |
| Overlay Line shaders, style/vertex types, base creation slot and copied payload fields, dynamic buffers and capacities, clipped quad geometry, upload, pipeline identities, prepare, and draw | `FOverlayLineRenderer`; copied payload fields collapse to the committed payload |
| Overlay Icon shaders, style/vertex types, base creation slot and copied payload fields, atlas/sampler, dynamic buffers and capacities, atlas construction, quad geometry, upload, pipeline identities, prepare, and draw | `FOverlayIconRenderer`; copied payload fields collapse to the committed payload |
| Editor Grid shaders, base creation slot and copied payload fields, fullscreen geometry use, pipeline identities, uniform validation/preparation, and draw | `FEditorGridRenderer`; deterministic `EditorGridRendering::BuildUniform` remains independently testable either as an owner static method or a narrow adjacent helper |
| Editor-assistance generation, force-recompile selection, feature/output/depth/topology identity formatting, diagnostic formatting, base/pipeline generation lookup, shared pipeline vector, `EnsurePipeline`, `AddPreparedPipeline`, invalidation, retry, and release | Generation and force selection move to `FRendererResourceCoordinator`; exact-key pipeline slots, diagnostics, invalidation, retry, and release move to each concrete assistance renderer; aggregate prepared-key collection stays in `FEditorAssistanceRenderer` |
| `FScene` primitive, light, and SkyBox mutation queues, render-thread containers, revision checks, release command, and render-thread queries | Rename atomically to `FRenderScene` in Stage 6 without changing ownership or behavior |
| `FRendererModule` command startup, default-resource startup, scene construction, resource release enqueue, shutdown flush, `RenderView`, and `RenderScene` | Startup/shutdown and scene construction remain in `FRendererModule`; rendering and concrete resource fan-out delegate to its owned `FSceneRenderer` |

Every `TRenderResourceCreationSlot` therefore has one selected owner: StaticMesh
base resources, each StaticMesh material shader map and pipeline identity,
TextureCube thumbnail, SkyBox, Post Process, shared fullscreen geometry, four
editor-assistance base resources, and each exact assistance pipeline identity.
The two current size/key containers likewise have one owner:
`FPostProcessRenderer::SceneTargetsBySize` and the per-feature assistance
pipeline caches. No creation slot or keyed cache remains module-global.

#### Frozen lifecycle and frame behavior

- `InitializeDefaultTextures_RenderThread`, default texture access, resource
  invalidation application and snapshot access retain explicit render-thread
  assertions. `FRenderScene` render-command bodies and SkyBox queries retain
  `CheckRenderingThread`; scene creation retains its game-thread assertion.
  Extracted feature entry points add or retain render-thread assertions rather
  than relying only on their caller.
- Startup registers both development commands before enqueuing default texture
  initialization, and skips GPU texture creation when no RHI exists.
- Shutdown closes command admission and unregisters commands, enqueues release
  of default textures, feature payloads, generations, assistance resources,
  Post Process resources, and fullscreen geometry, flushes rendering commands,
  and only then permits owner destruction. Each extracted release remains
  idempotent.
- Shader invalidation advances only the shader generation, records forced
  recompilation only for `all`, and retains last-known-good payloads. Manual
  retry advances only the manual generation. Device invalidation first clears
  device-dependent payloads, advances only the device generation, then
  recreates demanded startup defaults; feature reconstruction remains lazy.
- A null or zero-sized output returns before demand or resource creation.
  Scene Color/depth resolution precedes constrained viewport fitting. Scene
  drawing is SkyBox, StaticMesh, then TextureCube preview. Assistance prepares
  after the scene pass. Copy/FXAA writes the output, followed only when
  drawable by a preserved-color/depth assistance pass ordered Grid, X-Ray
  Gizmo/Line/Icon, then visible Gizmo/Line/Icon. The last pass alone selects
  Present or ShaderReadOnly output.

#### Frozen public rename consumers

Stage 6 atomically renames the interface declaration in
`Engine/Source/Runtime/Engine/Public/IScene.h`, the concrete
`Renderer/Public/Scene.h` and `Renderer/Private/Scene.cpp`, and the module
contract in `RenderCore/Public/IRendererModule.h` and
`Renderer/Public/RendererModule.h`. Direct runtime consumers are Engine
forward declarations and ownership in `EngineFwd.h`, `Engine.h/.cpp`,
`World.h/.cpp`, `SceneComponent.h`, `PrimitiveComponent.h/.cpp`,
`DirectionalLightComponent.cpp`, `SkyBoxComponent.cpp`, and
`Mona/SceneViewport.h/.cpp`. Direct editor consumers are
`DurinEd/Public/Preview/PreviewScene.h`,
`DurinEd/Private/Preview/PreviewScene.cpp`,
`DurinEd/Private/Thumbnail/RenderedAssetThumbnailPreviewScene.cpp`, and
`MaterialEditor/Private/Widgets/MaterialPreview.cpp`.

The direct concrete-scene test consumers are
`Materials/MaterialTestSupport.h`, `Materials/StaticMeshUpdateTests.cpp`,
`SkyBox/SkyBoxTestSupport.h`, `SkyBox/SkyBoxComponentTests.cpp`,
`SkyBox/SkyBoxEditorTests.cpp`, `SkyBox/SkyBoxVulkanTests.cpp`,
`Texture/TextureCookTests.cpp`, and `World/WorldTestSupport.h`. Documentation
references are updated only where they describe the live renderer scene;
historical plan handoffs and investigations keep their historical vocabulary.

#### Baseline validation

The `Win64-Debug-DurinEditor-Tests` Agent Build Profile completed
`.\DevTool.bat test --target all --output compact --plain` on 2026-07-30.
The complete `all` build passed, followed by 807 registered tests with zero
failures and one existing platform-dependent skip. The run included
`EditorRenderingTests` (29), `RendererResourceReloadVulkanTests` (1),
`SkyBoxTests` (9), `SkyBoxVulkanIntegrationTests` (1), `MaterialTests` (51),
`StaticMeshTests` (54), `StaticModelImportVulkanTests` (1), and
`ThumbnailTests` (45), plus the RenderCore resource, shader, and contract
targets. The CTest log is
`Build/.agent-state/logs/20260730-145045-340644-8028-ctest.log`.

Open questions: none. Stage 1 must preserve the inventory and behavior above;
any conflict discovered during movement returns here as an explicit changed
decision before implementation continues.

## Goal

- Make `FRendererModule` a thin module lifecycle and public-interface adapter.
- Give per-view orchestration one explicit `FSceneRenderer` owner.
- Give every independent drawing responsibility a concrete module-private
  renderer type with a matching header and implementation filename.
- Move shader maps, pipelines, RHI payloads, size-keyed targets, dynamic
  geometry, and retry state into the renderer or resource object that owns
  their lifetime.
- Separate shared renderer resources from feature renderers so a helper is not
  called a renderer merely because rendering code uses it.
- Preserve rendered output, render-thread confinement, lazy resource creation,
  invalidation behavior, failure isolation, and shutdown ordering at every
  stage.
- Replace the ambiguous public `IScene`/`FScene` renderer types with
  `IRenderScene`/`FRenderScene` after the private ownership split is stable.

## Scope

- `Engine/Source/Runtime/Renderer` public and private source organization.
- `FRendererModule` startup, shutdown, scene creation, and render delegation.
- Scene render orchestration, viewport fitting, render-pass ordering, and
  output transitions.
- StaticMesh, SkyBox, TextureCube thumbnail, Post Process, and Editor
  Assistance renderer ownership.
- Grid, Gizmo, Overlay Line, and Overlay Icon sub-renderers beneath Editor
  Assistance.
- Default texture, fullscreen geometry, render-target layout, resource
  generation, retry, invalidation, and diagnostic ownership.
- Renderer-facing tests and direct `IScene`/`FScene` consumers affected by the
  final public rename.
- Lasting Renderer ownership and lifecycle documentation after implementation.

## Non-Goals

- Introducing a render graph, frame graph, mesh draw command cache, or generic
  render-pass framework.
- Adding a public `IRenderer` hierarchy or runtime-polymorphic feature
  renderer registry.
- Changing shader source interfaces, material permutation policy, vertex
  formats, RHI abstractions, or pipeline-state semantics.
- Changing scene-proxy ownership, material render-proxy behavior, StaticMesh
  render-data lifetime, or vertex-factory design owned by adjacent plans.
- Moving editor assistance into an Editor module or rendering it through
  Mona/ImGui.
- Changing Lit/Unlit, Solid/Wireframe, FXAA, skybox, thumbnail, fixed-aspect,
  or editor-assistance visuals.
- Broad naming cleanup outside Renderer and the direct scene-interface
  consumers.
- Performance tuning beyond preventing structural regressions in current
  demand-driven creation and bounded size-keyed caches.

## Design Decisions and Invariants

### Target source layout

The Renderer module will use `Private/Renderers/` as a collection directory.
The plural name avoids the redundant path `Renderer/Private/Renderer/`.
Feature renderers and their primary files align one-to-one:

```text
Engine/Source/Runtime/Renderer/
|-- Public/
|   |-- RendererModule.h
|   |-- RenderScene.h
|   `-- DefaultTextures.h
`-- Private/
    |-- RendererModule.cpp
    |-- RenderScene.cpp
    |-- Renderers/
    |   |-- SceneRenderer.h
    |   |-- SceneRenderer.cpp
    |   |-- StaticMeshRenderer.h
    |   |-- StaticMeshRenderer.cpp
    |   |-- SkyBoxRenderer.h
    |   |-- SkyBoxRenderer.cpp
    |   |-- TextureCubeThumbnailRenderer.h
    |   |-- TextureCubeThumbnailRenderer.cpp
    |   |-- PostProcessRenderer.h
    |   |-- PostProcessRenderer.cpp
    |   `-- EditorAssistance/
    |       |-- EditorAssistanceRenderer.h
    |       |-- EditorAssistanceRenderer.cpp
    |       |-- EditorGridRenderer.h
    |       |-- EditorGridRenderer.cpp
    |       |-- GizmoRenderer.h
    |       |-- GizmoRenderer.cpp
    |       |-- OverlayLineRenderer.h
    |       |-- OverlayLineRenderer.cpp
    |       |-- OverlayIconRenderer.h
    |       `-- OverlayIconRenderer.cpp
    `-- Resources/
        |-- DefaultTextureResources.h
        |-- DefaultTextureResources.cpp
        |-- FullscreenGeometryResources.h
        |-- FullscreenGeometryResources.cpp
        |-- RenderTargetLayouts.h
        |-- RenderTargetLayouts.cpp
        |-- RendererResourceCoordinator.h
        `-- RendererResourceCoordinator.cpp
```

The exact helper count may shrink when a helper can be private to one renderer,
but the implementation must not replace the current monoliths with numbered
fragments or unrelated free-function files. A new file requires a coherent
owner or independently reusable renderer-private responsibility.

### Ownership composition

The persistent ownership hierarchy is:

```text
FRendererModule
`-- FSceneRenderer
    |-- FRendererResourceCoordinator
    |-- FDefaultTextureResources
    |-- FFullscreenGeometryResources
    |-- FStaticMeshRenderer
    |-- FSkyBoxRenderer
    |-- FTextureCubeThumbnailRenderer
    |-- FPostProcessRenderer
    `-- FEditorAssistanceRenderer
        |-- FEditorGridRenderer
        |-- FGizmoRenderer
        |-- FOverlayLineRenderer
        `-- FOverlayIconRenderer
```

- `FRendererModule` owns one `FSceneRenderer`, creates render scenes, and
  delegates rendering and lifecycle events. It does not define shaders,
  construct pipelines, enumerate proxies, or submit feature draws.
- `FSceneRenderer` owns feature renderers and preserves the complete per-view
  orchestration order. It is a concrete composed type, not a virtual feature
  registry.
- Each feature renderer owns its complete creation slots, keyed caches,
  shaders, pipelines, samplers, geometry, and diagnostics. Existing
  `FStaticMeshRendererState`, `FSkyBoxRendererState`,
  `FTextureCubeThumbnailRendererState`, `FPostProcessRendererState`, and editor
  assistance global state become members rather than renamed global structs.
- A renderer may expose `Render`, `Prepare`, `Draw`, `ReleaseResources`, and
  invalidation operations appropriate to its feature. The plan does not force
  unlike renderers through a common base class.
- Shared facilities are concrete resource or coordinator types. They are not
  named `Renderer` unless they submit the drawing work for a coherent feature.

### Render-thread and lifetime rules

- GPU resource creation, publication, mutation, invalidation, retry, and
  release remain render-thread-only.
- Feature renderer construction may establish CPU-only owner objects during
  module startup, but it must not eagerly create RHI resources that are
  currently demand-driven.
- Public and internal entry points that require the rendering thread retain
  explicit thread assertions.
- Renderer shutdown first closes console-command admission and unregisters
  commands, then enqueues release of every feature and shared resource, flushes
  rendering commands, and only then destroys inert owning objects.
- Render-scene creation and game-thread scene mutation contracts remain
  unchanged during the private split.
- No refactoring stage may make a feature renderer, render scene, or scene
  proxy outlive its established module or engine owner.

### Resource creation and invalidation

- `TRenderResourceCreationSlot` remains the atomic candidate-publication
  mechanism. Callers continue to observe a complete current payload, a
  complete last-known-good payload, or no payload.
- Shader, device, and manual resource generations remain independent.
- Same-generation failures remain suppressed; an applicable generation change
  permits one lazy retry.
- Shader and manual refresh may retain last-known-good payloads. Device
  invalidation clears device-dependent payloads before retry.
- `renderer.reload-shaders changed`, `renderer.reload-shaders all`, and
  `renderer.retry-resources` retain their names, admission behavior, render
  command ordering, force-recompile semantics, and diagnostics.
- `FRendererResourceCoordinator` owns shared generation and console-command
  coordination. `FSceneRenderer` explicitly forwards invalidation to concrete
  feature/resource owners; it does not introduce a virtual listener registry.
- Failure isolation remains per feature or exact pipeline identity. A failed
  SkyBox, Grid, Gizmo, Overlay Line, Overlay Icon, thumbnail, material shader
  map, or exact pipeline must not suppress an independent available draw.

### Per-view ordering and output behavior

`FSceneRenderer` preserves the current order:

1. Reject a null or zero-sized output without creating demanded resources.
2. Derive Present or Offscreen output and editor-assistance demand from the
   immutable `FSceneView`.
3. Resolve size-keyed Scene Color and depth targets.
4. Fit the constrained content viewport and leave outside fixed-aspect regions
   at the view clear color.
5. Render SkyBox, StaticMesh proxies, and TextureCube thumbnail proxies into
   Scene Color/depth in their current order and conditions.
6. Prepare demanded editor assistance.
7. Copy or apply FXAA from Scene Color to the final output.
8. When at least one assistance operation is drawable, load preserved output
   color and depth, restore the constrained viewport/scissor, and draw Grid,
   X-Ray operations, then visible depth-tested operations.
9. Apply only the current Present or ShaderReadOnly final transition.

The refactoring must not create renderer-global semantic view settings.
Shading, rasterization, post-process, assistance, viewport, and aspect policy
continue to come from the immutable per-view snapshot.

### Naming and visibility

- A feature type named `FXxxRenderer` has matching
  `XxxRenderer.h`/`XxxRenderer.cpp` files.
- Renderer-private collection namespaces such as
  `RendererEditorAssistance`, `RendererFullscreenGeometry`, and
  `RendererRenderTargetLayouts` are removed or narrowed as their functions
  become owner methods or clearly named shared facilities.
- Module-private renderer classes remain under `Private`; they are not
  exported through `RENDERER_API`.
- The public module implementation remains `FRendererModule` because it
  implements `IRendererModule`.
- The renderer-owned scene becomes `IRenderScene`/`FRenderScene`, with
  `RenderScene.h`/`RenderScene.cpp`. `FSceneViewport` retains its current name
  because it is a viewport implementation rather than the renderer scene.

### Adjacent-plan sequencing

- StaticMesh render-data and vertex-factory types are consumed as established
  inputs from the Static Mesh Render-Data Lifecycle and Static Mesh LOD
  Resources Refactor plans.
- Material shader-map identities, proxy bindings, and invalidation semantics
  are consumed as established inputs from the Material Render-Proxy
  Invalidation plan.
- `FStaticMeshRenderer` extraction begins only from an explicit handoff or
  completed baseline for the overlapping stages of those plans. It moves
  renderer ownership without reopening their selected designs.
- If an adjacent plan changes a symbol recorded by this plan, Stage 0 updates
  the working set and baseline before implementation instead of preserving a
  compatibility layer for an obsolete intermediate type.

## Current Foundations and Gaps

### Foundations

- `FRendererModule` and `IRendererModule` already provide a narrow public
  module boundary.
- `FSceneView` already carries immutable per-view output policy.
- `TRenderResourceCreationSlot` and `TRendererResourceSlotCache` already
  provide recoverable fixed and keyed resource publication.
- `FRendererResourceInvalidationController` already centralizes console
  command admission and cause selection.
- `RendererRenderTargetLayouts` has focused tests for preserved depth and
  final output transitions.
- Editor assistance already separates pure request analysis from demanded
  resource preparation and drawing, and has focused failure-isolation and draw
  ordering tests.
- `SkyBoxRendering` and `EditorGridRendering` already isolate testable uniform
  construction from RHI submission.
- Renderer scene-color/depth intermediates are already bounded and keyed by
  viewport dimensions.

### Gaps

- `RendererModule.cpp` contains nearly every non-editor feature shader,
  resource state, factory, draw function, invalidation hook, and frame
  orchestration path.
- `RendererEditorAssistanceRenderer.cpp` contains four feature resource
  owners, procedural geometry generation, pipeline selection, dynamic
  uploads, and all assistance draw paths.
- Persistent feature state is held by anonymous globals, so ownership and
  destruction order must be reconstructed from free functions.
- Existing names mix module context, functional namespaces, and implementation
  suffixes: `RendererEditorAssistanceRenderer.cpp`,
  `RendererFullscreenGeometry`, `RendererRenderTargetLayouts`, and generic
  `IScene`/`FScene`.
- The small `RendererEditorAssistance.cpp` and large
  `RendererEditorAssistanceRenderer.cpp` share one header, so filenames do not
  predict where analysis, resource, and draw behavior is implemented.
- `RendererModule.cpp` is the conflict point for active StaticMesh and material
  work, making otherwise independent renderer changes difficult to stage.

## Implementation Stages

### Stage 0: Freeze the modularization baseline

Dependencies: stable handoffs from overlapping active-plan stages.

- [x] Record the implementation baseline commit and confirm the Renderer
  working set has no overlapping uncommitted edits.
- [x] Inventory every Renderer anonymous global, creation slot, keyed cache,
  console callback, shader type, draw helper, proxy enumeration path, and
  release action, assigning exactly one target owner.
- [x] Record the current render-thread assertions, render-pass order, viewport
  and scissor changes, resource-generation transitions, and shutdown order.
- [x] Run the existing focused Renderer, render-resource, shader reload,
  editor-assistance, render-target-layout, SkyBox, Editor Grid, material,
  StaticMesh, and rendered-output baselines.
- [x] Confirm the exact handoff commits and affected symbols from the active
  StaticMesh and material plans before selecting the
  `FStaticMeshRenderer` working set.
- [x] Freeze the target filenames and public rename consumer list; put any
  unresolved ownership decision in this stage before source movement begins.

#### Acceptance Gate

- Every current state object and free function has one selected destination,
  adjacent-plan ownership is non-overlapping, existing behavioral baselines
  pass, and Stage 1 has a bounded working set.

#### Stage 0 Implementation Handoff

- Baseline: `3092da1f58bd89126be49db2bc9a00d3c2673af9`.
- Working set: the Renderer sources and direct consumers enumerated in the
  frozen inventory above; Stage 1 is limited to shared-resource ownership.
- Key symbols: `FRendererResourceInvalidationController`,
  `FRenderResourceGeneration`, `TRenderResourceCreationSlot`,
  `TRendererResourceSlotCache`, `GDefaultTextures`,
  `RendererFullscreenGeometry`, `RendererRenderTargetLayouts`,
  `GStaticMeshState`, `GTextureCubeThumbnailState`, `GSkyBoxState`,
  `GPostProcessState`, and `RendererEditorAssistance::GState`.
- Decisions: the target composition and filenames in this plan are frozen;
  creation slots move intact before feature behavior changes; current
  post-process and assistance keyed storage remains bounded and demand-driven;
  adjacent StaticMesh and material ownership APIs are inputs, not migration
  targets.
- Open questions: none.
- Validation: complete Tests-profile `all` build and aggregate passed; 807
  registered tests, zero failures, one existing skip.

### Stage 1: Establish explicit shared resource ownership

Dependencies: Stage 0.

- [ ] Introduce `FRendererResourceCoordinator` for resource generations,
  force-recompile selection, console-command admission, invalidation
  sequencing, and snapshots.
- [ ] Convert default texture globals into `FDefaultTextureResources` while
  preserving the public default texture accessors and render-thread-only
  initialization.
- [ ] Convert shared fullscreen geometry state into
  `FFullscreenGeometryResources` without assigning it to Post Process or
  Editor Grid exclusively.
- [ ] Move and rename renderer-private render-target layout helpers to
  `Resources/RenderTargetLayouts.*`, preserving their pure layout API and
  focused tests.
- [ ] Make release explicit and idempotent for each new owner; keep destruction
  after the shutdown flush.
- [ ] Add focused ownership/lifecycle tests where existing resource-slot tests
  do not cover moved coordinator behavior.

#### Acceptance Gate

- Shared resources and generation state have explicit owners, the development
  commands and retry snapshots are behaviorally unchanged, and no feature
  renderer has gained ownership of a resource shared by another feature.

### Stage 2: Extract independent scene feature renderers

Dependencies: Stage 1.

- [ ] Extract `FSkyBoxRenderer`, including shader types, resource slot,
  uniform preparation, validation, and draw submission.
- [ ] Extract `FTextureCubeThumbnailRenderer`, including its shader/pipeline
  slot, sampler, vertex declaration, proxy filtering input, and drawing.
- [ ] Extract `FPostProcessRenderer`, including copy/FXAA shaders and
  pipelines, bounded size-keyed Scene Color/depth targets, fullscreen drawing,
  and output variants.
- [ ] Replace copied payload fields with one authoritative committed payload
  per owner unless a documented mutable field requires separate storage.
- [ ] Preserve lazy creation, exact pipeline identities, fallback behavior,
  viewport sizing, and Present/Offscreen transitions.
- [ ] Keep proxy enumeration and total scene ordering in the current
  orchestrator until `FSceneRenderer` is introduced.

#### Acceptance Gate

- SkyBox, TextureCube thumbnail, and Post Process shaders, resources, caches,
  and drawing no longer live in `RendererModule.cpp`; each concrete renderer
  passes focused and rendered-output validation with unchanged output.

### Stage 3: Extract the StaticMesh renderer

Dependencies: Stages 1 and 2 plus the recorded StaticMesh/material handoffs
from Stage 0.

- [ ] Introduce `FStaticMeshRenderer` as the owner of base resources,
  material shader-map slots, material pipeline slots, StaticMesh shader types,
  draw uniforms, diagnostics, and proxy draw submission.
- [ ] Consume the established `FStaticMeshSceneProxy`, material render-proxy,
  render-data, buffer, and vertex-factory APIs without introducing adapter
  ownership.
- [ ] Move Lit/Unlit and Solid/Wireframe selection without changing shader-map
  or pipeline identities.
- [ ] Preserve last-known-good shader/pipeline payload behavior across shader
  and manual invalidation and clear device-dependent payloads on device
  invalidation.
- [ ] Keep scene-proxy enumeration non-owning and render-thread confined.
- [ ] Run focused material binding, material invalidation, StaticMesh
  render-data lifetime, resource reload, pipeline identity, and Vulkan
  rendered-output tests.

#### Acceptance Gate

- All StaticMesh renderer state and draw code have one concrete owner, no
  adjacent-plan lifetime or invalidation contract changed, and the established
  Lit/Unlit, Solid/Wireframe, material, reload, and rendered-output baselines
  pass.

### Stage 4: Decompose Editor Assistance

Dependencies: Stages 1 and 2.

- [ ] Introduce `FEditorAssistanceRenderer` as the owner of request analysis,
  per-view preparation, operation ordering, and aggregate draw orchestration.
- [ ] Extract `FEditorGridRenderer`, `FGizmoRenderer`,
  `FOverlayLineRenderer`, and `FOverlayIconRenderer` with their own base
  resources, exact-key pipeline caches, geometry preparation, dynamic
  capacities, diagnostics, invalidation, retry, and release.
- [ ] Retain one immutable prepared result per view; do not move dynamic
  per-view counts or selected operations into persistent global state.
- [ ] Preserve demand-driven initialization: an empty view creates no
  assistance resources, and one demanded feature does not initialize
  unrelated features.
- [ ] Preserve independent failure isolation and current Grid, X-Ray, and
  visible operation ordering.
- [ ] Absorb `EditorGridRendering` helpers into `FEditorGridRenderer` only if
  their deterministic uniform tests remain independent of RHI setup;
  otherwise retain a narrowly named private helper.
- [ ] Remove the ambiguous split between
  `RendererEditorAssistance.cpp` and
  `RendererEditorAssistanceRenderer.cpp`.

#### Acceptance Gate

- Editor assistance is composed from explicit feature owners, no replacement
  monolith remains, empty-demand and partial-failure behavior is unchanged,
  and all assistance request, pipeline, ordering, geometry, and rendered
  editor tests pass.

### Stage 5: Introduce the Scene Renderer and thin the module

Dependencies: Stages 2 through 4.

- [ ] Introduce `FSceneRenderer` and transfer fitted viewport calculation,
  render-target resolution, scene-pass setup, feature ordering, post-process
  setup, editor-assistance preparation/final pass, and final transitions.
- [ ] Compose all feature renderers and shared resources under
  `FSceneRenderer`.
- [ ] Move proxy enumeration to the concrete renderer that consumes each
  proxy, while keeping `FSceneRenderer` responsible for ordering renderers.
- [ ] Make invalidation and shutdown fan-out explicit and deterministic.
- [ ] Reduce `FRendererModule::RenderView` to validation/delegation and
  `FRendererModule::StartupModule`/`ShutdownModule` to owner lifecycle.
- [ ] Ensure `RendererModule.cpp` contains no feature shader declaration,
  pipeline factory, resource payload, procedural geometry, or feature draw
  implementation.

#### Acceptance Gate

- `FRendererModule` is a thin adapter, `FSceneRenderer` is the sole frame
  orchestrator, concrete renderers own feature work, and multi-view,
  fixed-aspect, Present/Offscreen, FXAA, assistance, and shutdown behavior
  match the baseline.

### Stage 6: Align the public render-scene naming

Dependencies: Stage 5.

- [ ] Rename `IScene` to `IRenderScene`, `FScene` to `FRenderScene`, and
  `Scene.h`/`Scene.cpp` to `RenderScene.h`/`RenderScene.cpp`.
- [ ] Update `IRendererModule::CreateScene`, `RenderView`, engine world scene
  ownership, scene proxies, tests, and direct renderer consumers atomically.
- [ ] Keep `FSceneViewport`, engine world, actor/component, and editor preview
  scene names unchanged where they represent different abstractions.
- [ ] Preserve game-thread mutation, render-command capture, release, light,
  SkyBox, proxy replacement, and revision behavior.
- [ ] Remove obsolete compatibility aliases in the same stage; the repository
  must have one render-scene vocabulary.

#### Acceptance Gate

- Renderer APIs and files consistently use RenderScene terminology, unrelated
  scene abstractions are untouched, and all scene lifetime, viewport,
  component render-state, preview-scene, and renderer tests pass.

### Stage 7: Validate and publish the lasting contract

Dependencies: Stages 1 through 6.

- [ ] Remove obsolete files, anonymous feature globals, redundant state
  copies, old namespaces, temporary forwarding functions, and stale includes.
- [ ] Confirm every module-private `FXxxRenderer` has matching files and owns
  one coherent draw responsibility.
- [ ] Confirm no feature resource can be released twice, survive module
  shutdown, or mutate outside the rendering thread.
- [ ] Run the focused validation matrix, the applicable native suites, a
  successful full `all` build, and the hidden-window editor smoke test through
  the repository DurinDevTool workflow.
- [ ] Update the Runtime rendering documentation with the implemented
  ownership hierarchy, resource invalidation flow, scene naming, and render
  order.
- [ ] Validate all plans and record the final implementation handoff and
  evidence.

#### Acceptance Gate

- The target ownership and file layout are the only supported implementation,
  all behavioral and lifecycle validation succeeds, lasting contracts are
  documented outside the plan, and no required cleanup or compatibility path
  remains.

## Validation Matrix

| Area | Representative coverage | Required result |
| --- | --- | --- |
| Module lifecycle | startup, command registration failure, shutdown admission close, queued release, flush, object destruction | Same ordering, no leaked or late-released RHI payload |
| Resource recovery | changed/all shader reload, manual retry, same-generation suppression, stale-ready fallback, device invalidation seam | Same generation, retry, fallback, and diagnostic behavior |
| StaticMesh | Lit/Unlit, Solid/Wireframe, material identities, proxy updates, missing/failed resources | Same shader/pipeline selection and rendered output |
| SkyBox | absent/present scene data, revision, fitted viewport, transform validation, failed resources | Same background, isolation, and fallback behavior |
| TextureCube thumbnail | preview proxy selection, pipeline creation, sampling, invalid resources | Same preview rendering and independent failure behavior |
| Post Process | copy, FXAA, Present, Offscreen, resized and alternating viewport extents | Same final image, transitions, and bounded target reuse |
| Editor Assistance | empty demand; Grid, solid/wire Gizmo, Lines, Icons; X-Ray/visible; partial failures | Demand-driven creation, exact draw order, and independent availability |
| Render-target layouts | Scene Color/depth, preserved depth, assistance load, final output transition | Existing layout tests pass without semantic changes |
| Multi-view and aspect | main/auxiliary views, camera preview, window-backed output, fixed aspect, black/clear bars | Independent per-view policy and correct viewport/scissor restoration |
| Render scene | proxy add/replace/remove, transforms, material binding, lights, SkyBox revisions, release | Same game/render-thread ownership and scene contents |
| Structure | target directories, matching type/file names, no feature anonymous globals, thin module | Ownership is discoverable from types and source layout |
| Final integration | focused tests, applicable native suites, full `all` build, hidden-window editor smoke | All required validation succeeds from one Agent Build Profile |

Build, test, and runtime validation use
[Build and Run](../Development/Build/BuildAndRun.md); this plan does not copy
commands or output paths that may change.

## Definition of Done

- `FRendererModule` owns and delegates to one `FSceneRenderer` and contains no
  feature rendering implementation.
- StaticMesh, SkyBox, TextureCube thumbnail, Post Process, Editor Assistance,
  Grid, Gizmo, Overlay Line, and Overlay Icon responsibilities have concrete
  module-private owners with matching filenames.
- Default textures, fullscreen geometry, render-target layouts, and resource
  invalidation have explicit shared-resource owners rather than feature or
  anonymous global ownership.
- No current `GStaticMeshState`, `GSkyBoxState`,
  `GTextureCubeThumbnailState`, `GPostProcessState`, editor-assistance global
  aggregate, or equivalent renamed feature global remains.
- Render order, immutable view policy, render-thread confinement, lazy
  creation, independent generation tracking, last-known-good fallback,
  failure isolation, output transitions, and ordered shutdown match the
  established contract.
- `IRenderScene`/`FRenderScene` is the only renderer-scene vocabulary, while
  other scene abstractions retain their domain-specific names.
- Adjacent StaticMesh and material plans retain ownership of their selected
  lifecycle, vertex-factory, and invalidation designs.
- Each stage lands as a bounded commit with its checklist, baseline, working
  set, decisions, open questions, and validation handoff updated.
- Lasting ownership and lifecycle rules are published in Runtime rendering
  documentation.
- Plan validation, focused tests, final full build, and editor runtime smoke
  validation all pass.

## Deferred Follow-ups

- A render graph or frame graph after resource transitions and pass
  dependencies require runtime scheduling rather than fixed orchestration.
- A public or runtime-polymorphic renderer/pass interface after a second
  module needs feature registration.
- Mesh draw command caching, PSO precaching, render sorting, batching, or
  parallel command generation.
- Moving editor assistance to a separate module after module dependency and
  runtime/editor availability requirements are independently selected.
- Splitting Post Process into a chain of effects when more than the current
  copy/FXAA policy exists.
- Renderer performance changes not required to preserve current bounded caches
  and demand-driven resource behavior.

## Related Documentation

- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)
- [Static Mesh Render-Data Lifecycle Plan](StaticMeshRenderDataLifecycle.md)
- [Static Mesh LOD Resources Refactor Plan](StaticMeshLODResourcesRefactor.md)
- [Material Render-Proxy Invalidation Plan](MaterialRenderProxyInvalidation.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/IRendererModule.h`
- `Engine/Source/Runtime/Renderer/Public/RendererModule.h`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Public/DefaultTextures.h`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistance.h`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistance.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistanceRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererFullscreenGeometry.h`
- `Engine/Source/Runtime/Renderer/Private/RendererFullscreenGeometry.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererRenderTargetLayouts.h`
- `Engine/Source/Runtime/Renderer/Private/RendererRenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererResourceInvalidation.h`
- `Engine/Source/Runtime/Renderer/Private/RendererResourceInvalidation.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererResourceSlotCache.h`
- `Engine/Source/Runtime/Renderer/Private/SkyBoxRendering.h`
- `Engine/Source/Runtime/Renderer/Private/SkyBoxRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.h`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererEditorAssistanceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererRenderTargetLayoutTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceInvalidationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceReloadVulkanTests.cpp`
