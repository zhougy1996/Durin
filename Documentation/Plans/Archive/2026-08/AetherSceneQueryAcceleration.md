# Aether Scene Query Acceleration Plan

Summary: Replace Aether's linear body lifecycle and Production candidate walk with generation-checked dense storage and a deterministic static/moving broad phase while preserving the M0 query oracle.

Last reviewed: 2026-08-11

Status: Archived
Completed: 2026-08-11

## Current Status

Completed as M1 of the
[Aether Physics Evolution Roadmap](../../../Roadmaps/AetherPhysicsEvolution.md) from
source revision `07b9bc567b0deaa3b744755047d14f89a4711dce`. The implementation
adds generation-checked dense slots, explicit Static/Kinematic/Dynamic
publication, outward-rounded compact exact bounds, a deterministic static BVH,
an incremental moving fat-AABB tree, strictly-safe closest pruning, fixed
128-entry traversal scratch, and complete Reference fallback.

The qualified layouts are a 192-byte dense record, 12-byte slot, and 36-byte
node. Actual 0/32/1,000/10,000 all-static, all-moving, and mixed capacities fit
`64 * live bodies + 64 KiB`; the 10,000-moving measurement retained 655,360 M1
bytes against a 705,536-byte gate. On `Win64-Release-DurinEditor`, 10,000-body
sparse LineTrace and Sweep misses both emitted zero candidates and measured
0.120/0.300 microseconds median. Dense overlap returned all 10,000 results. The
32-body update P95 was 0.017 microseconds; 10,000-body update and stable
remove/add P95 were 0.030/1.430 microseconds. Compare recorded zero mismatch,
and qualified runs recorded zero scratch overflow or spatial fallback.

Focused Debug and Release PhysicsScene tests, focused Debug and Release Sandbox
gameplay tests, and the final default-profile native `--target all` passed. M2
geometry and narrow-phase architecture is next; no M2 algorithm was selected
by this plan.

## Goal

Make Aether body lookup and lifecycle independent of scene size and make normal
Production queries test bodies near the finite query volume rather than every
body, without changing Engine-facing World query APIs or any M0 result
semantics.

At completion, stale handles fail in constant time, Static and moving bodies
are indexed independently, conservative bounds produce no false negatives,
single-result queries traverse with bounded reusable scratch and closest-hit
pruning, and Production has zero unexplained mismatch against the retained flat
Reference executor across the qualified corpus.

## Scope

- AetherCore motion classification and conservative world/query AABBs for the
  existing Box, Sphere, and Capsule shape values and transform rules.
- Generation-checked slot identity, dense body records, free-slot reuse,
  swap-remove bookkeeping, and amortized constant-time add, lookup, update, and
  removal excluding spatial-index maintenance.
- Explicit Static, Kinematic, and reserved Dynamic motion values independent
  of collision channel, collision enabled mode, and response profile.
- Engine publication of Static for qualified immutable world geometry and
  Kinematic for component-driven moving/query bodies without a new editor UI.
- A deterministic rebuildable static BVH and an incremental moving fat-AABB
  tree behind one Aether-private candidate traversal contract.
- Transactional partition insertion, removal, migration, transform/geometry
  update, and filter-only update behavior.
- Finite LineTrace segment bounds, conservative swept-shape bounds, overlap
  bounds, node near-time tests, and closest-hit pruning.
- Bounded reusable traversal scratch, explicit overflow/fallback behavior, and
  no candidate vector proportional to total bodies for ordinary single queries.
- Partition, storage, bounds, node, mutation, memory, traversal, pruning,
  reinsertion, rebuild, scratch, and fallback diagnostics integrated with M0.
- Characterization, fixed-seed parity, sparse/dense/adversarial scale,
  mutation-heavy, lifecycle, and real Sandbox movement qualification.

## Non-Goals

- Changing World trace/sweep/overlap APIs, public result fields, collision
  profiles, ignored-body behavior, tolerances, or stable ordering.
- Replacing the M0 Reference executor, comparison matrix, Production default,
  reference-on-mismatch policy, or bounded diagnostic capture contract.
