# Terrain Patch LOD Plan

Summary: Add deterministic per-view terrain patch LOD, adjacency resolution, crack-free stitched topology, and bounded rendering qualification to the finite Terrain primitive.

Last reviewed: 2026-08-13

Status: Archived
Completed: 2026-08-13

## Current Status

T3 is complete. Terrain proxies now own revision-bound, 64-KiB-bounded patch
LOD metadata: stable grid coordinates, exact legal power-of-two steps, and
monotonic maximum bilinear-reconstruction Z errors. Each camera or shadow view
selects from a strict two-pixel error threshold, resolves the complete patch
grid by deterministic coarse-neighbor promotion, and stores the requested and
resolved LOD, sample step, N/E/S/W stitch mask, and exact triangle count in the
prepared draw.

The Renderer shares immutable index-only topology by the complete
`(CellCountX, CellCountY, LODStep, StitchMask)` key. The CPU oracle covered six
stitchable steps times all 16 masks, proved continuous exact boundary coverage,
positive nonzero area, exact total area, and no outside coordinate, and also
proved partial-edge key rejection. Vulkan executed all 16 masks, mixed camera
LOD, independent directional-shadow draws, topology separation/reuse, and the
disabled-by-default diagnostic overlay. The exhaustive sample-space oracle is
the golden topology authority; per-mask driver-specific raster goldens were
replaced by offscreen Vulkan execution because exact coverage and winding are
proved before rasterization while backend execution still covers bindings and
lifetime.

On the Win64 Debug validation profile available for this handoff (NVIDIA
GeForce RTX 3090), 1025x1025 `ForceLOD0` retained 256 draws and 2,097,152
triangles at 1567.04 ms CPU median / 2100.17 ms p95 and 1.85901 ms Scene Color
GPU median / 3.39014 ms p95. The automatic flat far oracle retained the same
256 patches/draws but selected step 64 and 512 triangles, with one measured
1547.17 ms CPU preparation. Both stay below the inherited 5000 ms CPU and 50
ms GPU gates; draw-call scalability remains deferred rather than being claimed
as an LOD benefit.

Focused Terrain contract/Vulkan tests, the Terrain domain including
source/DDC-free Cooked Runtime proxy creation, the explicit qualification
target, the ordinary native aggregate, full Debug Editor and Game builds, and
normal-exit hidden-window Editor/Game smokes passed. Lasting behavior is owned
by [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md); T4 may consume
the stable counters and overlay without redefining T3.

### Stage 0/1 semantics and foundation handoff (2026-08-13)

- Legal steps are `1, 2, 4, ...` while both exact patch cell dimensions remain
  divisible; the largest full-patch step is 64 and an unrepresentable partial
  step is absent rather than clamped.
- Each step's error is the maximum absolute Z difference between canonical
  samples and bilinear reconstruction from that step, monotonically accumulated
  in `double`. Proxy-retained LOD vector objects and scalar storage are capped at
  64 KiB.
- A level is selected only when projected error is strictly less than two
  pixels. Equality, `ForceLOD0`, and invalid inputs choose step one.
- Stitch bits are `N/E/S/W = 1/2/4/8`; index-only collapse won over skirts
  because it retains authored height vertices, bounds, silhouette, shadow
  extent, and collision isolation.

### Stage 2/3 selection and rendering handoff (2026-08-13)

- Stable Y-major east/south sweeps promote only the coarser index. The finite
  work bound is the sum of patch LOD counts plus the final no-change pass; every
  resulting neighbor step is equal or exactly 2:1.
- `requested histogram sum = resolved histogram sum = patch candidates` and
  `visible + culled = candidates`; mask and selected-triangle totals cover only
  prepared visible draws, while resource and execution attempts each conserve
  successes plus rejections.
- The prepared draw is authoritative for topology key, triangle count, material
  pass, sort, resource lookup, and execution. Directional shadows invoke the
  same preparation independently. Invalid topology publishes no cache entry.
- The overlay is opt-in and transient. Ordinary view state remains unchanged
  when disabled, and no expanded terrain surface is retained.

### Final T3 validation handoff (2026-08-13)

- CPU topology coverage: steps 1/2/4/8/16/32 across all 16 masks, plus step 64,
  partial dimensions, exact boundary intervals, positive winding, nonzero
  triangles, total area, and index/extent checks.
