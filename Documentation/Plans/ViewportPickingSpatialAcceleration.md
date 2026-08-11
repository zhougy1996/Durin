# Viewport Picking Spatial Acceleration Plan

Summary: Add a game-thread scene broad phase and immutable StaticMesh ray-query acceleration while preserving exact M1-M2 picking results.

Last reviewed: 2026-08-11

Status: Active
Completed:

## Current Status

M1 and M2 provide the stable semantic request, lifetime, ordering, StaticMesh
LOD0 oracle, and current-pose SkeletalMesh oracle required to measure an
accelerated implementation. M3 is now active at Stage 0.

The current reference backend discovers every registered primitive by scanning
the Level for every geometry request. A bounds test rejects many mesh queries,
but every surviving StaticMesh still tests every LOD0 triangle, and every
bounds-surviving SkeletalMesh skins every referenced vertex and tests every
triangle within the request-wide M2 budget. There is no reusable scene index,
asset triangle hierarchy, mutation protocol, acceleration memory budget, or
reference-versus-accelerated comparison mode.

Stage 0 must record representative editor workloads and freeze the numeric
budgets before implementation. The selected ownership, update, fallback, and
parity rules below are already binding; Stage 0 may tune only the explicitly
listed thresholds and must record the evidence for each change.

## Goal

Make ordinary Level Editor scene-geometry picking scale with ray-relevant
primitives and triangles instead of total Level and mesh size, without changing
the M1 public API, M2 surface semantics, selection UX, or deterministic winner.

The completed M3 path must incrementally maintain current primitive bounds,
reuse immutable StaticMesh acceleration across component instances and
viewports, retain selectable reference execution, and report enough private
work and memory counters to prove both correctness and useful candidate
reduction.

## Scope

- A Level-bound, game-thread scene broad phase shared by the viewports in one
  `FLevelEditorContext`.
- Editor-only primitive mutation observation for registration, retirement,
  transform, visibility, StaticMesh proxy/data replacement, and SkeletalMesh
  pose-bound publication.
- Finite current world bounds, exact primitive identity, registration
  generation, weak Actor/component identity, and primitive-family metadata in
  each scene-index leaf.
- A deterministic dynamic AABB hierarchy with incremental add, remove, and
  update behavior plus bounded rebuild policy.
- Immutable, asset-lifetime, per-LOD StaticMesh triangle BVHs; M3 queries remain
  LOD0 to preserve the M1 precision contract.
- An accelerated backend behind `IViewportPickingBackend`, plus explicit
  reference, accelerated, and compare execution policies for tests and
  diagnostics.
- Conservative current-pose SkeletalMesh candidate reduction only if Stage 0
  measurements pass its activation gate.
- Private counters for index mutation, build/refit, retained bytes, bounds and
  node visits, candidate primitives, candidate/tested triangles, skeletal
  skinning work, fallback, and parity mismatch.
- Deterministic fixtures, randomized/adversarial parity, lifetime coverage,
  performance qualification, lasting documentation, and roadmap disposition.

## Non-Goals

- Changing `FViewportPickRequest`, `FViewportHitResult`, ticket lifecycle,
  visualization arbitration, Select mode, `FLevelEditorContext` selection, or
  transform-gizmo ownership.
- Changing the established LOD0, double-sided, world-distance, distance epsilon,
  stable-key, current-pose, or atomic M2 budget semantics.
- Reading Renderer `FScene`, SceneInfo, prepared views, or reflected objects on
  the render thread for CPU picking.
- Turning editor picking into collision, physics, navigation, or a universal
  world-query service.
- GPU ID rendering, asynchronous texture readback, hardware ray tracing,
  BLAS/TLAS APIs, compute picking, meshlets, or a Render Graph.
- Persisting BVHs as authored asset data or derived-data cache payloads in the
  first M3 slice; that requires separate build-time and compatibility evidence.
- Accelerating bone, socket, section, material, instance, vertex, or triangle
  selection.
- Treating bind-pose triangle bounds as conservative for arbitrary skeletal
  deformation.

## Design Decisions and Invariants

### Ownership and thread boundary

- `FLevelEditorContext` owns one Level-bound picking scene index and shares it
  with all of its viewport picking services. Switching or clearing the Level
  retires the complete index before requests can target the new document.
