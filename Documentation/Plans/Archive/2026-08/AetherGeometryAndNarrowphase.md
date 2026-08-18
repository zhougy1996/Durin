# Aether Geometry And Narrowphase Plan

Summary: Add immutable shared collision geometry and pair dispatch, complete primitive and compound queries, and replace the pathological production Capsule/Box search without changing the World or M1 scene boundary.

Last reviewed: 2026-08-12

Status: Archived
Completed: 2026-08-12

## Current Status

Activated as M2 of the
[Aether Physics Evolution Roadmap](../../../Roadmaps/Archive/2026-08/AetherPhysicsEvolution.md) after
M1 completed at source revision `07b9bc567b0deaa3b744755047d14f89a4711dce`.
The entry evidence is sufficient: generation-checked body storage and both
spatial indexes are qualified; Reference, Production, and Compare have a
zero-mismatch corpus; query and geometry work are independently counted; and
the production boundary reaches narrow phase only after bounded traversal and
filtering.

Completed at source revision `82fef8cb`. Bodies now retain immutable
primitive or 1-64 child compound resources; BodySetup publishes one identity
per revision; AetherCore owns the complete primitive operation/pair facade;
and Production Capsule/Box uses exact piecewise segment/box distance with
bounded advancement. The Win64 layout is 176 bytes per body record, 16 bytes
per reference, 112 bytes per child, and 208/7,264 retained bytes for 1/64
children. Debug full native validation passes after a successful full `all`
build. Focused Release PhysicsScene and Sandbox suites pass; the controlled
sparse pair median is 437 ns Production versus 11,000 ns reference (25.17x).
The roadmap now names M3 cooked world collision as the next required milestone
with immutable resource identity, child bounds/order, operation/pair dispatch,
and diagnostics as its entry handoff. M3 cooking, simulation, and concurrency
remain inactive pending their own accepted plans.

## Goal

Make geometry ownership and narrow-phase selection independently extensible
without changing `DWorld` queries, M1 traversal, component ownership, or
stable result ordering. At completion:

- bodies share immutable primitive or compound payloads while retaining their
  own transforms, filters, motion, handles, and tokens;
- Raycast, Sweep, and Overlap support the full Box/Sphere/Capsule target matrix;
- scene code invokes one operation/pair facade and contains no pair algorithms;
- common primitive pairs use qualified fast paths and generic convex support is
  bounded, observable fallback capability; and
- Sandbox Capsule/Box production queries no longer run the nested reference
  search.

## Scope

- AetherCore immutable geometry references, validated primitive/compound
  resources, stable identity, child transforms/order, bounds, and memory facts.
- Ray against Box/Sphere/Capsule and every directed Box/Sphere/Capsule Sweep and
  Overlap pair.
- Analytic fast paths, dedicated Capsule/Box distance/cast, bounded generic
  convex distance/penetration/cast, explicit convergence status, and reference
  fallback.
- Aether body references, aggregate bounds, dispatch, compound accumulation,
  stable internal ties, and reconciled pair/iteration diagnostics.
- `DBodySetup` publication of one cached immutable resource per revision while
  preserving its existing single-shape authored API.
- Golden, randomized, adversarial, memory, lifecycle, Release cost, and Sandbox
  movement qualification plus lasting documentation and M3 handoff budgets.

## Non-Goals

- Convex-hull or triangle-mesh cooking, serialization, DDC, import invalidation,
  asset BVHs, render-mesh reuse, inspection, or simple-versus-complex policy.
- Reflected compound authoring or persisted per-child material/filter metadata;
  M2 qualifies programmatic low-level compounds.
- Heightfields, planes, deformable/procedural geometry, or angular casts.
- Manifolds, persistent contacts, simulation, forces, materials, or events.
- Batch/worker queries, snapshots, runtime registration, backend interfaces, or
  third-party geometry libraries.
- World API, filtering, ignore, public hit ordering, or one-result-per-body
  changes.

## Design Decisions and Invariants

### Geometry is an immutable AetherCore resource

- `FCollisionShape` remains the compact by-value simple query shape. A new
  copyable `FCollisionGeometryRef` is an opaque owning reference to a `const`
  AetherCore payload; only validated factory functions create it.
- Each payload has a non-zero process-local identity used for update detection
  and diagnostics, never serialization or gameplay results.
