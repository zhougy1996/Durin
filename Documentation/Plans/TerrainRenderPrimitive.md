# Terrain Render Primitive Plan

Summary: Add a finite single-LOD terrain Actor/Component and Renderer primitive family that consumes exact heightmap revisions through bounded patch resources, conservative visibility, and the existing PBR passes.

Last reviewed: 2026-08-12

Status: Completed
Completed: 2026-08-12

## Current Status

Stages 0-4 are implemented and focused qualification passes. `ATerrainActor`
and `DTerrainComponent` publish immutable revisioned payload snapshots through
an explicit Terrain proxy/SceneInfo/visibility family. Exact 64x64-cell patches
use shared `UShort2` topology and revision-keyed `R16_UINT` textures; the
Terrain vertex path performs integer loads, signed height reconstruction,
one-sided/central finite differences, stable UVs, and the existing material,
lighting, raster, output, and translucent-ordering policies.

The proposed 4097x4097 ceiling was rejected after its 4,096-draw Debug Vulkan
fixture exceeded 300 seconds on an NVIDIA GeForce GTX 1060 6GB. The frozen T1
ceiling is 1025x1025: 256 patches, 2,097,152 triangles, 2,101,250 height bytes,
and 66,052 shared full-patch topology bytes. With two warm-up and seven measured
17x17 offscreen frames, Debug Vulkan validation recorded CPU preparation median
3096.58 ms / p95 3150.25 ms and Scene Color GPU median 26.7722 ms / p95 37.8943
ms. The 5,000 ms CPU and 50 ms GPU Debug gates pass but establish an expensive
single-LOD baseline that T3 must address.

Focused evidence passes `TerrainRenderPrimitiveTests` (5),
`TerrainRenderVulkanTests` (1), `TerrainHeightmapTests` (6),
`TerrainHeightmapCookTests` (1), `RenderShaderContractTests` (29),
`RendererSceneContractTests` (11), `EditorRenderingTests` (40), and
`EditorAssetWorkflowTests` (80 passed, one skipped). Lasting behavior is in
[Terrain Rendering](../Runtime/Rendering/TerrainRendering.md). The full `all`
build, validation-enabled editor smoke, plan/roadmap validators, and diff check
pass from the same Win64 Debug DurinEditor profile. T1 is complete; T3 receives
the measured 1025x1025 single-LOD baseline and the rejected 4097x4097 evidence.

Implementation keeps the planned `VertexFactory.TerrainVertexFactory` module
but compiles Terrain entry points from `StaticMeshBasePass.slang` under
`DURIN_TERRAIN` instead of duplicating a `TerrainBasePass.slang` fragment
implementation. This preserves one exact material/PBR descriptor ABI and
lighting body while still producing distinct Terrain shader maps and PSOs.

## Goal

Render one finite rectangular `DTerrainHeightmap` as a first-class Terrain
primitive in main, auxiliary, Present, offscreen, fixed-aspect, Lit, Unlit,
Solid, and Wireframe view paths. A reflected Terrain Component supplies world
spacing, height scale/offset, transform, visibility, and one PBR material. The
renderer uploads one exact committed height revision, submits deterministic
single-LOD regular-grid patches with conservative per-patch bounds, shares
resources across consumers, and survives assignment, reimport, transform,
material, viewport, device, removal, and shutdown transitions without reading
reflected objects on the render thread.

This plan establishes the correct single-LOD rendering baseline and measured
patch costs required by T3. It does not solve scalable LOD or crack control.

## Scope

- Reflected `ATerrainActor` and `DTerrainComponent` types with one heightmap,
  positive XY sample spacing, finite Z scale/offset, one material assignment,
  inherited transform/visibility, and explicit render status/diagnostics.
- Detached `FTerrainSceneProxy` values and a typed Terrain SceneInfo family in
  the existing primitive scene, visibility, prepared-view, and mutation paths.
- Renderer-owned, revision-keyed 16-bit height textures and size-keyed reusable
  regular-grid topology resources with complete candidate publication,
  reference counting, pruning, device invalidation, and shutdown release.
