# Primitive Draw Interface Plan

Summary: Introduce a UE-aligned primitive draw submission and simple-element rendering path for editor and debug geometry.

Last reviewed: 2026-08-29

Status: Active
Completed:

## Current Status

Editor and debug producers currently append Renderer-specific overlay values
directly to `FSceneView`. `FEditorAssistanceRenderer` then dispatches separate
EditorGrid, Gizmo, OverlayLine, and OverlayIcon renderers. Line and icon paths
duplicate shader/pipeline slots, depth/output variants, diagnostics, dynamic
vertex/index buffer growth, upload, and release. Producers also depend on
closed overlay enums such as `EViewOverlayIcon`, preventing general project or
plugin drawing without expanding RenderCore vocabulary.

This plan introduces the Unreal Engine concepts `FPrimitiveDrawInterface`,
`FSimpleElementCollector`, and `FSimpleElementRenderer`. The first migration
covers line, translucent line, point, and sprite submission, including the
current overlay line/icon and wire-gizmo use cases. Procedural fullscreen Grid
and solid Gizmo mesh rendering remain specialized until a later mesh-batch
foundation exists. The renderer implementation depends on the accepted Global
Shader Framework Stages 0-2 rather than creating another private shader-map
lifecycle.

## Goal

Let editor, engine, project, and debug producers describe simple visual
primitives through a stable UE-aligned interface while one shared collector and
renderer own batching, clipping, depth variants, shaders, pipelines, transient
GPU upload, diagnostics, and lifecycle.

## Scope

- Engine-public `FPrimitiveDrawInterface` producer contract with UE-aligned
  `DrawLine`, `DrawTranslucentLine`, `DrawPoint`, and `DrawSprite` vocabulary.
- `ESceneDepthPriorityGroup` with bounded World and Foreground semantics, plus
  explicit conversion of the existing XRay+Visible presentation into two
  submitted elements rather than hidden renderer duplication.
- Durin-specific line style data for screen-space width and solid/dashed
  pattern without changing the base meaning of UE-aligned calls.
- Value-owned, immutable per-view simple-element submission data crossing from
  view construction to the render thread; the PDI object itself never crosses
  threads or owns RHI resources.
- `FSimpleElementCollector` classification, stable ordering, batching,
  clipping, capacity accounting, and diagnostics.
- `FSimpleElementRenderer` global shaders, vertex declarations, keyed pipelines,
  transient vertex/index upload, draw execution, failure isolation, device
  recovery, and shutdown.
- Migration of OverlayLine, OverlayIcon, wire Gizmo, collision/debug lines, and
  editor visualization icons to the shared path.
- CPU contract, collector, clipping, batching, resource failure/recovery,
  Vulkan visual, interaction, lifecycle, and budget validation.

## Non-Goals

- Moving the procedural fullscreen Editor Grid into line batches. Its analytic
  depth, camera-relative precision, decimal LOD, and fullscreen shader remain a
  specialized pass.
- Replacing solid Gizmo procedural meshes before `FMeshBatch`, vertex factory,
  material proxy, and mesh pass ownership are designed.
- Introducing `FMeshElementCollector`, `FMeshBatch`, `FMeshPassProcessor`,
  `FDynamicMeshBuilder`, hit proxies, selection outlines, or general scene
  primitive rendering in this plan.
- A public API for arbitrary user shaders, raw RHI buffers, pipeline
  initializers, render-target selection, or direct command-list access.
- Preserving `FViewOverlayLine` and `FViewOverlayIcon` as permanent parallel
  producer APIs after migration.
- Making PDI calls from arbitrary worker or render threads; producer-thread
  admission and immutable submission remain explicit.
- A renderer-wide PSO cache or general dynamic-buffer allocator beyond the
  bounded simple-element path.

## Design Decisions and Invariants

- `FPrimitiveDrawInterface` lives in Engine, matching its producer-facing scene
  role and preserving the existing dependency direction `Engine -> RenderCore`.
  It writes RenderCore-owned value types through an Engine implementation and
  does not require RenderCore to depend on Engine or Renderer.