- A primitive resource contains one shape. A compound contains 1 through 64
  simple children in stable input order, each with one valid local transform.
  M2 compounds do not nest.
- Creation precomputes immutable local bounds and retained bytes. Scene records
  copy the reference, not child data. Normal reference lifetime handles body
  removal and World teardown; no registry or Engine callback owns destruction.
- `DBodySetup` owns a transient cache keyed by authored `Revision`. Existing
  setters invalidate it; repeated publication of one revision returns the same
  resource identity on the existing game-thread path.

### Transform and scale semantics remain compatible

- Transforms remain finite, normalizable, and strictly positive scale. Box
  scale is component-wise; Sphere uses maximum XYZ scale; Z-axis Capsule uses
  maximum XY radius scale and Z half-height scale clamped to its radius.
- Child world transform is `FTransform::Combine(Body, Child)`. Bounds and narrow
  phase use that same combined value, preventing broad/exact disagreement.
- Existing queries continue to accept one simple query shape; compounds are
  body targets only. Geometry identity, transform, and motion are spatial state;
  filter/token-only updates do not touch either index.

### One dispatcher owns operation and pair selection

- AetherCore owns a closed compile-time table indexed by Raycast/Sweep/Overlap,
  query primitive where applicable, and target leaf type. Runtime registration
  and virtual geometry objects are excluded.
- Its internal status distinguishes Hit, Miss, Invalid, Unsupported, and
  NonConverged. Scene bool APIs remain unchanged, but diagnostics retain the
  richer result.
- `FPhysicsScene` keeps validation, traversal, filtering, and body accumulation,
  then invokes the facade. Adding a future type changes geometry/dispatch and
  bounds code, not scene traversal, World, or components.
- Compound dispatch walks stable child order. Closest queries select
  `(Time, child index)` before the scene's `(Time, body handle)` rule. Overlap
  emits one result per body using the lowest overlapping child for contact
  fields. M2 does not expose child identity publicly.
- Unsupported or non-converged Production work records the reason and executes
  the complete bounded Reference operation for that body; it never becomes a
  silent miss. Invalid resources cannot enter the scene.

### Fast paths and generic convex have distinct jobs

- Ray/Box stays the oracle; Ray/Sphere and Ray/Capsule use analytic finite-
  segment intersections.
- Analytic/finite-feature paths own qualified Sphere/Sphere, Sphere/Box,
  Sphere/Capsule, Capsule/Capsule, and Box/Box operations. Symmetric overlap
  wrappers reverse normals deliberately.
- Capsule/Box overlap and translational cast use an exact bounded segment/box
  distance-feature evaluation and must not call the current per-sample
  28-iteration ternary search.
- Generic convex uses bounded GJK distance/intersection, a qualified
  penetration construction, and conservative advancement for remaining casts.
  It fills selected matrix cells or explicit recovery; it does not displace
  faster qualified paths for uniformity.
- Stage 0 freezes caps after adversarial evidence. Initial maxima are 32 GJK,
  32 penetration, and 32 cast iterations. The existing nested Capsule/Box path
  remains only as Reference evidence and explicit final fallback.

### Contacts, tolerances, and ordering are frozen

- Contact tolerance stays `1.0e-8`; Compare stays `1.0e-12` for normalized time
  and `1.0e-8` for distance, positions, normal, and depth.
- `Time` is `[0,1]`; `Distance` is translation length times time; `Location` is
  the query origin at time; `ImpactPoint` is on the selected target feature;
  and the unit normal points from target toward query. Initial overlap sets zero
  time/distance, `bStartPenetrating`, and non-negative depth.
- Tangency remains a ray/sweep contact but Overlap requires positive
  penetration beyond tolerance. Zero-delta Sweep returns only an initial
  penetration and does not adopt Overlap result ordering.
- Coincident/equal-feature cases use golden-tested axis precedence. Results do
  not depend on allocation, table, child storage, or traversal order.

### Diagnostics remain reconciled and bounded

- One body candidate still increments `NarrowPhasePairTests` once. Separate
  counters report leaf tests, compound children, analytic/generic dispatch,
  support evaluations, distance/penetration/cast iterations, unsupported,
  non-convergence, and reference pair fallback.
- Counter saturation still sets the snapshot overflow bit. Ordinary queries use
  fixed value/stack scratch and allocate neither per pair nor by scene size;
  Overlap output remains the only result-sized growth path.