- One Terrain vertex factory and base-pass shader using exact grid/sample
  coordinates, height reconstruction, finite-difference normals, stable UVs,
  existing material v3 bindings, shared lighting/environment data, and current
  Opaque/Masked/Translucent pass policy.
- Deterministic single-LOD patch decomposition, exact sample coverage,
  conservative local/world patch bounds from the T0 hierarchy, per-view patch
  frustum classification, stable pass ordering, and conserved counters.
- Explicit heightmap-revision propagation, complete old/new GPU generation
  lifetime, recoverable resource failure, save/reload/Cook/runtime behavior,
  and a first visible editor placement fixture and smoke.
- Focused structural, shader, image, lifecycle, resource, Cook/runtime, and
  bounded performance/memory evidence plus lasting Runtime documentation.

## Non-Goals

- Multi-resolution topology, screen-space error, distance LOD, morphing,
  neighbor resolution, skirts, stitching, geomipmapping, or crack control; T3
  owns those contracts after this single-LOD baseline is measured.
- Heightfield collision, `DBodySetup`, `FBodyInstance` geometry publication,
  World traces/sweeps/overlaps, or Aether resource ownership; T2 owns them and
  must not consume Renderer buffers.
- Sculpting, writable samples, holes, painted layers, splat maps, runtime
  deformation, procedural generation, foliage, roads, water, navmesh, or
  terrain streaming/world partition.
- A dedicated terrain asset editor, thumbnail, brush tools, polished placement
  workflow, specialized viewport picking, or end-to-end editor UX; T4 owns
  those beyond the bounded placement and visibility smoke required here.
- Hardware tessellation, mesh/geometry shaders, compute-generated geometry,
  indirect drawing, GPU culling, bindless resources, virtual textures, or a
  render graph.
- Changing `DTerrainHeightmap` source decoding, sample orientation, hierarchy,
  revision, DDC, Cook, or ownership semantics, or routing height through
  `DTexture2D`.

## Design Decisions and Invariants

### Coordinate and component contract

- Height sample `(X, Y)` becomes component-local position
  `(X * SpacingX, Y * SpacingY, HeightOffset + (Sample / 65535) * HeightScale)`.
  X increases right across a source row, Y increases down source rows and in
  positive local Y, and Z is up. No source flip, transpose, normalization by
  observed range, or half-texel shift is permitted.
- `SpacingX` and `SpacingY` must be finite and strictly positive. `HeightScale`
  and `HeightOffset` must be finite; negative height scale is legal and swaps
  extrema when bounds are formed. The inherited component transform may be
  mirrored; existing determinant-based raster winding policy remains the
  authority. Non-finite transforms take the existing conservative visible
  fallback and never reach a draw with non-finite shader data.
- One sample is one grid vertex. A valid renderable heightmap has at least two
  samples on both axes, so its cell extent is `(Width - 1, Height - 1)` and its
  outermost sample is the exact terrain edge. Adjacent patches share border
  sample coordinates and therefore produce bit-identical positions.
- `DTerrainComponent` persists a `DTerrainHeightmap`, spacing, height scale,
  height offset, and one optional `DMaterialInterface`. Null material resolves
  through the Engine default-material service; invalid material data resolves
  through the established ErrorMaterial path. No component property is copied
  from or written into the heightmap asset.

### Patch decomposition and bounds

- The initial topology candidate is 64x64 cells with 65x65 grid vertices,
  aligning interior patch queries with T0's 64-sample regional hierarchy while
  retaining the extra shared border sample. Stage 0 must freeze this value
  against draw, triangle, buffer, and bounds-query evidence before code relies
  on it.
- Patch origins are enumerated Y-major then X-major in cell space. Interior
  patches cover the frozen cell size; right/bottom patches cover the exact
  remaining cells. Topology is cached by exact `(CellCountX, CellCountY)`, so
  edge patches do not add triangles outside the authored extent or depend on
  degenerate clamping.
- Each patch local bound uses its inclusive vertex sample rectangle, queried as
  the corresponding half-open T0 sample rectangle. Normal reconstruction may
  read one clamped neighboring sample but cannot expand the geometry extent.
  Height extrema are converted with scale/offset in both signs before the
  eight local bound corners are transformed by the existing SceneInfo path.