- Optimizing Ray/Box or Capsule/Box narrow phase, changing contact tolerances,
  or reducing the current 28-iteration reference searches. Those remain M2.
- Adding compounds, convex algorithms, triangle meshes, cooked collision,
  asset BVHs, physical materials, per-face results, or heightfields.
- Adding rigid-body state, integration, forces, contacts, constraints,
  sleeping, CCD, events, or moving-platform gameplay policy. Dynamic is only a
  reserved moving-partition value in M1.
- Adding batch queries, worker threads, command buffers, committed snapshots,
  asynchronous APIs, or backend/provider interfaces.
- Reusing the LevelEditor picking index or moving editor visibility, primitive
  identity, weak object ownership, or request lifecycle below Engine.
- Making motion type an alias for object channel, response profile, Actor
  class, or collision enabled mode.
- Exposing nodes, proxies, dense/slot indices, or mutable diagnostic storage
  through Engine components or World APIs.
- Adding a reflected motion editor control, serialization migration, or new
  collision visualization in M1.

## Design Decisions and Invariants

### Ownership and compatibility boundary

- The dependency direction remains `Core -> AetherCore -> Aether -> Engine`.
  AetherCore owns motion and conservative-bounds values/functions. Aether owns
  slots, dense storage, partitions, indexes, traversal, scratch, and scene
  diagnostics. Engine publishes motion with body descriptions and continues to
  resolve opaque user tokens.
- `FPhysicsScene` remains the complete public Aether facade. Storage and index
  types stay private and create no dependency on Engine, LevelEditor, Renderer,
  or AssetCore.
- Reference and Production read the same validated dense records. Reference
  keeps a complete flat walk; Production alone uses the indexes. Compare still
  executes both synchronously against one unmutated scene and returns Reference
  on any complete-output mismatch.
- Closest hits remain ordered by exact normalized `Time` and then the complete
  handle; OverlapMulti remains ordered by handle. Tree, partition, insertion,
  traversal, and swap-remove order are never semantic tie-breaks.

### Generation-checked dense body storage

- A valid handle identifies a slot plus its non-zero generation. Each live slot
  resolves directly to one dense record, and the record retains its owning
  slot. Removal swap-removes the last record and patches exactly one moved slot.
- Retiring a slot invalidates the old generation before free-list admission.
  Reuse advances the generation; zero and wraparound are reserved. A slot whose
  next generation is not representable is retired permanently.
- Add constructs the body, exact conservative AABB, partition proxy, slot, and
  record transactionally. Update publishes the complete new descriptor/bounds/
  proxy state or retains the complete old state. Remove cannot leave an orphan
  proxy, slot, or record.
- `ContainsBody`, lookup, update, and remove perform bounded slot validation
  independent of dense count. Reference iteration remains dense but is not a
  handle lookup mechanism.
- Handle reuse and swap-remove have explicit equal-time, ignored-body,
  CaptureBodies, component-token, and Compare fixtures. No hidden registration
  serial replaces the documented handle tie-break.

### Explicit motion publication

- AetherCore adds `EPhysicsBodyMotionType` with fixed Static, Kinematic, and
  Dynamic values, and `FPhysicsBodyDesc` carries one value. Direct low-level
  descriptions default to Kinematic so existing mutable callers do not
  silently imply immutable storage.
- `FBodyInstance` owns an explicit non-reflected M1 motion value and publishes
  it in creation and update descriptions. It is not inferred from channel,
  profile, collision enabled state, or Actor type at the Aether boundary.
- Qualified `AStaticMeshActor` world geometry explicitly publishes Static.
  Component-driven shapes, including the Sandbox pawn Capsule, publish
  Kinematic. Dynamic is accepted and queried in the moving partition but has
  no simulation or Engine authoring behavior in M1.
- Motion change is one transactional partition migration. Filter-only change
  preserves the proxy and performs no rebuild/refit/reinsertion. Transform or
  shape change recomputes bounds and touches only the current partition.

### Conservative shape and query bounds