- The index, observer registration, mutation application, BVH queries, and
  request-local target capture run on the game thread. No new worker or render
  thread is introduced by M3.
- Engine exposes a narrow editor-only Level primitive-mutation observer seam.
  `DPrimitiveComponent` and `DSkeletalMeshComponent` publish value-described
  dirtiness through their existing authoritative registration, render-state,
  transform, visibility, asset/proxy, and pose-bound paths. The seam does not
  depend on LevelEditor types and does not retain an observer after its owner
  unregisters.
- One initial snapshot is delivered when an index attaches. Later events are
  ordered on the game thread. A missed revision, observer overflow, invalid
  event, or Level replacement triggers a complete snapshot rebuild; it never
  leaves a knowingly partial index queryable.
- Index leaves retain weak Actor/component identity and owned values only. They
  never extend reflected-object lifetime. Request submission still freezes the
  M1 request-local token table and later validates Level, component ownership,
  registration generation, visibility, and `FPrimitiveSceneId`.

### Scene broad-phase contract

- Only registered, visible StaticMesh and SkeletalMesh components with non-zero
  primitive identity and finite valid current world bounds enter the index.
  Invalid bounds omit the leaf; a later valid update admits it again.
- StaticMesh bounds derive from current LOD0 local geometry bounds transformed
  by the component matrix. Skeletal bounds derive from the same immutable
  current-pose palette/bounds publication used by M2, transformed by the
  component matrix. Singular or non-finite transforms omit the leaf.
- The hierarchy uses exact leaf bounds and may use deterministic fat internal
  update bounds. A leaf that escapes its update bound is removed and reinserted;
  visibility and retirement remove it immediately.
- Ray traversal returns a set of candidate primitive identities, not a winner.
  Candidate enumeration order is deliberately non-semantic; the existing
  world-distance epsilon and stable key select the winner.
- A deterministic rebuild is permitted only after Stage 0 freezes tree-height,
  reinsertion, and mutation-ratio thresholds. Rebuild happens at mutation
  synchronization, never as an unbounded hidden operation inside result
  arbitration.

### StaticMesh acceleration and lifetime

- Each valid `FStaticMeshLODResources` may own or reference one immutable flat
  binary triangle BVH built from its CPU positions and uint32 index triplets.
  Component instances reuse the same local-space data by transforming the ray.
- Construction uses deterministic longest-axis centroid partitioning with
  triangle ordinal as the final split tie-break and at most eight triangles per
  leaf. Nodes contain finite local AABBs and 32-bit child or triangle ranges;
  traversal uses near-bound distance only to prune work, never to alter hit
  comparison.
- Acceleration is published atomically with the matching StaticMesh CPU-data
  revision and retired with that render-data generation. A component never
  queries a BVH built from another asset, LOD, or revision.
- Malformed, non-finite, over-budget, or unavailable acceleration data makes
  that component use the retained M1 reference provider. It does not create a
  bounds-only hit, fail an otherwise valid request, or expose a partial BVH.
- M3 builds per-LOD data so its lifetime is correct for future policies, but the
  viewport provider queries only LOD0 until a separate plan changes visible-LOD
  selection semantics.

### Skeletal candidate-reduction gate

- Scene broad-phase filtering applies to SkeletalMesh unconditionally and uses
  the latest complete pose bounds. Triangle-level skeletal acceleration is not
  required merely to complete the StaticMesh BVH.
- Stage 0 activates skeletal candidate reduction only if representative
  bounds-surviving queries consume at least 25 percent of either M2 request
  limit or account for at least 25 percent of median accelerated-request CPU
  time. The evidence fixture and measurements are checked into or recorded in
  this plan.
- If activated, the structure groups indexed triangles by conservative unions
  of existing palette influence bounds, transforms/refits those bounds from one
  immutable pose snapshot, and skins a referenced vertex at most once among
  surviving groups. Every omitted group must be provably disjoint from the ray.
- The accelerated skeletal path retains the M2 request-wide limits of 250,000
  skinned vertices and 500,000 tested triangles, counting actual work. Exceeding
  either remains an atomic `Failed` completion with no partial winner.
- If the activation gate is not met, Stage 3 records the measurement and closes
  with the exact M2 provider retained. Bind-pose triangle BVHs and
  non-conservative section bounds are forbidden in either outcome.

### Budgets, failure, and fallback