- Vulkan coverage: exact `ForceLOD0`, automatic flat decimation, mixed 2:1
  camera output, all 16 center-patch masks, complete topology-key reuse and
  separation, diagnostic overlay, directional-shadow draws, invalidation, and
  shutdown.
- Validation profile and performance values are recorded above. The final task
  commit owns the implementation revision; the repository test registry and
  command logs own executable counts and per-run details.

## Goal

Render finite Terrain with deterministic, view-local patch LOD whose selected
surface stays conservative, whose adjacent patches cannot expose a crack, and
whose CPU, GPU, draw, triangle, topology-memory, and lifecycle costs satisfy
frozen 1025x1025 quality and performance gates.

## Scope

- Per-patch geometric-error metadata derived from the committed height payload.
- Perspective and orthographic per-view LOD requests with explicit invalid-view
  fallback and `ForceLOD0` comparison behavior.
- Deterministic adjacency resolution with a maximum selected neighbor delta of
  one LOD step.
- Index-only 2:1 edge stitching for all legal edge-mask and boundary-patch
  combinations.
- Shared topology caching keyed by exact patch dimensions, LOD step, and stitch
  mask; no per-component GPU topology copies.
- Camera and directional-shadow view preparation, materials, solid/wireframe
  rendering, translucent sorting, counters, diagnostics, and lifecycle.
- Structural, geometry, image, motion, performance, memory, Cook/Game, and
  editor-runtime qualification at the existing 1025x1025 render ceiling.

## Non-Goals

- Terrain streaming, world partition, origin rebasing, asynchronous residency,
  or raising the 1025x1025 component ceiling.
- Runtime height edits, sculpting, painted layers, holes, foliage, navmesh,
  water, roads, or procedural terrain.
- GPU tessellation, mesh shaders, compute-generated topology, indirect draws,
  occlusion/HZB, or GPU-selected LOD.
- Temporal morphing, geomorph blends, dithered transitions, or a process-global
  camera/LOD setting.
- Changing HeightField collision, collision revision identity, World queries,
  or deriving collision from selected render LOD.
- User-facing terrain editor workflow beyond the diagnostics and comparison
  controls required to qualify T3; polished workflow remains T4.

## Design Decisions and Invariants

### LOD remains detached, deterministic, and view-local

- The immutable height payload remains the only height authority. Proxy-owned
  patch error and adjacency metadata are derived values tied to its revision;
  render preparation never reads a Component, Actor, collision resource, or
  mutable editor state.
- Each patch has stable Y-major identity. Initial LOD requests depend only on
  copied patch facts and the submitted `FSceneView`; adjacency resolution uses
  stable patch coordinates and value ordering, never pointer or container
  iteration order.
- Automatic selection uses a conservative screen-space projection of a frozen
  object-space geometric error. Equality selects the finer LOD. Invalid view,
  transform, error, or projection data falls back to LOD 0, stays visible, and
  increments a named bounded diagnostic.
- `ForceLOD0` selects step 1 for every visible patch before adjacency and is a
  qualified single-LOD oracle. Camera and shadow views select independently;
  one view never mutates proxy state or another view's result.
- T3 is stateless across frames. Any future hysteresis or morphing requires its
  own temporal ownership, multi-view policy, motion fixtures, and measured
  benefit rather than entering this plan implicitly.

### Adjacency resolution preserves quality

- Legal LODs form nested power-of-two sample coordinates and always retain the
  exact authored patch corners and world boundary. Stage 0 freezes the legal
  step set for partial right/bottom patches; a step that cannot represent the
  exact boundary is not selectable.
- Neighbor resolution enforces an LOD-index difference of at most one by
  promoting the coarser patch toward the finer patch. It never demotes a
  requested patch, so resolution cannot add geometric error.
- Resolution reaches a deterministic fixed point in bounded passes over the
  rectangular patch grid. Missing neighbors at authored world edges are not
  synthesized. Culled neighbors still contribute their requested/resolved LOD
  where needed to keep a subsequently visible shared boundary coherent.
- The same resolved patch result supplies resource preparation, pass lists,
  triangle accounting, draw sorting, and execution. No later phase silently
  reselects a different LOD.