- Each record owns one finite tight conservative world AABB distinct from any
  moving fat proxy. Its representation may round outward, never inward, and
  its retained storage counts against the M1 memory gate.
- Box bounds use the absolute normalized rotation basis applied to positively
  scaled half extents. Sphere bounds use the conservative maximum-axis scale.
  Capsule bounds use the same scaled radius, half height, rotation, and segment
  endpoints as the M0 Capsule geometry path.
- LineTrace covers the finite closed segment. Sweep uses the conservative union
  of start and translated-end shape bounds. Overlap uses the query shape AABB.
  Bounds include the frozen contact tolerance or a recorded outward-rounding
  allowance so grazing contacts are not culled.
- Invalid or overflowing bounds reject the mutation/query; they are never
  clamped to a smaller box. False positives are legal and counted. One false
  negative relative to Reference is a correctness failure.

### Hybrid broad phase and mutation policy

- Static bodies use a deterministic contiguous rebuildable BVH. Stage 0 freezes
  fanout, leaf capacity, compact outward-rounded nodes, split/tie rules, height,
  build policy, and exact retained layout.
- Kinematic and Dynamic bodies share a node-pooled dynamic AABB tree. Exact
  bounds remain in the record; proxies use deterministic fat bounds. Contained
  updates avoid reinsertion; escaping updates remove and reinsert one proxy and
  perform only bounded ancestor refit/rotation work.
- Static mutations may rebuild only Static. Ordinary moving updates never
  rebuild Static. Moving add/remove/migration changes only the moving tree.
- Static construction and dynamic insertion/rotation ties use stable handles
  after geometric keys. Rebuild output and candidate sets do not depend on
  unordered-container iteration.
- The completed editor picking index is evidence for deterministic centroid
  construction, fat bounds, and diagnostics, but its snapshots, weak identity,
  full rebuild on escape, ray-only traversal, and 384-byte layout are not copied.
- Allocation/build failure preserves correctness through Reference and records
  degraded fallback. This is an exceptional fail-safe, not a supported
  unbounded flat Production mode; qualified scales must record zero fallback.

### Traversal, pruning, scratch, and ordering

- Production builds one immutable query bound, traverses both partitions,
  resolves each proxy through its checked slot, applies exact body-AABB
  intersection, and streams the record into existing ignore/filter/pair/result
  stages. It does not materialize or sort all scene candidates.
- LineTrace/Sweep node tests compute a conservative near time. Children may be
  visited near-first; pruning is legal only when node near time is strictly
  greater than the current exact best hit time. Equal times remain traversable.
- Overlap cannot prune by a winner. It streams results and retains the M0 final
  handle sort. Result storage may grow with actual hits; hidden candidate
  storage may not grow with total bodies.
- Each partition uses fixed-capacity or scene-retained reusable scratch whose
  capacity and maximum supported height are frozen in Stage 0. Ordinary queries
  perform no candidate/node-stack heap allocation. Overflow is counted and
  returns the complete Reference result, never an incomplete result.
- M0 test faults continue to prove candidate omission, reversal, and output
  corruption detection. Candidate order may change work, never semantics.

### Diagnostics and budget accounting

- Existing equations remain valid except that Production no longer visits the
  total body count. A Production body visit is one checked live proxy surviving
  partition traversal; every visit is one candidate, and `Candidates =
  IgnoredBodies + FilterRejectedBodies + NarrowPhasePairTests` remains exact.
- New values distinguish live/free slots, reuse/exhaustion, dense swaps,
  Static/Kinematic/Dynamic counts, bound builds, static builds, moving insert/
  remove/refit/rotation/reinsert, migrations, filter-only updates, node/bound
  tests, pruning, stale proxies, scratch overflow, fallback, node capacities,
  and retained bytes by storage class.
- Diagnostics remain saturating value-only owning-thread integers. Disabled
  timing adds no allocation, logging, atomics, or scene walk. Capture/reset stay
  O(1), mismatch capacity stays one, and debug capture remains explicit O(N).