- Stage 0 profiles at minimum: 100, 2,000, and 10,000 mixed primitives; shared
  StaticMesh assets at 10,000, 250,000, and 1,000,000 LOD0 triangles; transformed
  duplicates; sparse and dense ray crossings; and animated skeletal fixtures
  below, at, and above the M2 work limits.
- Initial hard ceilings are 64 MiB retained scene-index memory per Level,
  256 bytes per admitted primitive, 256 MiB acceleration memory per StaticMesh
  asset, and 64 bytes per indexed triangle. Stage 0 may lower these ceilings or
  raise one only with measured fixture size, build time, and fallback behavior
  recorded before Stage 1 begins.
- Crossing a scene or asset memory ceiling disables that acceleration unit and
  uses the reference discovery/query path for the complete request or component.
  Allocation/build failure follows the same fallback. Acceleration exhaustion
  never changes a valid semantic result into a partial result.
- Compare mode runs reference and accelerated queries against the same immutable
  request snapshot. Any status, hit presence, token, or distance mismatch beyond
  the established epsilon increments a mismatch counter and returns the
  reference completion; tests fail with both diagnostic records.
- Production interaction emits no per-click log. Aggregate private diagnostics
  are exposed through focused test seams and an explicit profiling capture only.

### Performance and parity qualification

- Correctness is exact at the semantic level: status, winning primitive,
  Actor/component identity, and stable equal-distance ordering match the
  reference path. Hit distance uses the existing `1e-8` comparison contract.
- Randomized fixtures use recorded seeds. Adversarial fixtures include equal
  centroids, degenerate triangles, axis-parallel rays, ray origins inside
  bounds, mirrored/non-uniform/singular transforms, overlapping bounds, equal
  distances, empty/invalid data, and hierarchy rebuild thresholds.
- On the representative 10,000-primitive sparse-ray fixture, scene candidates
  must be at most 5 percent of admitted leaves. On the 1,000,000-triangle
  StaticMesh fixture, exact triangle tests must be at most 1 percent of source
  triangles. These are candidate-reduction gates, not universal guarantees.
- The large-fixture median accelerated query time must be no more than 25
  percent of reference time over a recorded warm run, while the 100-primitive
  fixture may regress by no more than 10 percent. Wall-clock evidence supplements
  deterministic counters and cannot replace parity or memory qualification.

## Current Foundations and Gaps

| Area | Foundation | M3 gap |
| --- | --- | --- |
| Semantic service | Per-viewport tickets, immutable rays, request-local tokens, weak identity, generation validation, and one resolver | Target capture scans every Actor/component for every request; no shared Level query snapshot |
| Primitive identity | Stable `FPrimitiveSceneId`, registration generation, visibility, component transform, and Level hierarchy revision | No ordered editor primitive mutation observer or current-bounds registry |
| StaticMesh | Detached CPU positions/indices/sections/bounds and asset-local render-data revision | No immutable triangle BVH, build budget, revision pairing, or reference fallback counter |
| SkeletalMesh | Immutable current pose/palette/bounds, influence metadata, exact M2 skinning, and deterministic work caps | No scene index update from pose bounds and no evidence-based candidate grouping |
| Geometry math | Private double-sided ray/box and ray/triangle tests with one epsilon | No reusable deterministic node traversal or near-distance pruning |
| Diagnostics | M2 private invalid/bounds/budget/skinned/tested counters | No scene/BVH build, mutation, memory, visit, candidate, fallback, timing, or mismatch counters |
| Tests | Static/skeletal exact hits, transforms, animation, invalid data, budgets, lifetime, ordering, and selection behavior | No randomized parity, adversarial hierarchy, incremental update, shared-index, memory, or performance fixtures |

## Implementation Stages

### Stage 0: Freeze workloads, budgets, and mutation protocol

- [ ] Add deterministic representative fixture builders for the primitive and
  triangle counts listed in the budget contract without adding large binary
  test assets.
- [ ] Capture reference counters and warm-run timings for sparse, dense,
  transformed-instance, equal-distance, and animated-pose queries under the
  active Agent Build Profile.
- [ ] Record actual component add/remove, transform, visibility, StaticMesh
  replacement, registration-cycle, and skeletal pose-bound update rates from
  representative editor actions.
- [ ] Freeze the editor-only Level observer payload, revision sequence,
  subscription/retirement rules, initial snapshot, missed-revision recovery,
  and re-entrant mutation prohibition.