- The concrete view-building implementation is `FViewPrimitiveDrawInterface`.
  It is stack/frame scoped, non-copyable, contains no RHI reference, and becomes
  invalid when immutable `FSceneView` submission is sealed.
- `FPrimitiveDrawInterface` methods copy all caller data. No retained pointer to
  textures, modules, actors, components, editor tools, strings, or temporary
  arrays may cross the submission boundary. Sprite resources use a stable,
  explicitly retained render-resource reference selected in Stage 0.
- `ESceneDepthPriorityGroup::World` enables scene-depth testing;
  `ESceneDepthPriorityGroup::Foreground` draws without scene-depth rejection.
  Existing XRay+Visible overlays submit a low-alpha Foreground element followed
  by an opaque/normal-alpha World element, preserving current ordering without
  a hidden `XRayAndVisible` public mode.
- `DrawLine` is opaque with respect to caller alpha;
  `DrawTranslucentLine` preserves alpha. Durin-specific dash and screen-width
  behavior lives in an explicit style value or overload selected in Stage 0,
  not in ambiguous flag combinations.
- `DrawSprite` accepts a texture/resource identity and UV rectangle rather than
  the closed `EViewOverlayIcon` enum. Camera, light, and player-start icons
  resolve through an atlas/resource owner before submission.
- Submission order is stable within one depth group and blend class. Batching
  may merge compatible elements but cannot reorder Foreground/World layers,
  opaque/translucent blending, sprites, hit-visible diagnostics, or the existing
  Grid-before-assistance contract.
- `FSimpleElementCollector` owns CPU-side classification and immutable prepared
  batches; `FSimpleElementRenderer` owns only shared GPU rendering resources.
  Neither knows about LevelEditor tools, actor classes, icon meanings, or Gizmo
  interaction state.
- Simple-element shader types resolve through `FGlobalShaderMap`; the renderer
  does not compile or retain a private `FShaderMapBase`.
- Each distinct pipeline identity includes primitive topology, blend class,
  depth priority, depth convention, output layout, vertex declaration, and
  exact global shader-set identity. Debug names are not cache identities.
- Dynamic vertex/index uploads grow geometrically or allocate from a bounded
  frame-local pool, never shrink per frame, retry safely after allocation
  failure, and publish no partial draw batch. Stage 0 selects the initial
  strategy against existing command-list allocation and memory budgets.
- One unsupported texture, malformed element, failed pipeline, or failed batch
  allocation cannot suppress independent compatible batches. Diagnostics are
  generation-scoped and do not repeat every frame.
- Device invalidation drops every dependent RHI resource before recreation;
  producer values and already sealed CPU submissions never authorize RHI
  fallback across a device generation.

## Current Foundations and Gaps

`FSceneView` already transports value-owned `FViewOverlayLine`,
`FViewOverlayIcon`, and `FViewOverlayPrimitive` arrays. LevelEditor viewport
clients and customizations populate those arrays before render submission.
Renderer already preserves final scene color/depth, orders Grid before XRay and
Visible assistance, clips world lines in homogeneous space, generates
screen-space quads, samples the icon atlas, and isolates pipeline failures by
feature and depth variant.

The missing boundary is a producer-facing draw interface and common simple
element path. OverlayLine and OverlayIcon separately build compatible dynamic
indexed geometry and almost identical PSOs. `FViewOverlayIcon` exposes a closed
engine enum rather than a texture/UV resource contract. Wire Gizmo geometry is
owned by the same specialized renderer as solid shapes, and debug producers
must know `FSceneView` storage rather than submit primitives through an
interface.

## Implementation Stages

### Stage 0: Freeze producer semantics, ownership, and visual baseline

- [ ] Inventory every writer and reader of overlay line, icon, point-like,
  wire-gizmo, collision/debug, and sprite data; classify unsupported solid mesh
  and fullscreen cases explicitly.