- The 64-byte-per-body budget counts every M1-added slot, generation, free-list,
  exact bound, proxy, node, and capacity allocation above the M0 176-byte body
  payload. The 64 KiB fixed allowance includes roots, empty capacities,
  traversal scratch, diagnostics growth, and allocator state. Actual retained
  capacities, not only `sizeof`, must pass.
- Qualification reuses the M0 Release profile, fixtures, warm-up/sample method,
  checksum, and counters. Timing is comparable only on a recorded matching
  environment; semantic, candidate, memory, and mutation gates are mandatory.

## Current Foundations and Gaps

| Area | Existing foundation | M1 gap |
| --- | --- | --- |
| Scene facade | Stable synchronous scene, World queries, thread rejection, and value results | Production still walks every body |
| Oracle | Reference/Production/Compare, complete comparator, parity corpus, and fault injection | No accelerated traversal has qualified |
| Identity/storage | Opaque handle fields and one 176-byte dense-looking vector record | IDs never reuse generations; lookup/update/remove are linear; no slot repair |
| Motion | Components publish mutable body descriptions | No explicit motion value or partition contract |
| Bounds | Shape/transform validation and Core `FBox` | No conservative body/query AABB contract |
| Broad phase | Private Production candidate seam | No partition, proxy, node, pruning, or failure policy |
| Mutation | Synchronous owning-thread calls and counters | No update classification or index diagnostics |
| Scratch | Singles allocate no results; overlaps allocate returned hits | No bounded traversal stack |
| Measurement | Qualified scale/churn/Sandbox evidence | No index memory, node work, candidate reduction, or accelerated timing |

## Frozen Entry Budgets

| Contract | M1 gate |
| --- | --- |
| Sparse candidates | At most 32 at 1,000 bodies and 100 at 10,000 bodies for sparse LineTrace and Sweep misses; no arbitrary cap or false negative |
| Dense results | The 10,000-body dense Overlap returns all 10,000 real results in stable handle order |
| Retained memory | All M1-added retained state is at most `64 * live bodies + 64 KiB` above the M0 payload/fixed diagnostics |
| Small queries | Every qualified 32-body Production median is at most 110 percent of its M0 median |
| Small mutation | 32-body moving Update P95 is at most 0.160 us |
| Large mutation | 10,000-body moving Update P95 is at most 12.600 us; stable-count Remove/Add P95 is at most 10.780 us |
| Large sparse timing | 10,000-body LineTrace median is at most 0.633 ms and Sweep median at most 54.398 ms, with candidate gates also passing |
| Correctness/observation | Zero mismatch/false negatives; M0 equations reconcile; capture/reset O(1); mismatch capacity one |
| Partition behavior | An ordinary moving update performs zero static rebuilds and touches no unrelated proxy |
| Gameplay | Qualified Sandbox movement outcomes and query assertions remain unchanged |

## Implementation Stages

### Stage 0: Freeze identity, bounds, structures, and budgets

Dependencies: completed M0 oracle, counters, fixtures, Release profile, and the
accepted entry budgets.

- [x] Record the source revision and rerun the smallest existing PhysicsScene
  and Sandbox correctness baselines before changing storage or descriptors.
- [x] Add characterization fixtures for handle order, ignored handles,
  equal-time winners, CaptureBodies order, tokens, and lifecycle across slot
  reuse and dense swap-remove.
- [x] Freeze slot encoding, generation advance/exhaustion, free-list ordering,
  removal repair, maximum slots, and failure behavior with executable tests.
- [x] Freeze motion values, Kinematic default, Engine publication points,
  Dynamic reserved behavior, and profile/channel-independent migration.
- [x] Add AetherCore golden bounds for shapes and all query kinds across
  rotation, positive non-uniform scale, zero delta, tangency, large finite
  coordinates, and invalid input.
- [x] Prototype the static BVH and moving fat-AABB layouts in bounded private
  test code; freeze fanout, leaf capacity, split/ties, compact bounds, fat
  margin, sibling selection, rotations/refits, height thresholds, and scratch.