- [ ] Freeze the dynamic hierarchy fat-bound margin, height/reinsertion rebuild
  thresholds, synchronization point, and deterministic rebuild ordering.
- [ ] Measure proposed scene-node and StaticMesh-BVH layouts; confirm or revise
  the hard ceilings with exact byte accounting and reference fallback coverage.
- [ ] Apply the skeletal activation formula and record whether Stage 3 will
  implement candidate grouping or close with measured deferral.
- [ ] Confirm the accelerated backend remains private to LevelEditor and that
  no public semantic request/result field or Renderer/RHI dependency is needed.

#### Acceptance Gate

- The checked-in/recorded fixture definitions reproduce stable reference
  winners and deterministic work counts.
- Ownership, thread, payload, revision, recovery, rebuild/refit, memory, and
  fallback decisions are unambiguous and numeric thresholds are recorded.
- Baseline timings and candidate counts cover every workload class required by
  the roadmap entry gate.
- The skeletal optimization is explicitly activated or deferred from measured
  evidence before implementation begins.

### Stage 1: Maintain the shared Level scene index

- [ ] Add the editor-only Engine observer seam and value-described primitive
  mutation events without adding a reverse dependency on LevelEditor.
- [ ] Route registration, unregistration/destruction, transform, owner
  visibility, mesh/proxy replacement, and skeletal pose-bound publication
  through the observer with monotonically ordered revisions.
- [ ] Implement the deterministic dynamic AABB hierarchy, exact byte counters,
  mutation counters, rebuild policy, and complete-snapshot recovery.
- [ ] Own one index in `FLevelEditorContext`, attach/detach it with the active
  Level, and share it with all viewport picking services without global mutable
  state.
- [ ] Replace normal request-time Level scans with broad-phase candidate capture
  while preserving M1 token-table contents, stable keys, and late validation.
- [ ] Fall back to complete reference discovery when the index is unavailable,
  stale, over budget, rebuilding unsuccessfully, or cannot prove a complete
  mutation sequence.
- [ ] Cover initial population, empty Level, add/remove, hide/show, transform,
  reparent, unregister/re-register, mesh replacement, pose-bound updates, Level
  switch, context shutdown, two viewports, and forced rebuild thresholds.

#### Acceptance Gate

- Every authoritative mutation is visible to the next game-thread request or
  causes complete reference fallback; no stale leaf can become a semantic hit.
- Multiple viewports reuse one index and retain independent ticket/request
  lifetimes.
- Broad-phase candidates match a brute-force bounds oracle across recorded-seed
  randomized and adversarial scenes.
- Memory ceilings, byte conservation, update/rebuild counters, and shutdown
  retirement pass focused native coverage.

### Stage 2: Add immutable StaticMesh BVHs and backend parity

- [ ] Define the immutable per-LOD flat node/triangle layout and build it from
  the exact CPU geometry revision using deterministic stable partitioning.
- [ ] Validate finite bounds, child/range coverage, triangle ordinals, node
  containment, empty leaves, maximum depth, integer overflow, and exact retained
  bytes before publication.
- [ ] Pair acceleration lifetime with StaticMesh CPU/render-data publication,
  replacement, release, reimport, and failure/retry behavior.
- [ ] Add local-space near-first traversal with current-best pruning while
  retaining the M1 double-sided triangle test and world-distance winner.
- [ ] Keep the reference provider selectable and implement reference,
  accelerated, and compare backend policies behind the existing private
  interface.
- [ ] Fall back per component for absent, malformed, over-budget, mismatched, or
  failed BVH data; prove fallback returns the exact reference result.
- [ ] Add deterministic BVH builder/traversal tests plus randomized/adversarial
  result parity for multiple assets, shared instances, all supported transforms,
  overlap, degenerates, equal hits, rebuild/reimport, and reversed enumeration.

#### Acceptance Gate

- Reference, accelerated, and compare policies return identical completion
  status, hit presence, token, distance, and stable winner across all fixtures.
- One asset/LOD acceleration allocation is reused by multiple components and
  viewports and is retired with the matching data revision without leaks or
  dangling borrows.
- The 1,000,000-triangle representative fixture passes the triangle-candidate,
  memory, and warm median-time gates.
- Invalid or unavailable acceleration proves semantic reference fallback rather
  than partial selection or request failure.

