# Directional Shadow Caster Preparation Plan

Summary: Remove redundant per-cascade scene and draw-fact preparation while preserving independent conservative caster selection, cascade-local LOD, and exact shadow output.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

The selected production boundary is the Stage 1 frame-local caster table plus
the existing direct shadow-depth preparation mode and shared Skeletal palette.
The table walks authoritative scene primitives once, evaluates supported and
hidden state once, classifies every eligible caster against every enabled
cascade, and materializes reserved cascade-family references from a bounded
mask. Stage 0 timings and structural counters now also expose family fact
build/reuse counts, selected-LOD facts, Terrain patch classifications, and the
existing nested/disjoint timing boundaries.

Target-host qualification used Win64 Release at 1920x1080 on the Intel Core
i7-12700 and RTX 3090, driver 591.86, Vulkan device API 1.4.325. Five
interleaved 30-warm-up/120-measured-frame runs compared isolated pre-table
commit `0cb52ef409663a35de9be952efe1dd68fcae8b6a` with the table path. The
run-median ThreeCascades logical result improved from 1,106.4 us to 1,100.1 us
and p95 improved from 1,544.8 us to 1,459.0 us. SingleMap median/p95 improved
from 337.3/400.5 us to 318.3/388.8 us. The high-overlap fixture reports one
scene traversal, 128 unique eligible StaticMesh casters, 384 conservative
classification tests, and membership popcount 384. Stage 1 therefore passes
its frozen structural and no-regression gates.

Stage 2 activated because Static/Spline preparation owned approximately 97%
of the high-overlap fixture. A frame-local primitive/LOD/section fact-table
candidate preserved correctness but regressed ThreeCascades logical median to
at least 1,104 us versus the 1,019.5 us observation used for its candidate gate;
it missed both the 85% family and 95% total thresholds. The candidate and all
unused scaffolding were removed. Stage 2 is rejected; the already-qualified
shadow-depth translucent filtering and shared Skeletal palette remain.

Stage 3 activated because Terrain preparation owned 98.9% of a 256-patch,
two-cascade-membership fixture. Its shared primitive/material/transform/bounds
and patch-mask candidate improved Terrain median only from 988.2 us to 963.5 us
(2.5%) and total logical median from 999.0 us to 975.6 us (2.3%). This missed
the required 15% family and 5% total improvements, so the candidate and unused
scaffolding were removed. The qualification fixture and counters remain to
make that rejected boundary reproducible.

Exact directional-shadow qualification, the off-camera/mask contracts,
StaticMesh preparation, Skeletal resource reuse, and Terrain qualification
pass on the target host. The lasting runtime contract documents the selected
single-traversal table, cascade-local draw preparation, timing boundaries, and
frame-local palette ownership. Persistent cross-frame caching remains deferred.

### Frozen Stage 0 measurement policy

- The target lane is Win64 Release on the Intel Core i7-12700, RTX 3090,
  driver 591.86, and Vulkan device API 1.4.325 at 1920x1080. Each scenario uses
  30 warm-up frames followed by 120 measured frames. Inline and threaded
  executors use identical scene, view, light, candidate, and sample identities.
- The comparison revisions are pre-table commit `0cb52ef409663a35de9be952efe1dd68fcae8b6a`
  and table commit `d863734fdb067ce67d584d8ee226d7b3fe3ac45b`.
  Historical comparison builds use an isolated worktree and never add a second
  production traversal path to the active implementation.
- Stage 1 passes when the high-overlap fixture performs exactly one scene
  traversal, exactly `eligible casters x enabled cascades` conservative tests,
  preserves membership/counter/image identities, has a logical-preparation
  median no greater than the pre-table reference, and has p95 no greater than
  105% of the reference. Sparse SingleMap and ThreeCascades medians may regress
  by at most 5% and p95 by at most 10%.
- Stage 2 activates only when Static/Spline or Skeletal logical preparation is
  at least 20% of total logical shadow preparation in its owning fixture and
  membership popcount is at least 1.5 times unique eligible casters. It passes
  when the owning-family median is at most 85% of its Stage 0 baseline, total
  logical median is at most 95%, and sparse median/p95 regressions remain within
  5%/10%.