- [x] Measure actual retained capacities at 0/32/1,000/10,000 bodies for
  all-static, all-moving, and representative mixes. Confirm 64 bytes/64 KiB or
  revise this plan and roadmap with evidence before Stage 1.
- [x] Freeze allocation/build failure, stale-proxy, scratch-overflow, fallback,
  diagnostic names/equations, byte accounting, and candidate definition.

#### Acceptance Gate

- Identity, ordering, motion, bounds, trees, failure, scratch, and diagnostics
  are represented by executable characterization or failing reference fixtures.
- One exact static and moving layout fits the accepted memory gate at all
  required mixes, or a measured revision is recorded before implementation.
- M0 correctness stays green and no World API, UI, serialization, narrow-phase,
  simulation, threading, or backend expansion is required.

### Stage 1: Replace linear handle resolution with dense slot storage

Dependencies: Stage 0 handle, motion, memory, ordering, and failure contracts.

- [x] Add the AetherCore motion enum/body field and publish explicit values
  from Engine body instances, StaticMeshActor, and Sandbox paths.
- [x] Implement the generation slot table, dense records, free-list reuse, and
  swap-remove repair while retaining flat deterministic Reference iteration.
- [x] Make add/contains/update/remove validate slots and generations in bounded
  work; reject stale, zero, exhausted, malformed, and invalid handles safely.
- [x] Preserve old state on rejected add/update and retire body/token/debug
  visibility exactly once on successful remove.
- [x] Add slot/reuse/exhaustion/swap/lookup/capacity diagnostics without making
  capture/reset proportional to bodies.
- [x] Cover free-list edges, middle/last removal, repeated stale use, many
  generations, two Worlds, component cycles, Level replacement, and teardown.
- [x] Run the smallest affected PhysicsScene and Sandbox targets before indexes.

#### Acceptance Gate

- Live handles resolve in constant bounded work; stale handles never resolve
  after reuse; swap-remove changes no public identity, result, token, or order.
- Reference, flat Production, and Compare remain identical across M0 plus reuse
  and churn fixtures.
- Storage additions fit Stage 0 accounting and mutation equations reconcile.

### Stage 2: Maintain conservative static and moving indexes

Dependencies: Stage 1 storage and Stage 0 bounds/tree contracts.

- [x] Implement qualified body/query bound builders and store each accepted
  body's finite conservative exact AABB.
- [x] Implement the deterministic contiguous static BVH with selected compact
  nodes, construction policy, capacity accounting, and diagnostics.
- [x] Implement the moving fat-AABB tree with deterministic insertion, bounded
  refit/rotation, contained update, escape reinsertion, removal/free-node reuse,
  height recovery, and retained-byte counters.
- [x] Route all mutation classes and motion migration transactionally; moving
  updates must never rebuild Static.
- [x] Add a brute-force bounds oracle and fixed-seed randomized/adversarial
  membership tests across shapes, partition mixes, orders, removals, and churn.
- [x] Exercise invalid/overflow bounds, allocation/build failure, empty/single,
  coincident/clustered/giant bodies, fat-bound edges, and repeated migrations.
- [x] Reconcile partitions, slots, proxies, nodes, capacities, builds, moving
  maintenance, migrations, and fallback after each mutation cohort.

#### Acceptance Gate

- Partitions are disjoint and their union is the complete live scene.
- The bounds oracle has zero false negatives, deterministic builds, bounded
  moving maintenance, exact byte accounting, and no stale proxy after teardown.
- Filter-only updates do zero spatial work; contained updates avoid reinsertion;
  escaping moving updates touch only their tree; moving never rebuilds Static.

### Stage 3: Accelerate the Production candidate pipeline

Dependencies: Stage 2 indexes and unchanged M0 Production stages/comparator.

- [x] Replace the flat Production walk with streaming traversal of both
  partitions through checked slots and exact body AABBs.
- [x] Implement finite segment, swept-shape, and overlap node tests, near-first
  traversal, and strictly-safe closest-hit pruning.