- [ ] Select exact signatures and value types for `DrawLine`,
  `DrawTranslucentLine`, `DrawPoint`, and `DrawSprite`, including coordinate
  space, color/alpha, width, dash period, UVs, texture retention, depth bias,
  screen-space behavior, and invalid-input policy.
- [ ] Define `ESceneDepthPriorityGroup` ordering and the two-element conversion
  for existing XRay+Visible visuals across forward/reversed depth and
  Present/Offscreen output.
- [ ] Select persistent-capacity versus frame-local pooled upload for initial
  simple elements, with overflow, allocation failure, retry, and byte-budget
  behavior.
- [ ] Capture current line clipping, pixel width, dash phase, icon atlas/UV,
  hover color, occlusion, draw order, batch counts, upload bytes, failure
  isolation, Vulkan captures, and shutdown behavior.

#### Acceptance Gate

- Every initial producer maps to an exact PDI call sequence with value
  ownership, depth/blend ordering, texture lifetime, and invalid-input behavior;
  Grid and solid mesh cases have explicit retained owners.
- The selected upload and batching model has measurable bounds and can preserve
  the current visual and failure baseline without exposing Renderer/RHI types to
  producers.

### Stage 1: Introduce `FPrimitiveDrawInterface` and immutable view submission

- [ ] Add Engine-public `FPrimitiveDrawInterface`,
  `FViewPrimitiveDrawInterface`, `ESceneDepthPriorityGroup`, and selected simple
  element values with UE-aligned names and repository API/export conventions.
- [ ] Implement copy-based `DrawLine`, `DrawTranslucentLine`, `DrawPoint`, and
  `DrawSprite` admission with finite-value, size, UV, resource, and per-view
  count/byte validation.
- [ ] Add a sealed value-owned simple-element list to the view submission
  boundary; keep a temporary private conversion from existing overlay arrays
  only while individual producers migrate.
- [ ] Assert producer-thread use, reject calls after sealing, and prove queued
  render work retains no PDI, module callback, actor/component pointer, or
  transient caller memory.
- [ ] Add focused interface tests for call mapping, ordering, copies, invalid
  inputs, limits, texture retention, sealing, two-view isolation, and destruction.

#### Acceptance Gate

- Engine, editor, and project code can build an immutable per-view line, point,
  or sprite submission without naming EditorAssistance renderer types or
  mutating `FSceneView` overlay vectors directly.
- A sealed submission is deterministic, self-contained, bounded, and safe to
  execute after every producer-side temporary has been destroyed.

### Stage 2: Add simple-element collection, batching, and CPU geometry

- [ ] Add `FSimpleElementCollector` to classify elements by topology, blend,
  depth priority, depth convention, output, texture/sampler, and compatible
  style while retaining stable cross-class draw order.
- [ ] Move homogeneous line clipping, screen-space width expansion, dash
  coordinates, point quad generation, sprite billboard/UV generation, and
  finite rejection into shared CPU geometry utilities.
- [ ] Produce immutable prepared batches with exact vertex/index spans,
  pipeline keys, texture bindings, source counts, dropped counts, and byte
  accounting; no RHI creation occurs during collection.
- [ ] Preserve camera-relative/double-precision calculations where current
  world-space overlays require them and define behavior for near-plane,
  behind-camera, zero-length, oversized, and partially clipped elements.
- [ ] Add deterministic tests for topology, winding, clipping, dash continuity,
  pixel size, UVs, batching compatibility, stable ordering, overflow, and
  independent invalid-element rejection.

#### Acceptance Gate

- The collector converts the Stage 0 corpus into bounded prepared batches whose
  geometry, ordering, styles, UVs, and counters match the baseline without any
  Renderer, RHI, LevelEditor, or actor knowledge.
- Changing one element's blend, depth, texture, or style splits only the
  required batch and never reorders unrelated elements.

### Stage 3: Implement `FSimpleElementRenderer` on Global Shader infrastructure

- [ ] Register simple line/point/sprite vertex and fragment shaders through the
  accepted Global Shader Framework and use typed global refs for every batch.