- Component bounds conservatively enclose all patch bounds and are the first
  primitive-level visibility gate. Visible Terrain primitives then classify
  every patch against the same immutable fitted view. Invalid patch bounds or
  view inputs remain conservatively visible with named counters; a finite
  outside patch never reaches resource preparation or drawing.
- T1 has exactly one geometry resolution. Every visible patch uses full sample
  density, including at edges. Patch identity and ordering contain only copied
  primitive ID, patch coordinates, material/pass identity, and stable geometry
  facts; pointer addresses and container iteration never break ties.

### Renderer ownership and revision synchronization

- The game thread captures one `shared_ptr<const FTerrainHeightmapPayload>` and
  asset revision while constructing `FTerrainSceneProxy`. The proxy contains
  immutable payload ownership, copied dimensions/extrema/spacing/scale/offset,
  copied patch descriptors/bounds, and counted material proxies; it contains no
  Actor, Component, `DTerrainHeightmap`, reflected-property, source, DDC, or
  mutable sample pointer.
- Renderer owns a cache of height resources keyed by immutable payload identity
  plus dimensions and upload contract. Cache entries retain the snapshot only
  through complete upload/publication and recorded GPU lifetime, then retain
  the published texture and immutable identity facts. Multiple components using
  the same payload revision share one height texture; different revisions never
  mutate an entry in place or mix CPU/GPU generations.
- Terrain topology is renderer-owned and shared by exact cell dimensions.
  Vertex data contains only integer grid coordinates and index data contains
  deterministic two-triangle cells with the frozen winding. No per-component
  expanded XYZ/normal/UV vertex copy is built or retained.
- Height and topology candidates validate descriptions, checked byte counts,
  source spans, RHI capability, texture/view/buffer creation, and vertex-factory
  readiness before atomic cache publication. A failed candidate leaves the
  previous complete revision usable, records one bounded reason, and is not
  retried every frame in the same device generation.
- Heightmap assignment or committed revision change recreates the component
  proxy through the existing ordered render-state path. Old proxies, snapshots,
  textures, topology, material proxies, and recorded commands remain alive
  until their counted/fenced owners retire. No whole-scene scan, render-thread
  DObject lookup, global flush, or device-idle wait supplies correctness.
- Device invalidation releases all Terrain RHI objects before generation retry.
  Renderer shutdown stops admission, drains recorded work through existing
  ownership, releases Terrain caches and shader resources, and leaves no
  Terrain-specific global pointer or callback into destroyed Engine objects.

### GPU format, vertex factory, and surface contract

- The preferred exact upload is one mip of sampled `R16_UINT`; the vertex shader
  performs an integer texel load and divides by 65535. Stage 0 must prove the
  complete backend-neutral creation, binding, shader reflection, and load path.
  If it is unavailable, the plan must record and qualify an exact alternative
  before Stage 1; silent conversion through RGBA8, BC, sRGB, observed-range
  normalization, or `R16_FLOAT` is forbidden.
- `FTerrainVertexFactory` owns the compact integer-grid declaration and stream
  binding. `VertexFactory.TerrainVertexFactory` owns grid-coordinate decode;
  `TerrainBasePass.slang` owns height fetch, local/world transform, normal/UV
  construction, material evaluation, pass entry points, and lighting.
- Height sampling uses integer texel coordinates. Central differences use
  adjacent authored samples; terrain edges use the frozen one-sided/clamped
  rule selected in Stage 0. Normals account for XY spacing and signed height
  scale, are finite for uniform/extreme fixtures, and transform through the
  established normal matrix. UV0 is local sample coordinate divided by
  `(Width - 1, Height - 1)` before material UV transforms.
- Terrain consumes the exact material render v3/v2 compatibility boundary,
  default textures, direct/environment lighting, Lit/Unlit modes, Opaque,
  Masked, Translucent, two-sided, depth-write, wireframe, and mirrored-winding
  policies already owned by Renderer. It does not fork material GUID lookup,
  texture fallbacks, lighting equations, or pass-state interpretation.