### Cracks are removed with stitched indices

- T3 uses index-only edge stitching, not skirts. Rendered vertices therefore
  remain exact authored samples and no vertical wall extends the heightfield
  surface or changes its silhouette, bounds, picking, or shadow extent.
- A fine patch adjoining a patch exactly one LOD coarser uses a four-bit
  north/east/south/west stitch mask. Its boundary topology connects only to
  coordinates present on the coarse edge. All 16 masks, corner interactions,
  partial patches, negative height scale, mirrored transforms, and world edges
  must be structurally and visually qualified.
- Every generated index is checked, winding remains the T1 `(A,B,C)/(B,D,C)`
  convention away from transitions, triangles have nonzero sample-space area,
  and a patch never emits a coordinate outside its exact cell rectangle.
- Topology resources are immutable and shared by the complete value key
  `(CellCountX, CellCountY, LODStep, StitchMask)`. Creation failure rejects only
  affected draws with named counters and cannot damage an existing cache entry.

### Bounds, shading, and other consumers stay stable

- Patch visibility continues to use exact full-resolution regional min/max
  bounds. LOD selection or stitching may not shrink those bounds; invalid
  bounds retain the existing visible fallback.
- Height sampling, UVs, normal derivation, material passes, lighting,
  determinant-based winding, translucent order, directional shadows, output
  layouts, and device lifetime remain shared Renderer policy. Decimation never
  creates a second height texture or CPU-expanded vertex surface.
- Collision continues to use its independent immutable HeightField at full
  authored resolution. Render LOD and stitch masks are absent from collision
  identity, body publication, Cook data, and query diagnostics.
- Counters conserve candidates, visibility, requested/resolved LOD histograms,
  adjacency promotions, stitch masks, triangles, resource attempts/results,
  draws, topology cache entries/bytes, and fallbacks with saturating arithmetic.

## Current Foundations and Gaps

| Area | Existing foundation | T3 gap |
| --- | --- | --- |
| Height authority | Immutable exact 16-bit payload, revision, regional min/max hierarchy | No per-patch/per-LOD geometric-error facts |
| Patch layout | Stable Y-major 64x64-cell maximum descriptors with exact partial edges | No legal nested LOD step set or adjacency coordinates |
| View policy | Perspective/orthographic projected-size math and submission-local `Automatic`/`ForceLOD0` modes | No terrain error projection, threshold policy, or invalid-input LOD fallback |
| Visibility | Conservative primitive and patch frustum classification | Visibility result has no resolved neighbor context or LOD facts |
| Topology | Shared exact-dimension full-density index buffers | No decimated or edge-stitched variants and no complete LOD/stitch cache key |
| Draw preparation | Camera/shadow pass lists, materials, sorting, resources, execution | Draws do not retain requested/resolved LOD, step, stitch mask, or exact triangle count |
| Diagnostics | Patch/draw/triangle/upload/topology counters and Debug timing baseline | No LOD histograms, promotions, stitch distribution, fallback facts, or LOD overlay |
| Qualification | CPU fixtures, 17x17 Vulkan images, 1025x1025 Debug baseline, Cook/Game smoke | No transition topology/image/motion matrix or LOD performance comparison |

## Implementation Stages

### Stage 0: Freeze LOD semantics, fixtures, and budgets

- [x] Build asymmetric flat, ramp, ridge, saddle, spike, checker, extreme,
  negative-height-scale, mirrored-transform, non-square, and partial-edge
  fixtures. Include every neighbor direction, all 16 stitch masks, all legal
  LOD pairs, authored world edges, and perspective/orthographic views.
- [x] Implement a test-only CPU surface oracle that expands selected patch
  coordinates and stitched indices, then verifies shared-edge coverage,
  winding, nondegeneracy, extent, and maximum deviation from LOD 0.
- [x] Measure candidate geometric-error definitions against the exact T1
  surface and select one conservative error representation, construction
  algorithm, scalar precision, identity/lifetime boundary, and byte ceiling.
- [x] Freeze legal power-of-two steps for full and partial patches, screen-error
  thresholds, equality/fallback rules, maximum LOD count, adjacency pass bound,
  and the exact edge-mask convention. Record why every rejected partial step is
  unrepresentable rather than silently clamping it.