- [ ] Add simple-element vertex declarations and Renderer-owned keyed pipeline
  slots for topology, blend, depth priority, convention, output layout, and
  exact shader-set generation while keeping concrete initializer construction
  local.
- [ ] Implement the selected dynamic upload path with bounded growth/allocation,
  full-batch publication, next-frame retry after failure, counters, device
  invalidation, and explicit release.
- [ ] Integrate prepared simple batches into the existing editor-assistance
  render-target load and ordering boundary after Grid and in the selected
  Foreground/World sequence.
- [ ] Add resource failure injection, same-generation suppression where slots
  apply, Manual retry, global-shader fallback coupling, device invalidation,
  Vulkan output, and shutdown tests.

#### Acceptance Gate

- One shared renderer draws all admitted line, point, and sprite batches with
  correct blend/depth/output variants and no private ShaderMap compilation.
- Allocation, shader, pipeline, texture, or device failure affects only the
  dependent batch and recovers under the documented generation rules without
  partial draw publication or per-frame diagnostic spam.

### Stage 4: Migrate editor and debug producers and remove overlay-specific paths

- [ ] Migrate LevelEditor overlay line and icon producers to PDI calls,
  resolving built-in icon atlas/UV data before `DrawSprite` and submitting
  explicit Foreground plus World elements for current XRay+Visible behavior.
- [ ] Migrate wire Gizmo shapes and eligible collision/debug visualization to
  `DrawLine`/`DrawTranslucentLine`; retain solid Gizmo geometry and interaction
  ownership in its specialized path.
- [ ] Replace `FOverlayLineRenderer` and `FOverlayIconRenderer` with
  `FSimpleElementRenderer`, then remove their state, shaders, pipeline entries,
  dynamic buffers, depth/output naming helpers, and release paths.
- [ ] Remove `FViewOverlayLine`, `FViewOverlayIcon`, closed icon enums, and
  temporary conversion adapters after targeted search finds no producer or
  test call site; retain only explicitly justified solid primitive values.
- [ ] Run LevelEditor interaction, selection/hover, camera/light/player icon,
  dashed line, collision visualization, draw-order, multi-viewport, reload,
  device-recovery, and representative Vulkan parity coverage after each slice.

#### Acceptance Gate

- Production line, point, sprite, overlay icon, and wire-gizmo producers know
  only PDI vocabulary and resource/style values; none appends legacy overlay
  arrays or selects a renderer/pipeline.
- OverlayLine and OverlayIcon renderer implementations and their duplicated GPU
  lifecycle are deleted while Grid and solid Gizmo retain identical specialized
  behavior.

### Stage 5: Qualify extension boundaries, budgets, documentation, and handoff

- [ ] Prove project/runtime callers can submit supported primitives without an
  editor module dependency and that unsupported raw shader, raw RHI, mesh, and
  post-seal operations fail at the intended boundary.
- [ ] Measure calls, accepted/dropped elements, batches, draws, vertices,
  indices, upload bytes, retained capacity, pipeline variants, allocation time,
  and frame time against Stage 0 without silently raising budgets.
- [ ] Run the smallest registered interface, collector, EditorAssistance,
  LevelEditor, RenderCore, Renderer, RHI, Vulkan, module-lifecycle, and
  application targets selected through repository guidance, followed by the
  required bounded aggregate and build tier.
- [ ] Update Viewport Rendering, Editor Grid boundary, Renderer Resource
  Recovery, Code Modules, and editor visualization contracts; document the
  implemented PDI extension and thread/lifetime rules.
- [ ] Record exact visual, interaction, failure, device, lifecycle, budget,
  build, test, and documentation evidence before completing lifecycle metadata
  and repository-required plan/stage commit provenance.

#### Acceptance Gate

- Source, tests, captures, metrics, diagnostics, and lasting documentation agree
  on one PDI/simple-element architecture with no legacy line/icon submission or
  renderer path.