- Stage 3 activates only when Terrain logical preparation is at least 20% of
  total logical shadow preparation and Terrain patch classification or
  immutable patch-fact work repeats in at least two cascades. It uses the same
  85% owning-family, 95% total, and 5%/10% sparse median/p95 gates as Stage 2.
- Existing exact Q0/Q1 hashes, Q0/Q1 motion limits, Q2 cascade identities, and
  the off-camera-caster contract are the frozen rendering references. New
  preparation fixtures must conserve submitted, hidden, culled, invalid,
  membership, prepared family, draw, triangle, palette, patch, batch, and
  temporary-capacity identities between compared candidates.
- Timing and structural evidence remains in the existing optional view-counter
  sink. Sorting/batching stays nested, resource preparation stays disjoint, and
  neither is added twice to total logical preparation.

## Goal

Prepare authoritative directional-shadow casters once per rendered view,
classify every eligible primitive against all enabled cascades in one pass,
and reuse frame-local geometry, material, and resource facts across receiver
and shadow draw preparation. Preserve cascade-local membership, LOD, Terrain
adjacency, raster bias, counters, resource recovery, and rendered output while
reducing render-thread preparation cost and temporary allocation growth in
scenes with overlapping cascade casters.

## Scope

- Render-thread CPU timing and structural counters for shadow caster discovery,
  membership classification, family preparation, sorting/batching, resource
  preparation, and total pre-pass preparation.
- One frame-local authoritative shadow-caster table built from `FScene`
  primitive collections, with supported kind, eligibility outcome, and one bit
  per enabled cascade.
- Per-cascade candidate views derived from the shared table without rescanning
  the scene or rechecking shared hidden/kind facts.
- Frame-local StaticMesh, SplineMesh, and SkeletalMesh preparation facts shared
  where material, geometry, transform, deformation, and resource snapshots are
  identical.
- Cascade-local Static/Spline LOD selection, prepared primitive records,
  raster-bias state, final draw references, counters, and execution order.
- A Terrain-specific shared layer for primitive material/transform facts,
  patch bounds/topology facts, and patch cascade masks while retaining
  cascade-local LOD adjacency and stitch decisions.
- Exact counter reconciliation and image, motion, failure, multi-view, and
  performance qualification for SingleMap and ThreeCascades.
- Publication of lasting preparation ownership and invalidation rules in the
  directional-shadow runtime contract after implementation.

## Non-Goals

- Reusing main-camera visibility as shadow-caster visibility or excluding an
  off-camera primitive that intersects a fitted caster volume.
- Removing the bounds test against any enabled cascade; one traversal still
  performs one conservative classification per primitive and cascade.
- Changing cascade count, split policy, caster extrusion, fitting, resolution,
  filter quality, transition blending, bias equations, shadow distance, or
  selected light ownership.
- Adding GPU-driven culling, indirect draws, a render graph, task-parallel
  preparation, local-light shadows, virtual shadow maps, ray tracing, or a new
  spatial acceleration structure.
- Changing Static/Spline LOD policy, adding SkeletalMesh LODs, or changing
  Terrain LOD and crack-prevention behavior to obtain a faster result.
- Treating a mixed-material primitive as wholly translucent. Eligibility that
  depends on blend mode remains section-level.
- A persistent cross-frame draw or shadow-map cache. Such a cache requires
  stable scene, transform, material, geometry, deformation, residency, light,
  and view revision identities and remains a separate measured follow-up.
- Reducing shadow-depth GPU draws or texel fill when the same caster correctly
  participates in multiple cascades.

## Design Decisions and Invariants

### Authoritative discovery and membership

- Shadow discovery starts from the authoritative `FScene` primitive
  collection. Camera visibility is neither an input nor a fallback.
- One `FDirectionalShadowCasterRecord`-equivalent value owns a non-owning
  `FPrimitiveSceneInfo` reference, primitive kind, shared eligibility outcome,
  and cascade mask for the current prepared view. The table is owned by
  `FPreparedSceneView` or a directional-shadow preparation value with identical
  lifetime; it never outlives SceneInfo publication.