- Terrain may own shader maps and PSOs for its vertex input, but shares material
  representation decoding and effective raster/depth/blend construction.
  Unsupported layouts resolve to ErrorMaterial before draw preparation exactly
  as for StaticMesh.

### Finite budgets, diagnostics, and rollout

- Stage 0 freezes a representative 1025x1025 sample fixture, an odd/non-square
  edge fixture, the largest T1-renderable extent, patch size, worst-case visible
  patch/draw/triangle counts, CPU proxy bytes, shared topology bytes, R16 upload
  bytes, peak publication bytes, and named adapter/view settings. The T0 asset
  maximum is not implicitly the T1 render maximum.
- Candidates begin with a 64x64-cell patch. Stage 0 measurement lowered the
  T1 ceiling from the proposed 4097x4097 samples to 1025x1025; raising
  it requires recorded upload, submission, frame, and retained-memory evidence.
  An asset outside the frozen render ceiling remains a valid asset but produces
  a named Terrain render rejection rather than truncation or downsampling.
- Counters conserve submitted/hidden/primitive-culled/visible Terrain
  primitives, patch candidates, patch-culled/visible/invalid-bound fallbacks,
  resource attempts/success/rejection, pass-classified patches, triangles,
  draws, height uploads/reuses/bytes, topology creations/reuses/bytes, and
  revision replacements. Counters are command-local snapshots, not mutable
  scene policy.
- Correctness tests do not assert wall-clock or GPU-time thresholds. Stage 4
  records named Debug/Shipping profile, adapter/driver, resolution, warm-up,
  sample count, median/p95 CPU preparation and GPU interval when supported,
  plus actual draw/triangle/resource bytes. These measurements establish the
  single-LOD baseline for T3 and do not claim scalable terrain performance.

## Current Foundations and Gaps

| Area | Existing foundation | T1 gap |
| --- | --- | --- |
| Height authority | Exact immutable `FTerrainHeightmapPayload`, revision, dimensions, samples, global and regional extrema | No render snapshot, upload identity, GPU format, or revision propagation |
| Engine primitive | `DPrimitiveComponent`, render-state mutation, Actor/component serialization, material services | No Terrain Actor/Component, properties, status, proxy, or assignment validation |
| Scene ownership | Stable primitive ID, detached proxy, SceneInfo, typed StaticMesh/SkeletalMesh lists | No Terrain proxy kind, typed accessor/list, mutation tests, or editor-observer family |
| Visibility | One centralized primitive classification and conservative fallback per view | No patch decomposition, patch bounds/classification, Terrain counters, or prepared work |
| RHI resources | Sampled R16 formats, integer vertex elements, uploads, indexed drawing, recorded lifetime | No exact height texture, shared grid topology, terrain vertex factory, or cache lifecycle |
| Materials and passes | Material v3/v2 decoding, fallbacks, PBR lighting, Opaque/Masked/Translucent, wireframe and mirrored winding | No Terrain shader entry points, normals/UVs, PSOs, pass buckets, or combined translucent ordering |
| Editor/runtime | Reflected details, actor spawning, viewport paths, cooked heightmap loading | No bounded Terrain placement fixture, visible saved/cooked level, or reimport smoke |
| Diagnostics | View counters, resource-coordinator diagnostics, validation-enabled runtime | No Terrain conservation, revision, upload, topology, rejection, or timing facts |

## Implementation Stages

### Stage 0: Freeze coordinate, resource, and qualification contracts

- [x] Build independent asymmetric height fixtures that distinguish source X/Y,
  local X/Y, UV orientation, triangle winding, normal sign, min/max bounds, and
  signed height scale; include uniform, extreme, odd, non-square, and mirrored
  transform cases without using production terrain code for golden values.
- [x] Verify backend-neutral sampled `R16_UINT` creation, upload, integer texel
  load, reflection, state transitions, and recorded lifetime on both command
  executors; select and document an exact alternative or stop if any required
  path cannot preserve all 16-bit values.
- [x] Measure 32/64/128-cell topology candidates and representative 1025x1025
  preparation/submission on the named adapter. Freeze patch size, T1 maximum
  render extent, topology index width, edge topology policy, CPU/GPU/peak byte
  ceilings, draw/triangle ceilings, and the Stage 4 timing protocol.