- [x] Prototype and structurally compare stitched patterns against skirts at
  corners, silhouettes, shadows, and partial edges; retain the selected
  index-only stitching path unless evidence disproves an invariant above.
- [x] Characterize the retained T1 near/mixed/grazing fixtures plus 1025x1025
  `ForceLOD0` and flat-far views on the available validation adapter. Freeze
  CPU preparation, Scene Color GPU, draws, triangles, topology entries/bytes,
  proxy-build time, and retained CPU memory gates before production integration.
- [x] Add failing pure-math, proxy-metadata, selection, adjacency, topology,
  counter, Vulkan image, lifecycle, and performance tests. Record the Stage 0
  handoff in this plan before production resource schemas are changed.

#### Acceptance Gate

- One written handoff freezes the exact error metric, thresholds, legal steps,
  partial-edge rules, adjacency algorithm, mask convention, topology key,
  diagnostic equations, fixtures, and finite budgets.
- The CPU oracle proves the selected stitching scheme can cover every legal
  2:1 edge and corner combination without a gap, outside-extent triangle, or
  altered height vertex.
- Frozen mixed/far views require materially fewer triangles than `ForceLOD0`;
  any accepted CPU/GPU regression has a named cause and bounded budget rather
  than an unmeasured scalability claim.

### Stage 1: Build immutable patch error and topology foundations

Dependencies: Stage 0 semantics, fixtures, and budgets.

- [x] Extend detached patch/proxy values with stable grid coordinates, legal
  LOD facts, frozen geometric errors, and checked byte accounting tied to the
  exact payload revision.
- [x] Build error metadata transactionally from canonical samples at proxy
  creation. Reject overflow, non-finite conversion, invalid monotonicity,
  unsupported dimensions, or budget excess without publishing a partial proxy.
- [x] Add pure selection helpers for error projection, threshold equality,
  `ForceLOD0`, invalid-input fallback, and legal-step resolution across
  perspective and orthographic views.
- [x] Generate immutable topology for exact dimensions, LOD step, and stitch
  mask with checked counts/indices and stable identity; extend the bounded
  topology cache without weakening complete-entry or invalidation guarantees.
- [x] Prove topology reuse across components/views, separation across distinct
  keys, failed-creation recovery, recorded-command retention, device
  invalidation, and shutdown release.
- [x] Validate error bounds and every topology structurally against the Stage 0
  oracle before connecting selection to scene rendering; record the Stage 1
  handoff and measured build/retained costs.

#### Acceptance Gate

- A proxy revision owns complete immutable error facts or no render proxy, and
  its retained/peak costs stay within the Stage 0 gates.
- Every legal topology key produces deterministic in-range nondegenerate
  triangles with exact extent and edge coverage; illegal keys publish nothing.
- Existing height upload identity, full-density topology behavior, component
  revision transactions, and resource lifetime tests remain unchanged.

### Stage 2: Select and resolve deterministic per-view patch LOD

Dependencies: Stage 1 immutable metadata and pure selection helpers.

- [x] Compute one requested LOD per candidate patch from its geometric error
  and submitted view, preserving explicit LOD-0 fallback diagnostics.
- [x] Resolve the complete rectangular adjacency graph by deterministic
  coarse-neighbor promotion until every pair differs by at most one; enforce
  the frozen pass/work bound and reject impossible results safely.
- [x] Derive each visible patch's stitch mask from resolved neighbors and store
  requested/resolved indices, step, mask, exact triangle count, and stable sort
  values in `FPreparedTerrainDraw`.
- [x] Extend view counters with requested/resolved histograms, promotions,
  iterations, masks, fallbacks, selected triangles, and conservation equations.
  Keep capture/reset O(1) in scene size beyond existing prepared values.
- [x] Qualify repeated and permuted scene preparation, camera threshold
  equality, subpixel motion, near-plane crossing, orthographic zoom, invalid
  transforms/views, culled neighbors, multiple terrains, and independent
  camera/shadow views.
- [x] Record the Stage 2 handoff with selection equations, worst-case adjacency
  work, fixture results, and counter examples.

#### Acceptance Gate