- Disabled detailed diagnostics add no clocks, strings, logs, callbacks, or
  allocations to a pair test.

## Current Foundations and Gaps

| Area | Existing foundation | M2 gap |
| --- | --- | --- |
| Scene | M1 bounded traversal and stable accumulation | Six executors name concrete pair functions |
| Geometry | Valid Box/Sphere/Capsule value | Embedded per body; no identity or compound |
| Targets | Rotated positive-scale Box | Sphere/Capsule incomplete |
| Capsule movement | Qualified oracle and work counters | Nested search remains Production |
| Generic convex | Core math | No support mapping or convergence status |
| Engine | Shared BodySetup and revision | Rebuilds a shape per component publication |
| Oracle | Reference/Production/Compare corpus | Both paths call the same pair function |
| Diagnostics | Pair/distance/search counters | No algorithm/leaf/convergence attribution |

## Implementation Stages

### Frozen M2 matrix and budgets

The qualified production matrix is closed and complete:

| Operation | Query | Box target | Sphere target | Capsule target |
| --- | --- | --- | --- | --- |
| Ray | finite segment | slab intersection | quadratic segment/sphere | cylinder plus cap intersections |
| Sweep | Box | bounded feature advancement | bounded feature advancement | bounded feature advancement |
| Sweep | Sphere | bounded analytic-feature advancement | bounded analytic-feature advancement | bounded analytic-feature advancement |
| Sweep | Capsule | exact segment/box plus bounded advancement | bounded analytic-feature advancement | bounded analytic-feature advancement |
| Overlap | Box | 15-axis SAT | point/box feature distance | exact segment/box feature distance |
| Overlap | Sphere | point/box feature distance | center distance | point/segment distance |
| Overlap | Capsule | exact segment/box feature distance | point/segment distance | segment/segment distance |

Every cell is qualified by a directed matrix golden plus contact invariants;
Capsule/Box additionally retains the old nested oracle as direct evidence.
Ray/Sweep tangency, strict Overlap, zero delta, initial penetration, rotation,
non-uniform scale, compound child order, and fixed-seed scene permutation are
covered by focused tests.

Production caps are 32 advancement iterations, 32 reserved generic-convex
iterations, 32 reserved penetration iterations, 96 feature evaluations, and 64
combined search/cast iterations. Unsupported and NonConverged are explicit;
qualified Capsule/Box recovery records a reference fallback. The measured
Win64 layout is a 16-byte reference, 112-byte child, 96-byte payload header,
208 retained bytes for one primitive, 7,264 retained bytes for 64 children,
176-byte body record, and 12-byte slot. Invalid/empty geometry allocates no
payload; valid primitive and compound creation retain one shared header/control
allocation and one exactly reserved child-array allocation.

### Stage 0: Freeze resources, pairs, contacts, and budgets

- [x] Map every directed Ray/Sweep/Overlap primitive cell to its production
  algorithm and independent oracle, golden, or invariant qualification.
- [x] Add goldens for miss, tangency, penetration, coincidence, zero motion,
  rotation, non-uniform scale, large coordinates, and equal features.
- [x] Prototype exact segment/box and generic convex distance, penetration, and
  cast paths outside scene dispatch; compare with the retained oracle.
- [x] Freeze caps, statuses, fallback, and Release work/timing gates.
- [x] Measure current layouts and 0/1/64-child allocation; freeze body-record,
  reference, header, child, and retained-memory budgets.
- [x] Freeze compound validation/precedence and adjust this plan before Stage 1
  if evidence changes a selected algorithm or budget.

#### Acceptance Gate

- Every matrix cell has one production path and independent qualification; no
  cell silently means unsupported.
- The Capsule/Box prototype matches goldens without nested ternary search;
  ordinary qualified casts stay within 96 feature evaluations and 64 total
  search/cast iterations, with adversarial cases ending in a named status.
- Convex caps, contact precedence, and layout/memory budgets are recorded and
  deterministic under operand/fixture permutation.

### Stage 1: Add shared primitive and compound resources

- [x] Implement the opaque reference and validated factories with identity,
  bounds, retained bytes, and test-only immutable inspection.
- [x] Replace body `Shape` with geometry; update validation, spatial-state
  comparison, exact bounds, snapshots, lifecycle, and both indexes.
- [x] Union exact transformed child bounds for compound world bounds.
- [x] Add the revision-keyed transient `DBodySetup` cache without changing its
  authored shape setters.