- [x] Freeze local position, UV, normal edge rule, winding, signed height scale,
  mirrored transform, component/property validation, primitive and patch bound,
  invalid-data fallback, material, translucent ordering, and editor observation
  contracts with exact golden facts.
- [x] Inventory the smallest existing StaticMesh/SkeletalMesh seams to reuse for
  proxy/info typing, visibility, prepared views, material binding, resource
  coordination, device invalidation, counters, editor placement, Cook/runtime,
  and shutdown; record every place requiring an explicit Terrain case.
- [x] Record baseline focused Engine/Renderer/RHI/editor tests, documentation
  validation, representative memory/timing evidence, selected constants, and
  the Stage 0 handoff before adding production Terrain types.

#### Acceptance Gate

- Exact sample-to-position/UV/normal mapping, patch coverage and ordering,
  upload format, ownership, bounds, failure behavior, limits, fixtures, and
  measurement protocol are frozen with no unresolved precision, extent, RHI,
  or threading decision entering implementation.

### Stage 1: Add Terrain Engine types and typed scene ownership

- [x] Add reflected `ATerrainActor` and `DTerrainComponent` files, module/reflection
  membership, forward declarations, default subobject ownership, editable
  heightmap/spacing/scale/offset/material properties, validation, PostLoad, and
  complete assignment APIs.
- [x] Compute deterministic patch descriptors and exact conservative local
  primitive/patch bounds from one immutable payload snapshot without opening
  source, copying the complete sample plane, or publishing partial state.
- [x] Add `FTerrainSceneProxy`, `EPrimitiveSceneProxyKind::Terrain`, typed
  SceneInfo access, one authoritative Terrain SceneInfo list, visibility result,
  prepared-view slot, and editor primitive-family observation where required
  for bounded placement visibility.
- [x] Route registration, transform, owner visibility, heightmap assignment,
  spacing/scale/offset, material binding, unregister, replacement, and retirement
  through existing ordered render-state mutation with stable primitive identity.
- [x] Add focused serialization, property-validation, coordinate/bounds,
  proxy-detachment, typed-membership, mutation-order, material-binding,
  duplicate, save/reload, and destruction tests.
- [x] Record Stage 1 handoff and focused validation evidence.

#### Acceptance Gate

- A saved Terrain Actor recreates one detached proxy whose copied snapshot,
  revision, properties, material, patch descriptors, bounds, and typed scene
  membership are exact; no render-thread path can reach a reflected object or
  stale/partially replaced generation.

### Stage 2: Build exact shared GPU resources and Terrain shaders

- [x] Implement the renderer-owned revision-keyed height cache with checked
  `R16_UINT` upload, sampled view, state transitions, complete candidate
  publication, reference/pruning policy, diagnostics, and exact readback or
  shader-fixture validation.
- [x] Implement size-keyed reusable edge-aware topology buffers and
  `FTerrainVertexFactory` using compact integer grid coordinates, deterministic
  uint16/uint32 index selection from frozen limits, validated winding, complete
  initialization rollback, and reverse-order release.
- [x] Add `VertexFactory.TerrainVertexFactory` and `TerrainBasePass.slang` with
  integer height fetch, signed world-height reconstruction, frozen edge normals,
  stable UV0, normal transform, material v3/v2 binding, Lit/Unlit PBR lighting,
  and Opaque/Masked/Translucent entry points.
- [x] Add Terrain shader-map/PSO caches through the resource coordinator,
  established effective material pipeline identities, default/Error material
  and texture fallbacks, generation-scoped retry, manual refresh, shader reload,
  and device invalidation without backend-specific code.
- [x] Prove uploaded sample corners/interiors, topology indices, vertex positions,
  normals, UVs, winding, material constants/textures, and resource byte counts
  with focused RHI/Renderer tests and deterministic offscreen fixtures.
- [x] Record Stage 2 handoff, format/topology golden identities, and focused
  validation evidence.

#### Acceptance Gate