- Identical proxy/view values always produce identical requested/resolved LODs,
  masks, ordering, triangle counts, and counters independent of prior frames or
  container addresses.
- Every resolved neighbor pair satisfies the frozen delta and stitching rule;
  resolution never lowers requested detail and terminates within its work gate.
- `ForceLOD0` exactly reproduces T1 visible patches, topology, triangles,
  materials, sorting, and fallback behavior.

### Stage 3: Integrate stitched LOD through all Terrain render paths

Dependencies: Stage 2 complete prepared-draw results and Stage 1 resources.

- [x] Bind selected topology resources during Terrain resource preparation and
  execution without changing the exact height texture or vertex-factory sample
  contract.
- [x] Use the same prepared LOD/stitch facts for Opaque, Masked, Translucent,
  Solid, Wireframe, Lit, Unlit, offscreen, editor viewport, and directional
  shadow paths; shadow views retain independent selection.
- [x] Preserve PBR/material compatibility, UVs, normals, winding under negative
  height scale and mirrored transforms, depth policy, translucent ordering,
  environment lighting, and render-target layouts.
- [x] Add bounded read-only LOD diagnostics and a disabled-by-default viewport
  overlay showing patch edges, resolved level, and stitched edges without
  retaining expanded terrain geometry.
- [x] Validate asset assignment/removal, changed/no-op/failed reimport,
  spacing/height/material edits, add/remove, two Worlds, recorded commands,
  device invalidation, renderer restart, component destruction, and shutdown.
- [x] Add exhaustive CPU geometry coverage plus offscreen Vulkan execution for all neighbor
  directions/masks, grazing cameras, world edges, partial patches, shadows,
  negative scale, mirrored transforms, and camera motion; record the Stage 3
  handoff.

#### Acceptance Gate

- Camera and shadow output contain no background-visible gap, T-junction
  exposure, outside-extent surface, winding inversion, or discontinuous shared
  edge throughout the frozen matrix.
- Every successful draw uses the topology, triangle count, LOD, and mask stored
  during preparation; resource failure is bounded, counted, and recoverable.
- Existing Terrain material, visibility, Cook/runtime, collision, revision,
  and lifecycle contracts remain clean under automatic and forced LOD modes.

### Stage 4: Qualify scalability and complete the T3 handoff

Dependencies: Stages 1-3 complete end-to-end rendering.

- [x] Run structural, error, adjacency, topology, counter, randomized view,
  image, motion, material/pass, shadow, Cook/Game, multiple-World, lifecycle,
  and regression matrices using focused native targets throughout.
- [x] Measure frozen 1025x1025 `ForceLOD0` and flat-far views on the same
  profile, retaining the T1 near/mixed/grazing correctness fixtures and
  reporting CPU preparation, Scene Color GPU, draws, triangles, topology
  cache/bytes, retained metadata, fallbacks, and adjacency work against Stage 0 gates.
- [x] Prove automatic mixed/far views reduce triangle submission materially
  while near and forced paths retain exact T1 quality; do not claim benefit
  from visibility changes, altered extent, missing draws, or disabled passes.
- [x] Run the repository-required aggregate build/test and validation-enabled
  editor smoke for user-visible rendering, plus a cooked Game smoke without
  source or DDC, according to the owning build/test guidance.
- [x] Publish lasting LOD/error/adjacency/stitching/resource/diagnostic/limit
  contracts in [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md),
  update the Heightfield Terrain roadmap with T3 evidence and exact T4 entry
  state, and validate changed docs, all plans, and all roadmaps.
- [x] Record final revision, build profile, fixtures, test counts, images,
  timings, triangles, draws, bytes, topology variants, decisions, and deferred
  work before closing every passed checklist and completing this plan.

#### Acceptance Gate

- One clean handoff proves deterministic crack-free Terrain LOD for camera and
  shadow views across every legal neighbor transition, partial edge, transform,
  material pass, and lifecycle case.
- Automatic LOD meets all frozen structural, quality, CPU, GPU, work, memory,
  and resource gates at the existing ceiling, while `ForceLOD0` remains an
  exact qualified comparison path.
- Lasting Runtime documentation owns implemented behavior, and T4 can consume
  stable render diagnostics and selection behavior without redefining T3.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Error metric | Exact per-LOD deviation is conservative, monotonic as frozen, revision-bound, finite, and byte-accounted | Proxy metadata and CPU-oracle tests |