- Hidden state and supported primitive kind are evaluated once. Every enabled
  cascade still calls the existing conservative bounds classifier. Invalid
  bounds retain the current included fallback in every applicable cascade.
- Per-cascade submitted, hidden, culled, invalid-bounds, selected-family, and
  prepared counters retain their observable meaning. Aggregate counters may
  add unique-scene and membership-popcount values but do not silently redefine
  existing fields.
- Disabled culling and SingleMap use the same table path. A bitmask is valid
  only for enabled cascade indices and no stale bit from another view is
  observable.

### Shared facts and cascade-local decisions

- Shared facts are frame-local immutable snapshots, not references to mutable
  component or asset state. Preparation reads render-thread proxies and
  renderer generation state through existing ownership boundaries.
- Material resolution, exact v3/v2 binding validation, blend classification,
  two-sided state, opacity-mask identity, section/index-range validity,
  transform finiteness, winding, and stable geometry pointers are shared only
  when their inputs are identical.
- Static/Spline projected size, requested and resolved LOD, selected LOD
  geometry, and prepared primitive index remain cascade-local. LOD-specific
  section facts may be memoized once per selected `(primitive, LOD)` within the
  frame.
- Skeletal pose identity and palette range are shared for the render submission.
  Shadow and receiver views keep independent prepared primitive indices and
  draws but must not upload the same primitive pose once per cascade.
- Translucent sections never enter shadow draw lists. Shadow-mode preparation
  filters them before final shadow sorting and counter accumulation instead of
  constructing and then erasing complete translucent buckets.
- Raster depth bias is cascade-local and remains part of the effective shadow
  pipeline identity. Sharing a material fact never aliases or mutates another
  cascade's pipeline key.
- Final opaque and masked ordering remains deterministic. Reusing a shared
  state key or filtering a preordered immutable fact list is allowed only when
  it produces the same ordering relation as an independent cascade sort.

### Terrain boundary

- Terrain primitive material, transform/winding, patch descriptor, transformed
  patch bounds, and topology dimensions are candidates for frame-local reuse.
- Patch membership is classified against all enabled cascade views in one
  patch traversal after its owning Terrain primitive passes coarse membership.
- Requested LOD, resolved adjacency, stitch mask, triangle count derived from
  the resolved topology, visible draw set, and batches remain cascade-local.
  Adjacency may consume non-drawn neighboring patches when required by the
  existing crack-prevention contract.
- Terrain distance policy and ForwardZ shadow-view behavior remain unchanged.
  The optimization cannot obtain a pass by weakening patch culling or
  adjacency conservation.

### Failure, lifetime, and fallback

- Allocation failure, invalid material/geometry/deformation, missing residency,
  palette rejection, shader/PSO failure, device invalidation, and manual retry
  preserve the established fully lit fallback and renderer resource slots.
- A shared fact rejected for correctness cannot remain accepted in one
  cascade through stale references. Cascade-local LOD/resource fallback remains
  independently counted where its selected inputs differ.
- Sequential views own separate frame-local tables. No view may consume
  another view's membership mask, selected LOD, bias, palette range, patch
  membership, counters, or temporary storage.

## Current Foundations and Gaps

| Area | Existing foundation | Gap selected by this plan |
| --- | --- | --- |
| Scene visibility | One authoritative camera-visibility traversal already classifies hidden and camera-frustum state. | Shadow discovery independently walks the complete scene once per cascade; camera results cannot safely replace it. |
| Caster bounds | `ClassifyDirectionalShadowCasterBounds` provides conservative per-cascade classification with invalid-bounds fallback. | The caller invokes it from three separate scene traversals and allocates four family vectors per cascade. |
| Static/Spline | Production preparation owns LOD, material binding, sections, state keys, sorting, and diagnostics. | The same readiness, material, section, transform, and key facts are rebuilt for every participating cascade. |
| Skeletal | Production and shadow preparation share proxies, poses, materials, render resources, and one frame-local palette table established by `ca20846c`. | Draw and material facts are still rebuilt per cascade; the existing palette boundary must be retained. |
| Terrain | Patch LOD, adjacency, culling, topology keys, sorting, and batching are explicit and timed internally. | Every cascade repeats primitive material work, all-patch LOD/adjacency setup, transformed bounds, sorting, and batch construction. |
| Resource caches | Static, Skeletal, and Terrain renderers retain shader, PSO, topology, height, and related RHI resources across draws. | Cache hits avoid creation but do not avoid repeated logical lookup/key construction and per-cascade draw preparation. |
| Diagnostics | Per-cascade submitted through drawn counters and GPU shadow-depth timing exist. | There is no complete render-thread CPU breakdown, unique eligible count, membership popcount, or repeated-fact metric. |