- One patch renders exact full-resolution authored geometry through shared
  portable resources and existing PBR semantics; malformed, unsupported, or
  failed candidates publish nothing, preserve prior complete generations, and
  report a bounded retry-safe reason.

### Stage 3: Integrate patch visibility, passes, and view execution

- [x] Implement `FTerrainRenderer` preparation/execution and command-local
  Terrain prepared values; consume only the centralized visible Terrain
  SceneInfo list and classify every patch once against the fitted immutable view.
- [x] Build deterministic Opaque/Masked/Translucent buckets, state/sort keys,
  combined translucent ordering, resource preparation, indexed draws, and
  exact edge index counts using existing pass, lighting, output, and editor-
  assistance ordering.
- [x] Extend view counters so primitive, patch, pass, triangle, resource, draw,
  upload, topology, fallback, and rejection totals conserve at every preparation
  and execution phase; expose bounded snapshots without persistent view state.
- [x] Validate fully inside/outside/intersecting patches, boundary planes,
  invalid bounds/view fallbacks, hidden primitives, odd edges, non-square
  extents, mirrored transforms, signed height scale, and sequential unrelated
  views with structural and image fixtures.
- [x] Qualify Lit/Unlit, Solid/Wireframe, Opaque/Masked/Translucent, two-sided,
  depth policy, direct/environment lighting, main/auxiliary, Present/offscreen,
  fixed-aspect, and editor-assistance paths without a parallel frame renderer.
- [x] Record Stage 3 handoff, conserved counter examples, image references, and
  focused validation evidence.

#### Acceptance Gate

- Every visible authored cell is covered exactly once by deterministic
  full-resolution patches; finite outside work issues no draw, invalid inputs
  stay conservatively visible, all material/view paths match their reference,
  and counters reconcile submission through successful or rejected execution.

### Stage 4: Qualify lifecycle, editor/runtime use, and the T1 baseline

- [x] Propagate committed heightmap revision changes to loaded Terrain
  Components with coalesced complete proxy replacement; prove identical
  no-op reimport, changed reimport, rapid assignment/property edits, save
  failure, missing/failed asset, and last-known-good resource behavior.
- [x] Validate multiple components sharing one revision, components using
  different revisions, proxy removal during recorded work, viewport resize,
  shader reload, manual retry, device invalidation, level unload, DObject
  destruction, editor shutdown, and Engine shutdown with balanced resources.
- [x] Add the bounded editor placement fixture and smoke: create/assign a
  Terrain Actor through supported reflected/authoring paths, inspect properties,
  save/reload, render in the scene viewport, reimport, Cook, and launch the
  cooked runtime without source or DDC. Leave polished placement and picking UX
  to T4.
- [x] Record representative and frozen-maximum CPU preparation, GPU interval,
  upload, retained/peak memory, patch/draw/triangle, cache reuse, and invalidation
  evidence using the Stage 0 protocol. State clearly that T1 is a single-LOD
  baseline and feed measured costs into T3 entry.
- [x] Run the smallest affected native targets, required cross-target coverage,
  documentation validation, and—because Terrain is user-visible—the full `all`
  Agent build and validation-enabled editor smoke from one Agent Build Profile.
- [x] Publish lasting Terrain component, proxy/resource, coordinate, shader,
  material, visibility, lifecycle, failure, and diagnostic contracts under
  `Documentation/Runtime/Rendering/`; update the Heightfield Terrain roadmap
  T1 status and precise T3/T4 entry state; record final source revision,
  executable, evidence, decisions, and open follow-ups in the handoff.

#### Acceptance Gate

- A user-visible finite Terrain Actor saves, reloads, renders, reimports, Cooks,
  and runs from cooked data with correct 16-bit-derived geometry, conservative
  patch visibility, existing PBR/output semantics, clean revision/resource
  lifetime, reconciled counters, bounded diagnostics, and no source/DDC need.
- The representative and maximum T1 fixtures remain within frozen CPU/GPU,
  draw/triangle, upload, preparation, and frame budgets. T3 receives a measured
  correct single-LOD baseline rather than an implicit scalability claim.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Asymmetric heightmap | Corners/interiors map to exact local X/Y/Z, UV, winding, normal sign, and rendered orientation | Engine builder and shader/image tests |