- Repeated multi-viewport construction, empty/large submissions, reload, retry,
  device invalidation, producer destruction, module retirement, and shutdown
  leave no stale pointer, cross-view element, partial batch, live RHI resource,
  or unbounded retained allocation.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Public API | UE-aligned method/type names, copy ownership, producer-thread assertion, seal behavior, no Renderer/RHI exposure |
| Primitive semantics | Opaque/translucent lines, point size, sprite UV/texture, screen width, dash period, depth bias and invalid inputs |
| Depth and order | World/Foreground behavior, explicit XRay+Visible pair, forward/reversed Z, Grid-first order, stable blend ordering |
| Collection | Deterministic clipping, topology, winding, batching splits, texture grouping, counters, overflow and independent rejection |
| GPU upload | Capacity/pool bounds, geometric growth, exact bytes, allocation failure, retry, full-batch publication and release |
| Shader/pipeline | Global shader refs, complete keys, generation coupling, independent variants, diagnostics, Manual retry and fallback |
| Migration | OverlayLine/Icon and wire Gizmo parity; no legacy arrays/enums/renderers; Grid and solid Gizmo remain specialized |
| Interaction | Selection, hover, dashed guides, collision debug, built-in icons, multi-viewport and producer destruction |
| Rendering | Near-plane clipping, occlusion, pixel sizing, UVs, alpha, Present/Offscreen, forward/reversed depth and Vulkan captures |
| Lifecycle | Texture retention, queued submission lifetime, device invalidation, module retirement, repeated init/shutdown and no leaks |
| Performance | Elements, batches, draws, vertices, indices, upload bytes, retained capacity, pipelines, allocation/frame time versus baseline |
| Documentation | Changed/all validation and all-plan lifecycle validation pass after lasting contracts are updated |

## Definition of Done

- Engine exposes one documented `FPrimitiveDrawInterface` for bounded simple
  line, translucent line, point, and sprite submission with immutable per-view
  ownership and explicit depth/blend semantics.
- Renderer owns one collector and simple-element renderer using Global Shader
  typed refs, shared pipelines, bounded dynamic upload, recovery diagnostics,
  device invalidation, and deterministic release.
- OverlayLine, OverlayIcon, wire Gizmo, and selected debug producers are
  migrated; their legacy view values and renderer implementations are removed.
- Procedural Grid and solid Gizmo remain specialized by explicit design rather
  than hidden compatibility code.
- Focused, aggregate, build, Vulkan, interaction, recovery, lifecycle, budget,
  and documentation gates pass; lasting contracts match source and changes are
  committed with required plan provenance.

## Deferred Follow-ups

- `FMeshBatch`, `FMeshElementCollector`, `FMeshPassProcessor`, vertex factories,
  material proxies, and solid Gizmo migration.
- `FDynamicMeshBuilder` and pooled general dynamic mesh buffers.
- Hit proxies, editor picking IDs, selection outlines, view-mode overrides, and
  depth-priority groups beyond World/Foreground.
- Persistent line batch components, timed debug draws, remote/debug telemetry,
  and cross-frame primitive retention.
- A renderer-wide transient upload allocator shared beyond simple elements.
- A renderer-wide PSO cache and global shader PSO precaching.
- Text, screen-space UI primitives, custom materials, arbitrary texture arrays,
  and user-provided shaders.

## Related Documentation

- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Editor Grid](../Runtime/Rendering/EditorGrid.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Render Resource Lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
- [Renderer Frame Preparation](../Runtime/Rendering/RendererFramePreparation.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Global Shader Framework Plan](GlobalShaderFramework.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Engine/Public/Client/ViewportClient.h`
- `Engine/Source/Runtime/Engine/Public/Client/SceneViewport.h`
- `Engine/Source/Runtime/Engine/Private/Client/SceneViewport.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/EditorAssistance/`
- `Engine/Source/Runtime/Renderer/Private/Renderers/EditorAssistance/EditorAssistanceRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/EditorAssistance/EditorAssistanceRenderer.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/LevelEditorCustomizations.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererEditorAssistanceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridRenderingTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorGridVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportCustomizationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportInteractionTests.cpp`