## Implementation Stages

### Stage 0: Freeze CPU evidence and structural contracts

- [x] Add scoped render-thread CPU timings for total directional-shadow logical
  preparation, authoritative discovery/membership, Static/Spline preparation,
  Skeletal preparation, Terrain logical preparation, sorting/batching, and
  shadow resource preparation. Ensure nested timings have documented inclusion
  boundaries and do not double-count in the reported total.
- [x] Add structural counters for unique submitted primitives, unique eligible
  primitives by family, cascade classification tests, membership-mask popcount,
  per-family shared-fact builds/reuses, selected `(primitive, LOD)` facts,
  Terrain patch classifications, and temporary bytes or high-water counts.
- [x] Define deterministic camera-visible, off-camera-caster, high-overlap,
  sparse-cascade, Static/Spline-heavy, animated-Skeletal, and Terrain-heavy
  fixtures. Record SingleMap and ThreeCascades baselines with identical scene,
  view, light, executor, build, warm-up, and sample counts.
- [x] Before observing optimized results, record the target machine and exact
  median/p95 rollout gates for total render-thread shadow preparation and each
  stage's allowed regression. Record a structural Stage 1 gate of one scene
  traversal per prepared shadow view and exactly `eligible primitives x enabled
  cascades` conservative classification opportunities, excluding early
  unsupported/hidden rejection.
- [x] Record exact counter identities and image/motion hashes that subsequent
  stages must preserve. Resolve whether timing publication belongs in existing
  view counters or a dedicated optional profiling sink before Stage 1.

#### Acceptance Gate

- Baseline evidence is reproducible, timing boundaries reconcile, fixtures
  exercise all caster families and overlapping masks, correctness references
  are frozen, and numeric rollout gates are written into this plan before any
  optimized timing is accepted.

### Stage 1: Build one frame-local caster table and cascade mask

- [x] Replace per-cascade calls that rescan `FScene` with one authoritative
  table builder invoked after directional-shadow cascade fitting succeeds.
- [x] Evaluate null/hidden/kind eligibility once, classify each retained record
  against every enabled cascade, and encode membership in a bounded mask whose
  width is checked against `DirectionalShadowCascadeCount`.
- [x] Materialize typed per-cascade spans or reference vectors from the table
  without copying records or re-reading scene state. Reserve from recorded
  family/membership counts to avoid repeated growth.
- [x] Preserve `bDisableCulling`, SingleMap, invalid-bounds fallback, boundary
  contact, off-camera caster, and per-cascade counter semantics. Add direct mask
  and distribution tests, including one primitive in zero, one, two, and all
  cascades.
- [x] Remove or narrow `PrepareDirectionalShadowCasterCandidates` only after all
  callers and tests consume the shared-table contract; do not leave a second
  production scan path.

#### Acceptance Gate

- Each prepared shadow view walks the authoritative primitive collection once;
  per-cascade candidate identities and counters match the frozen baseline;
  images and motion references are unchanged; CPU median/p95 meet the frozen
  no-regression gate in sparse and overlapping fixtures.

### Stage 2: Share mesh and material facts within the prepared view

- [x] Rejected after target-host measurement: splitting Static/Spline
  preparation into immutable primitive/LOD/section facts
  and view-local draw instances. Memoize eligible facts once per frame and once
  per actually selected LOD without changing LOD resolution fallback.