| Selection | Perspective/orthographic thresholds, equality, `ForceLOD0`, and invalid input return stable legal levels | Pure view/LOD tests |
| Adjacency | Every direction and chain resolves to delta <= 1 by bounded deterministic promotion | Renderer preparation tests |
| Stitch topology | All 16 masks, corners, partial patches, and world edges cover shared boundaries with valid winding and no outside triangle | Topology generator/oracle tests |
| Surface fidelity | Heights, UVs, normals, material passes, lighting, negative scale, and mirrored transforms preserve T1 behavior | CPU fixtures and Vulkan images/readback |
| Motion continuity | Threshold crossings and grazing camera paths have no exposed crack, unstable non-policy oscillation, or extent change | Deterministic frame sequences and image comparison |
| Visibility/bounds | Full-resolution regional bounds remain conservative for every selected topology and frustum boundary | Scene visibility/preparation tests |
| Shadows | Independent shadow-view LOD remains stitched and matches camera-path ownership/lifetime rules | Directional-shadow Vulkan tests |
| Counters | Candidates, visible/culled patches, histograms, promotions, masks, triangles, resources, draws, bytes, and fallbacks reconcile | Counter contract tests |
| Resources/lifetime | Complete cache keys share; invalid keys/failures do not poison entries; invalidation and shutdown release all resources | Renderer resource tests |
| Revision/Cook | Changed revisions replace complete proxy/error state; no-op/failure retains identity; cooked Game needs no source/DDC | Terrain lifecycle and Cook/runtime tests |
| Performance | Frozen views meet CPU/GPU/draw/triangle/memory/work gates and automatic benefit is measured against forced LOD 0 | Qualification target and runtime smoke |
| Collision isolation | Render LOD never changes HeightField identity, bodies, queries, or collision facts | Terrain integration tests |

## Definition of Done

- Every Terrain proxy carries bounded immutable per-patch LOD error facts tied
  to one committed height revision, with deterministic failure and accounting.
- Every submitted view selects legal patch LODs, resolves adjacency to the
  frozen delta, and prepares one stable stitch mask/topology result per visible
  patch without prior-frame or global mutable state.
- Index-only stitching covers all legal transitions and boundary patches with
  no crack, T-junction exposure, outside extent, invalid index, degenerate
  triangle, or height alteration.
- Camera, shadow, material, visibility, sorting, counter, resource, revision,
  Cook/Game, and lifecycle paths consume the same qualified results and retain
  existing Terrain contracts.
- The 1025x1025 matrix meets frozen CPU/GPU/memory/work gates and demonstrates
  measured mixed/far triangle reduction against exact `ForceLOD0` output.
- Focused and aggregate validation, editor/Game smoke, lasting Runtime docs,
  roadmap status, plan/roadmap validation, and committed handoff all pass.

## Deferred Follow-ups

- T4 terrain asset presentation, property UX, picking precision, reimport/error
  presentation, undo/redo, and final user workflow qualification.
- Conditional streaming/residency only after a named world exceeds the finite
  1025x1025 budgets.
- Hysteresis, geomorphing, or dithered transitions only after measured camera
  motion exposes unacceptable threshold popping and temporal ownership is
  frozen.
- GPU-driven selection/submission, tessellation, mesh shaders, compute topology,
  indirect draws, and occlusion/HZB behind separate capability/performance
  evidence.
- Runtime deformation, dirty-region error rebuild, holes, layers, foliage,
  navmesh, and collision LOD.

## Related Documentation

- [Heightfield Terrain Roadmap](../../../Roadmaps/HeightfieldTerrain.md)
- [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md)
- [Terrain Heightmap Asset](../../../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Runtime Collision](../../../Runtime/Physics/Collision.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Components/TerrainComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/TerrainComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/TerrainSceneProxy.h`
- `Engine/Source/Runtime/Engine/Private/Engine/TerrainSceneProxy.cpp`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/RenderCore/Public/Terrain/TerrainVertexFactory.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ViewPreparationMath.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ViewPreparationMath.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderPrimitiveTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderQualificationTests.cpp`