| Uniform and extremes | Zero/65535, equal extrema, flat normals, negative height scale, and finite bounds remain exact | Component/proxy and shader tests |
| Patch coverage | Interior and odd right/bottom edges cover every cell once, share border vertices, and never exceed authored extent | Patch/topology tests |
| Primitive and patch culling | Hidden, outside, inside, intersecting, invalid-bound/view, and culling-disabled classifications conserve | Visibility and counter tests |
| Exact GPU height | 16-bit upload/readback or shader fixture preserves selected golden values without texture color policy | RHI/Renderer resource tests |
| Materials and passes | Default/Error, v3/v2, constants/textures, Opaque/Masked/Translucent, two-sided/depth, Lit/Unlit and wireframe match | Renderer structural/image tests |
| View compatibility | Main, auxiliary, Present, offscreen, fixed-aspect, editor assistance, and sequential unrelated views remain isolated | Renderer/editor integration tests |
| Revision transaction | No-op reimport changes nothing; changed revision publishes once; failures retain the prior complete visible generation | Engine/Renderer lifecycle tests |
| Resource sharing | Same payload revision and topology share resources; different revisions never alias; pruning and counters balance | Renderer cache tests |
| Device and shutdown | Resize/reload/retry/invalidation/removal/unload/shutdown leave no stale command, proxy, cache entry, or RHI resource | Renderer runtime tests |
| Cooked runtime | Saved Terrain level loads and renders the same geometry without height source or DDC | Cook/runtime process test |
| Finite budgets | Representative and maximum T1 fixtures stay within frozen CPU/GPU/peak bytes, draws, triangles, and timing protocol | Characterization fixture and final handoff |

## Definition of Done

- `ATerrainActor` and `DTerrainComponent` are reflected Engine types with one
  validated world interpretation of immutable `DTerrainHeightmap` revisions.
- Terrain is an explicit proxy/SceneInfo/visibility/prepared-view/render family;
  render-thread code owns detached values and reads no reflected object.
- Exact 16-bit height resources and edge-aware grid topology are shared,
  bounded, generation-safe, retry-safe, and released through existing Renderer
  lifecycle contracts.
- Full-resolution patches render correct positions, normals, UVs, winding,
  material passes, lighting, output paths, and conservative bounds with
  deterministic ordering and reconciled counters.
- Assignment, reimport, save/reload, Cook/runtime, device invalidation, removal,
  and shutdown preserve complete revision and resource lifetime semantics.
- Focused tests, required aggregate validation, full build, editor smoke,
  lasting documentation, roadmap status, and measured T3 baseline all pass.

## Deferred Follow-ups

- Screen-space error, patch LOD selection, adjacency resolution, skirts or
  stitched topology, temporal stability, and crack-free transitions (T3).
- Aether HeightField geometry, cell acceleration, query algorithms, Cook/load,
  BodySetup/BodyInstance publication, and collision debug evidence (T2).
- Polished placement, specialized picking, details customization, diagnostics
  presentation, undo/redo qualification, and end-to-end editor workflow (T4).
- Streaming/residency, world partition, origin rebasing, virtual textures,
  foliage, navmesh, and other world-scale consumers behind measured activation.
- Writable height regions, sculpting, holes, painted layers, procedural
  generation, runtime deformation, and network replication.

## Related Documentation

- [Heightfield Terrain Roadmap](../Roadmaps/HeightfieldTerrain.md)
- [Terrain Heightmap Asset](../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Rendering Capability Expansion](../Roadmaps/RenderingCapabilityExpansion.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmap.h`
- `Engine/Source/Runtime/Engine/Public/Actors/TerrainActor.h`
- `Engine/Source/Runtime/Engine/Public/Components/TerrainComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/StaticMeshComponent.h`
- `Engine/Source/Runtime/Engine/Public/Engine/FPrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/LocalVertexFactory.h`
- `Engine/Source/Runtime/RenderCore/Public/Terrain/TerrainVertexFactory.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Shaders/Slang/VertexFactory/LocalVertexFactory.slang`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Tests/Native/EngineTests`