### Stage 3: Apply the measured skeletal disposition

- [ ] Always route SkeletalMesh candidates through current-pose scene bounds and
  preserve one immutable pose snapshot per candidate query.
- [ ] If Stage 0 deferred triangle-level skeletal acceleration, add parity and
  candidate counters proving the scene broad phase integrates the unchanged M2
  provider, then record the deferral rationale.
- [ ] If Stage 0 activated it, build deterministic bind-data triangle groups
  from palette influence metadata, refit conservative group bounds from the
  current pose, and reject only groups whose refitted bounds miss the ray.
- [ ] If activated, skin each referenced surviving vertex once, test candidate
  triangles in stable ordinal order, and charge actual work to the unchanged
  request-wide M2 limits.
- [ ] If activated, compare every accelerated skeletal result against M2 across
  reference/interpolated/extreme poses, mixed/non-contiguous influences,
  multiple sections/components, invalid palettes, transforms, budget edges,
  and animation into/out of the ray.
- [ ] In either disposition, cover pose revision updates, invalid-to-valid
  bounds recovery, shared Level index mutation, Static/Skeletal competition,
  and atomic no-selection-change behavior on M2 budget failure.

#### Acceptance Gate

- The recorded Stage 0 disposition is implemented without weakening current-pose
  correctness or presenting bind-pose data as conservative.
- Skeletal broad-phase and any activated triangle reduction match M2 status,
  exact component, distance, ordering, and budget behavior for all fixtures.
- Actual skinned-vertex and tested-triangle counters conserve request work and
  demonstrate the recorded candidate reduction when the optimization is active.

### Stage 4: Qualify editor integration and performance

- [ ] Run recorded-seed randomized and adversarial compare suites repeatedly
  across scene-index rebuild, asset replacement, pose updates, and reversed
  target/tree construction order.
- [ ] Prove selection replace/toggle/blank behavior, visualization priority,
  gizmo preemption, contextual-mode restrictions, request cancellation,
  supersession, client/Level invalidation, and two-viewport isolation remain
  backend-independent.
- [ ] Capture final reference and accelerated counters, exact memory, build/refit
  time, warm query distributions, and fallback counts for every Stage 0 workload.
- [ ] Verify no request-time full Level scan occurs on the healthy accelerated
  path and no build/rebuild work is repeated per viewport or component instance.
- [ ] Run the smallest affected native targets, documentation validation, the
  required full `all` build for the editor-visible change, and editor startup
  under repository guidance.

#### Acceptance Gate

- All correctness suites report zero compare mismatches and retain exact M1-M2
  selection/lifetime behavior.
- Scene and StaticMesh candidate-reduction, memory, and timing gates pass, or a
  measured threshold revision is recorded and re-approved in Stage 0 before
  qualification is repeated.
- Focused validation, full build, and editor runtime startup succeed from one
  Agent Build Profile.

### Stage 5: Publish the M3 contract and disposition conditional GPU work

- [ ] Move lasting scene-index ownership, mutation, bounds, BVH lifetime,
  fallback, diagnostics, and ordering rules into Viewport Editing Architecture
  and the appropriate Engine/StaticMesh documentation.
- [ ] Record the final fixture measurements, budgets, counters, and qualification
  evidence in this plan; close only evidence-backed checks.
- [ ] Mark M3 complete in the parent roadmap and update its foundation/gap,
  milestone, validation, risk, and completion sections.
- [ ] Review M4-M5 activation evidence using final CPU measurements and explicitly
  activate or defer asynchronous readback and GPU picking without creating a
  conditional plan that lacks its entry gate.
- [ ] Run all-plan, roadmap, and changed-document validation required by the
  documentation lifecycle.

#### Acceptance Gate

- Lasting documentation, this plan, and the parent roadmap agree on the shipped
  CPU acceleration and any measured skeletal disposition.
- M3's roadmap exit gate has concrete parity, update, memory, candidate, timing,
  build, and runtime evidence.
- M4-M5 are either activated with accepted consumer budgets or explicitly
  deferred with final CPU measurements and product constraints.

## Validation Matrix