- [x] Test sharing, revision replacement, teardown, invalid/oversized compounds,
  child order, stale handles, and diagnostics.

#### Acceptance Gate

- 10,000 bodies from one BodySetup retain one payload identity and no per-body
  child copy while keeping distinct handles/transforms.
- Primitive and 1/64-child bounds have zero sampled false negatives; geometry
  updates touch only the affected proxy and filter/token updates do no spatial
  work.
- The dense record stays at or below M1's 192 bytes; per-body ownership adds at
  most two machine words and resource allocation meets Stage 0's gate.

### Stage 2: Centralize dispatch and analytic primitive queries

- [x] Add the operation/pair table, explicit status, facade, counters, and
  geometry-owned profiling names.
- [x] Implement analytic Ray against every primitive and the selected analytic
  overlap/sweep cells with deliberate symmetry wrappers.
- [x] Move compound iteration and internal winner selection into the facade.
- [x] Replace all scene pair switches/calls with the facade while keeping
  distinct Reference and Production algorithm selection.
- [x] Add table-completeness, symmetry, contacts, compound ties, and fault tests.

#### Acceptance Gate

- Every directed matrix cell reaches its documented implementation and passes
  hit status plus all public contact fields.
- Scene traversal contains no concrete geometry/pair algorithm switch; a
  test-only table extension needs no traversal, filter, World, or component edit.
- Compounds emit one stable body result independent of child allocation and
  candidate order, with existing World/Sandbox behavior preserved.

### Stage 3: Add bounded convex and fast Capsule/Box production

- [x] Implement Box/Sphere/Capsule support mappings under frozen scale semantics.
- [x] Implement bounded convex distance/intersection, penetration, and
  translational cast with deterministic simplex/feature precedence.
- [x] Implement dedicated exact-distance Capsule/Box overlap/cast and route the
  qualified directions to it.
- [x] Prefer analytic dispatch; invoke convex and Reference fallback only in
  their recorded roles.
- [x] Add fixed-seed randomized and adversarial permutation, scale, tangent,
  deep-penetration, long/zero-delta, cap, and fault tests.
- [x] Reconcile leaf/algorithm/iteration/fallback counters.

#### Acceptance Gate

- All primitive/compound fixtures have zero unexplained Compare mismatch;
  non-convergence is counted and complete fallback is fault-qualified.
- Sandbox Capsule/Box invokes no nested Production search, meets Stage 0 work
  caps, and improves controlled per-pair Release median by at least 4x.
- Ordinary qualified matrices record zero unsupported, non-converged, or pair
  fallback outcomes.

### Stage 4: Qualify scene memory, lifecycle, and gameplay

- [x] Run 0/32/1,000/10,000 primitive/shared-compound scenes through all policies
  under sparse, dense, clustered, adversarial, and mutation workloads.
- [x] Recheck M1 candidates, mutation isolation, memory, closest pruning, dense
  overlap, scratch fallback, and aggregate-bound false negatives.
- [x] Record controlled Release matrix-family, Capsule/Box, and Sandbox timings,
  separating traversal, leaf, support, iteration, and fallback cost.
- [x] Verify BodySetup/BodyInstance/component publication, Level/World teardown,
  PIE/standalone, collision debug capture, and default Box content lifetime.
- [x] Use focused native targets during work. Since final scope crosses
  AetherCore, Aether, Engine publication, and Sandbox, run the root-required
  full native validation before handoff.

#### Acceptance Gate

- Qualified queries have zero mismatch, unsupported, non-convergence, overflow,
  or unexpected fallback; dense result count/order remains exact.
- Memory is proportional to unique resources plus body references, not instances
  times children; all M1 scene gates still pass.
- Sandbox movement/lifecycle/debug behavior passes without a World API or
  user-visible collision regression.

### Stage 5: Publish contracts and complete M2

- [x] Move shipped ownership, matrix, compound, dispatch, contact, convergence,
  fallback, and diagnostics rules into Runtime Collision documentation.
- [x] Update the roadmap with M2 evidence and M3 entry constraints without
  activating M3 prematurely.
- [x] Record source revision, layouts, memory, work, timing, parity, fallback,
  and validation in Current Status.
- [x] Complete checklists/lifecycle metadata and run required plan, roadmap, and
  repository documentation validators.
- [x] Stage and commit the isolated implementation/tests/docs with M2 plan and
  stage provenance.