- [x] Not selected after the Static/Spline owner failed the Stage 2 gate:
  splitting Skeletal preparation into additional immutable geometry/material
  facts and view-local primitive/draw instances. The existing submission-local
  palette table retains its one-pose/one-range ownership.
- [x] Introduce an explicit shadow-depth preparation mode that admits only
  Opaque and Masked sections and builds the correct shadow pipeline keys
  directly. Remove the post-preparation translucent erase/accounting repair.
- [x] Rejected after target-host measurement: sharing material binding
  validation and stable state-key components between
  the receiver and cascades where inputs match. Keep projection size, LOD,
  primitive indices, bias, resource readiness outcome, and execution counters
  in their existing view/cascade owners.
- [x] Proved candidate correctness for deterministic opaque/masked order,
  mirrored winding, two-sided and
  masked parity, Spline deformation, Skeletal pose/palette reuse, resource
  invalidation, and error-material fallback.

#### Acceptance Gate

- Rejected. The fact-table candidate preserved frozen correctness references
  and palette ownership but did not meet the 85% family or 95% total CPU gate.
  No translucent shadow draw is constructed, and the rejected cache has no
  remaining production or compatibility path.

### Stage 3: Share Terrain facts and classify patches once

- [x] Rejected after target-host measurement: extracting frame-local Terrain
  primitive material/transform facts and patch
  world-bounds/topology facts without changing proxy or payload ownership.
- [x] Rejected with the Stage 3 candidate: computing a bounded cascade mask for
  each valid patch in one traversal and
  feed cascade-local LOD/adjacency preparation from those records. Retain any
  neighbor data needed to reproduce existing adjacency promotions and stitch
  masks exactly.
- [x] Rejected with the Stage 3 candidate: avoiding repeated material binding,
  bounds transformation, topology-key base
  construction, and immutable sort-key work. Keep resolved LOD, stitch mask,
  final topology key, draw membership, sort, and batches cascade-local.
- [x] Reconciled candidate primitive membership, patch membership,
  culled/radial rejected,
  LOD fallback, adjacency, stitch histogram, triangle, batch, instance, height,
  topology, resource, and draw counters for every cascade.
- [x] Qualified the candidate through the frozen Terrain and directional-shadow
  references, including boundary patches, invalid bounds, mixed membership,
  Terrain-heavy camera/light motion, disabled batching, height replacement,
  shader reload, device invalidation, and sequential views.

#### Acceptance Gate

- Rejected. The candidate preserved cascade-local LOD, adjacency, stitch,
  batches, triangles, counters, and rendered output, but its 2.5% Terrain and
  2.3% total median improvements did not meet the 15%/5% rollout gate. No
  shared Terrain fact or patch-mask scaffolding remains in production.

### Stage 4: Integrate, qualify, and publish the selected boundary

- [x] Remove obsolete duplicate preparation APIs, temporary compatibility
  fields, redundant allocations, and counters whose replacements have passed
  reconciliation. Keep public surface area private to Renderer unless another
  existing module already owns the shared fact type.
- [x] Run focused caster, visibility, preparation, material, deformation,
  Terrain, resource recovery, counter, and multi-view native tests following
  the repository testing workflow.
- [x] Run the required Renderer/RHI/Vulkan builds and runtime validation,
  fixed/moving image comparisons, threaded and inline executor cases, shader
  reload, resize, retry, device invalidation, editor smoke, and target-machine
  CPU/GPU timing matrix following repository build/run guidance.
- [x] Update the directional-shadow runtime contract with the implemented
  frame-local table, shared-fact boundary, cascade-local decisions, counter
  meanings, failure behavior, and measured result. Record any rejected deeper
  cache candidate rather than leaving two active implementations.
- [x] Complete this plan only when every selected stage passes. If Stage 0
  evidence rejects Stage 2 or Stage 3, record that disposition, remove unused
  scaffolding, and narrow the Definition of Done before marking completion.

#### Acceptance Gate

- Required tests, builds, validation layers, runtime matrices, image/motion
  references, recovery cases, and editor smoke pass; the selected target CPU
  gates pass without GPU, memory, draw-count, or output regression; lasting
  behavior is documented and no duplicate production preparation path remains.