- [x] Preserve ignore/filter/pair dispatch, geometry counters, result fields,
  winner selection, Overlap sort, and M0 test faults.
- [x] Use frozen reusable scratch with zero ordinary candidate allocation and
  complete Reference fallback on explicit overflow/degraded index.
- [x] Reconcile Reference flat work, Production node/bound/candidate work, and
  Compare totals.
- [x] Run fixed-seed/adversarial and 0/32/1,000/10,000 Compare across all query
  kinds, partition mixes, filter/ignore, ties, dense results, churn, and faults.
- [x] Prove candidate, tree, dense, and partition order cannot change semantics.

#### Acceptance Gate

- Production and Reference have zero mismatch and zero false negatives.
- Sparse fixtures pass 32/100 candidates; dense Overlap returns every result.
- Single queries have no body-proportional scratch; overflow is explicit and
  complete; closest pruning preserves equal-time handle winners.

### Stage 4: Qualify mutation, memory, performance, and gameplay

Dependencies: Stage 3 accelerated Production and reconciled diagnostics.

- [x] Record actual retained capacities for every required scale and partition
  mix, including slots, bounds, proxies, nodes, diagnostics, and scratch.
- [x] Qualify 32-body query and mutation regression ceilings.
- [x] Qualify 10,000-body candidate/timing, dense correctness, filter/ignore,
  moving mutation P95, stable remove/add P95, and static rebuild isolation.
- [x] Add clustered, giant-bound, origin-inside, tangent, long-segment,
  high-churn, and tree-quality workloads.
- [x] Re-run all real Sandbox movement and 30/60/120 Hz fixtures with gameplay
  assertions and partition diagnostics.
- [x] Verify diagnostics overhead, O(1) capture/reset, mismatch capacity, debug
  limits, and zero fallback/overflow at qualified supported scales.
- [x] Record why Aether remains independent from the editor picking index.

#### Acceptance Gate

- Every frozen candidate, memory, small-scene, mutation, 4x speedup, partition,
  diagnostics, parity, and Sandbox gate passes or a measured revision is
  accepted before handoff.
- Structural work proves sparse cost follows local occupancy, not total bodies.
- No qualified query uses fallback, overflows scratch, or omits a body.

### Stage 5: Publish contracts and complete handoff

Dependencies: Stage 4 evidence and every preceding gate.

- [x] Move lasting handle, motion, bounds, partition, mutation, traversal,
  failure, diagnostics, memory, and performance contracts to owning docs.
- [x] Update the roadmap with M1 evidence, mark M2 next, and carry forward
  residual pair/iteration measurements without selecting M2 algorithms.
- [x] Run focused PhysicsScene and Sandbox native targets throughout and after
  documentation updates.
- [x] Run final native `--target all` because M1 crosses AetherCore, Aether,
  Engine body lifecycle, and the separate Sandbox consumer.
- [x] Run changed/all documentation plus all-plan/all-roadmap validation.
- [x] Record final revision, profile, evidence, tests, and deferred limits;
  close passed checklists and mark the plan Completed.
- [x] Run a full `all` build only if scope is revised to add a user-visible
  surface, and then validate the editor executable from the same profile.

#### Acceptance Gate

- Focused/full native and documentation validation, all M1 budgets, lifecycle,
  and parity qualification pass from one coherent handoff revision.
- M2 can start on a stable indexed scene boundary with measured residual pair
  cost and no speculative geometry, simulation, concurrency, or backend seam.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Handle/storage | Reuse, exhaustion, stale/zero handles, swap-remove, equal ties, ignored handles, cross-World teardown, and bounded lookup steps |