#### Acceptance Gate

- Primitive/compound parity, Capsule movement cost, dispatch extensibility,
  sharing, memory, diagnostics, gameplay, and final validation are evidenced.
- Runtime docs are authoritative, this plan is completed, and the roadmap names
  M3 next with a concrete immutable-geometry/cook handoff.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Resource | Default/invalid, empty, 1/64/65 children, bad transform, identity/order, immutable bounds, bytes |
| Sharing | Same revision, 10,000 instances, revision replacement, update/remove, World teardown |
| Primitive pairs | All Ray targets and directed Sweep/Overlap cells: miss, tangent, hit, penetration, zero delta, rotation, scale |
| Compound | Bounds, first/middle/last and multiple hits, child tie, one body result, filtering/order |
| Contacts | Normal direction/length, target point, query location, time/distance, depth, coincidence, finiteness |
| Convex | Simplex/support, symmetry, degeneracy, large coordinates, caps, non-convergence, fallback |
| Capsule/Box | Oracle parity, no nested Production search, work/time gate, glancing/parallel/long casts, Sandbox |
| Scene | All policies/partitions, pruning, dense overlap, churn, revision update, scratch fault, fallback |
| Diagnostics | Reconciliation, attribution, saturation, reset/capture, disabled cost, forced faults |
| Compatibility | World APIs, filters/ignore, handle ties, setters, lifecycle, debug, PIE/standalone, Sandbox |

## Definition of Done

- Bodies reference validated immutable primitive/compound geometry without
  per-instance payload copies and with deterministic identity, bounds, lifetime,
  and memory accounting.
- All primitive Ray/Sweep/Overlap pairs and compound targets produce stable,
  qualified contacts through one AetherCore dispatcher outside scene traversal.
- Analytic paths remain preferred, convex work is bounded, and Production
  Capsule/Box removes the pathological reference search.
- Tolerance, scale, contact, convergence, fallback, and ordering pass golden,
  randomized, adversarial, and Compare qualification with zero unexplained
  mismatch.
- Diagnostics/memory reconcile, M1 and gameplay gates do not regress, and
  lasting docs, roadmap, validation, lifecycle, and committed handoff complete.

## Deferred Follow-ups

- M3 convex/triangle cook payloads, asset BVHs, serialization/DDC, invalidation,
  policy, inspection, and platform/version failure rules.
- Reflected compound authoring and child material/filter metadata until a
  persisted consumer contract exists.
- Public child/feature hit identity, manifolds/caches, simulation/events,
  angular casts, parallel queries/snapshots, and backend providers until gated.

## Related Documentation

- [Aether Physics Evolution Roadmap](../../../Roadmaps/Archive/2026-08/AetherPhysicsEvolution.md)
- [Aether Scene Query Acceleration Plan](AetherSceneQueryAcceleration.md)
- [Aether Physics Query Observability Plan](AetherPhysicsQueryObservability.md)
- [Physics Scene And Character Collision Plan](PhysicsSceneAndCharacterCollision.md)
- [Runtime Collision](../../../Runtime/Physics/Collision.md)
- [Core Math](../../../Runtime/Core/Math.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Sandbox Gameplay](../../../Runtime/Gameplay/SandboxGameplay.md)
- [Native Tests](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/AetherCore/Public/Collision/CollisionShape.h`
- `Engine/Source/Runtime/AetherCore/Public/Collision/CollisionGeometry.h`
- `Engine/Source/Runtime/AetherCore/Private/Collision/CollisionGeometry.cpp`
- `Engine/Source/Runtime/AetherCore/Public/Physics/PhysicsTypes.h`
- `Engine/Source/Runtime/Aether/Public/Physics/PhysicsScene.h`
- `Engine/Source/Runtime/Aether/Private/Physics/PhysicsScene.cpp`
- `Engine/Source/Runtime/Engine/Public/Physics/BodySetup.h`
- `Engine/Source/Runtime/Engine/Private/Physics/BodySetup.cpp`
- `Engine/Source/Runtime/Engine/Public/Physics/BodyInstance.h`
- `Engine/Source/Runtime/Engine/Private/Physics/BodyInstance.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Tests/Native/EngineTests/Private/Physics/PhysicsSceneTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Physics/PhysicsQueryObservabilityTests.cpp`
- `Sandbox/Tests/Native/Private/SandboxGameplayTests.cpp`