## Validation Matrix

| Contract | Required evidence |
| --- | --- |
| Authoritative discovery | Off-camera casters remain included; hidden, unsupported, invalid-bounds, boundary-contact, disabled-culling, SingleMap, and ThreeCascades cases match baseline. |
| Membership | Mask tests cover zero through all cascades; distribution and per-cascade counters reconcile with mask popcount and unique eligible counts. |
| Static/Spline | Cascade-local projected LOD and fallback remain exact; material, section, winding, two-sided, Masked, deformation, ordering, and resource outcomes match. |
| Skeletal | Pose validation, one submission-local palette range, animation, Masked coverage, material fallback, resource failure, and shadow/base deformation parity pass. |
| Terrain | Patch culling, LOD, adjacency, stitch masks, triangle counts, batches, height/topology resources, invalid bounds, and motion match the frozen references. |
| Ordering and bias | Opaque/Masked state ordering is deterministic; each cascade retains its own depth-bias pipeline identity and cannot mutate a shared key. |
| Counters | Unique, aggregate, per-cascade, family, section/patch, resource, draw, triangle, reuse, timing, and temporary-storage values have tested conservation equations. |
| Lifetime and failure | Scene mutation boundaries, sequential views, shader reload, resize, retry, device invalidation, rejected resources, release, and shutdown expose no stale table or fact. |
| Rendering | Fixed and moving images remain within the frozen exact/tolerance rules across supported caster families, views, diagnostics, filters, and executors. |
| Performance | Stage-specific and total render-thread median/p95 pass frozen target gates; measurements report scene, membership overlap, draws, patches, build, hardware, warm-up, and samples. Shadow-depth GPU time, draws, and memory do not regress beyond their frozen allowance. |
| Handoff | Focused and aggregate native tests, required builds, Vulkan validation, runtime matrix, documentation validation, and editor smoke follow repository guidance and pass. |

## Definition of Done

- Directional-shadow discovery traverses authoritative scene primitives once
  per prepared view and records one tested cascade mask per eligible caster.
- No production cascade independently rechecks shared hidden/kind eligibility.
  Deeper immutable material/geometry reuse remains intentionally unselected
  because both measured candidates missed their rollout gates.
- Static/Spline LOD and fallback, Terrain LOD/adjacency/stitch, raster bias,
  ordering, counters, and execution remain cascade-local where correctness
  requires them.
- Receiver and shadow Skeletal draws reuse one valid pose palette range per
  primitive submission and retain exact deformation.
- Opaque and Masked participation, off-camera casters, invalid-bounds fallback,
  resource failure, fully lit fallback, diagnostics, multi-view isolation, and
  rendered images match the frozen contract.
- Target-machine render-thread measurements meet the Stage 0 gates, and the
  result records both wins and any rejected deeper optimization.
- Lasting ownership and behavior are published in the runtime contract; all
  required validation passes; obsolete duplicate preparation paths are removed.

## Deferred Follow-ups

- Persistent cross-frame draw-fact caching requires explicit revision facts for
  scene membership, transforms, materials, geometry, residency, Spline data,
  Skeletal poses, Terrain payloads, renderer generation, light, and cascade
  fitting. It activates only if post-plan profiling still shows material CPU
  cost in sufficiently static workloads.
- A scene spatial index or GPU-driven caster culling requires world-scale and
  primitive-density evidence showing that the retained
  `eligible primitives x cascades` bounds tests are material.
- Shadow-map caching or cascade update scheduling requires stable caster/light/
  view revision identities and separate stale-content, motion, and multi-view
  acceptance contracts.
- Task-parallel logical preparation requires measured single-thread cost plus
  explicit render-proxy snapshot and allocator ownership; it is not implicit in
  the shared-fact representation.

## Related Documentation

- [Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md)
- [Shadow System Evolution Roadmap](../../../Roadmaps/Archive/2026-08/ShadowSystemEvolution.md)
- [Cascaded Directional Shadows Plan](CascadedDirectionalShadows.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