| Contract | Coverage | Required outcome |
| --- | --- | --- |
| Scene membership | Initial snapshot; add/remove; register cycles; hide/show; Level replacement | Healthy index equals complete brute-force membership; incomplete revision falls back completely |
| Bounds updates | Translation, rotation, non-uniform scale, mirror, reparent, singular/non-finite, pose update | Candidate set is conservative and stale/invalid leaves cannot win |
| Hierarchy behavior | Sparse/dense rays, inside origin, axis-parallel, overlapping/equal bounds, forced reinsertion/rebuild | Broad phase matches bounds oracle independent of build or traversal order |
| Static BVH build | Empty/malformed geometry, degenerates, equal centroids, deep/unbalanced inputs, byte ceilings | Complete valid immutable tree or exact reference fallback; never partial publication |
| Static traversal | Nearest/equal triangles, front/back, transformed shared instances, multiple assets and LODs | LOD0 token and world distance match M1 within the existing epsilon |
| Skeletal disposition | Current bounds, animation, influences, invalid pose, budget boundaries | M2 exact result and atomic failure remain; activated reduction is conservative |
| Semantic competition | Static/Skeletal near/far/equal, visualization, reversed enumeration | Existing distance, layer, priority, and stable-key resolver is unchanged |
| Lifetime | Asset replace/reimport/release, component destroy, Level/client/context shutdown, pending result | No stale BVH/index/request identity resolves or extends object lifetime |
| Fallback | Index/BVH unavailable, stale, over budget, allocation/build failure, parity mismatch | Complete reference result is returned and diagnostic reason is conserved |
| Multi-viewport | Two clients sharing one context/index with independent views and tickets | Shared acceleration does not share request, cancellation, or winner state |
| Performance | Required workload matrix and warm distributions | Numeric candidate, memory, build/refit, small-regression, and large-speedup gates pass |
| User-visible qualification | Targeted native tests, docs, full build, runtime startup | Repository handoff rules pass from one Agent Build Profile |

## Definition of Done

- All Stage 0-5 checklist items and acceptance gates are complete with recorded
  evidence.
- Healthy scene picking no longer scans every Level Actor/component per request;
  it queries one current shared Level index and freezes only ray candidates into
  the M1 request table.
- StaticMesh LOD0 picking traverses immutable revision-matched asset BVHs and
  preserves exact M1 world-distance and stable winner behavior.
- SkeletalMesh uses current pose bounds through the index and follows the
  evidence-backed Stage 3 disposition without weakening M2 deformation,
  validity, ordering, or budget failure semantics.
- Reference, accelerated, and compare modes exist behind the private backend
  seam; qualification reports zero semantic parity mismatches.
- Add/remove/transform/visibility/asset/pose updates, Level replacement,
  multiple viewports, cancellation, and shutdown cannot expose stale identity
  or retained reflected objects.
- Scene and asset acceleration obey measured hard memory ceilings and complete
  reference fallback; diagnostics conserve builds, mutations, bytes, visits,
  candidates, tested work, fallbacks, and mismatches.
- Representative candidate-reduction and timing gates pass, focused native
  validation is green, and the required full editor build and runtime startup
  succeed.
- Lasting contracts are published, M3 is complete in the roadmap, and M4-M5 are
  explicitly activated or deferred from final evidence.

## Deferred Follow-ups

- Visible/render-selected LOD picking remains a separately gated semantic change;
  M3 prepares per-LOD StaticMesh data but continues querying LOD0.
- Persisting ray-query BVHs in cooked or derived asset data requires format
  versioning, cache compatibility, build pipeline, and non-editor consumers.
- Collision/physics/navigation reuse requires a second concrete consumer and an
  ownership plan; M3 private utilities do not become a universal query service.
- Asynchronous bounded-region readback and GPU viewport picking remain
  conditional M4-M5 work after Stage 5 reviews the CPU evidence.
- Bone, section, instance, material, vertex, and triangle selection remain
  consumer-driven extensions to the semantic result contract.

## Related Documentation

- [Viewport Picking Roadmap](../Roadmaps/ViewportPicking.md)
- [Viewport Picking Contract Plan](ViewportPickingContract.md)
- [Skeletal Viewport Picking Plan](SkeletalViewportPicking.md)
- [Viewport Editing Architecture](../Editor/Architecture/ViewportEditing.md)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorContext.h`
- `Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorContext.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingService.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingService.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Level.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Level.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/SkeletalMeshComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/SkeletalMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshResources.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportPickingContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportInteractionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshEditorTests.cpp`