| Motion | Fixed values/defaults, Engine publication, no channel/profile inference, migration, and reserved Dynamic queries |
| Bounds | Shape/body and all query AABBs are finite, outward conservative, transform-correct, tangent-safe, and zero-false-negative |
| Static index | Deterministic build, exact membership, compact layout, query quality, static-only rebuilds, fallback, and teardown |
| Moving index | Fat containment, bounded maintenance, reinsertion, node reuse, height recovery, membership, and no static rebuild |
| Query parity | Complete M0 matrix, seeds, adversarial inputs, permutations, pruning, dense results, and fault detection |
| Diagnostics | Query, candidate, pair, geometry, slot, partition, node, mutation, migration, scratch, memory, fallback, and overflow equations |
| Resources | Actual capacities meet `64 * N + 64 KiB`; single scratch is bounded; Overlap grows only with hits; capture/reset O(1) |
| Performance | 32-body regression, candidate gates, 10,000 mutation P95, 4x sparse timing, dense correctness, and adversarial quality |
| Engine/gameplay | Component lifecycle, StaticMeshActor Static, pawn Kinematic, Level/World/PIE lifecycle, debug limits, and all Sandbox outcomes |
| Ownership | No reverse dependency, editor index reuse, render/asset coupling, public index type, reflected UI, workers, or backend interface |

## Definition of Done

- Handles resolve generation-checked slots in bounded work; stale generations
  never resolve and dense swap-remove preserves every live identity/token.
- Motion is explicit and independent of filter state; qualified world geometry
  and moving components publish the intended partition without new UI.
- Body/query bounds are conservative; Static and moving indexes form the exact
  live scene and have zero false negatives.
- Production streams local candidates with bounded scratch and safe pruning;
  Reference remains the oracle and Compare has zero unexplained mismatch.
- Moving/filter mutations perform only their documented spatial work; stale
  proxies cannot survive reuse/teardown; exceptional failure is complete.
- Candidate, memory, small-scene, mutation, 4x, diagnostic, and Sandbox gates
  pass with reproducible evidence.
- Lasting docs, roadmap advancement, focused/full validation, and a clean
  committed handoff are complete.

## Deferred Follow-ups

- M2 immutable geometry, compounds, pair dispatch, complete primitive targets,
  analytic fast paths, convex fallback, contacts, and convergence diagnostics.
- M3 cooked/shared convex and triangle geometry, asset BVHs, DDC/serialization,
  import invalidation, policies, and inspection.
- Dynamic simulation, Engine transform authority/materials/events, parallel
  execution, snapshots, and backend providers until their conditional gates.
- Shared Core spatial primitives until Aether and LevelEditor independently
  prove one consumer-neutral contract; their scene indexes remain separate.

## Related Documentation

- [Aether Physics Evolution Roadmap](../../../Roadmaps/AetherPhysicsEvolution.md)
- [Aether Physics Query Observability Plan](AetherPhysicsQueryObservability.md)
- [Physics Scene And Character Collision Plan](PhysicsSceneAndCharacterCollision.md)
- [Runtime Collision](../../../Runtime/Physics/Collision.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Sandbox Gameplay](../../../Runtime/Gameplay/SandboxGameplay.md)
- [Core Math](../../../Runtime/Core/Math.md)
- [Viewport Picking Spatial Acceleration Plan](ViewportPickingSpatialAcceleration.md)
- [Native Tests](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/AetherCore/Public/Physics/PhysicsTypes.h`
- `Engine/Source/Runtime/AetherCore/Public/Collision/CollisionShape.h`
- `Engine/Source/Runtime/AetherCore/Public/Collision/CollisionGeometry.h`
- `Engine/Source/Runtime/AetherCore/Private/Collision/CollisionGeometry.cpp`
- `Engine/Source/Runtime/Core/Public/Math/Box.h`
- `Engine/Source/Runtime/Aether/Public/Physics/PhysicsScene.h`
- `Engine/Source/Runtime/Aether/Private/Physics/PhysicsScene.cpp`
- `Engine/Source/Runtime/Engine/Public/Physics/BodyInstance.h`
- `Engine/Source/Runtime/Engine/Public/Collision/CollisionTypes.h`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Actors/StaticMeshActor.cpp`
- `Engine/Tests/Native/EngineTests/Private/Physics/PhysicsSceneTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Physics/PhysicsQueryObservabilityTests.cpp`
- `Sandbox/Tests/Native/Private/SandboxGameplayTests.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingSceneIndex.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingSceneIndex.cpp`
