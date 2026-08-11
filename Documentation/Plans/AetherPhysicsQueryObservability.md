# Aether Physics Query Observability Plan

Summary: Establish an instrumented reference/production query pipeline, deterministic parity checking, and representative Aether collision baselines before scene acceleration changes.

Last reviewed: 2026-08-11

Status: Active
Completed:

## Current Status

Selected as M0 of the
[Aether Physics Evolution Roadmap](../Roadmaps/AetherPhysicsEvolution.md).
Implementation has not begun. The baseline source revision is
`cca78dbc30e0cc70a6d64e7a9d12d990c725fa2a`, where the completed first-slice
physics scene stores bodies in one flat vector and executes deterministic
LineTrace, Capsule Sweep, and Capsule Overlap queries by walking every body.

The existing implementation is the semantic oracle for this plan. M0 will
separate reference execution from the future production query pipeline, add
bounded counters and comparison diagnostics, and record small, sparse, dense,
mutation-heavy, and Sandbox movement baselines. It will not implement a scene
index, replace current geometry algorithms, or claim production speedup.

## Goal

Make every future Aether query optimization measurable and correctness-checked
without changing Engine-facing collision behavior.

At completion, one immutable query input can execute through a retained flat
reference path, the production pipeline, or both in Compare mode. Diagnostics
must explain validation, body visits, ignored/filter-rejected bodies,
narrow-phase pair work and iterations, hits/results, mismatch/fallback state,
and bounded capture cost. Representative checked-in fixtures and recorded
baselines must identify whether cost comes from scene traversal or geometry
math before M1 selects a broad phase.

## Scope

- Private Aether query-pipeline orchestration behind the existing
  `FPhysicsScene` public query methods.
- A retained deterministic flat reference path preserving current query and
  result semantics.
- Internal Reference, Production, and Compare execution policies. Production
  uses a flat candidate source during M0; M1 may replace only that candidate
  source with acceleration.
- Value-only scene diagnostics for LineTraceSingle, SweepSingle, and
  OverlapMulti, including query validation, candidate/filter/narrow-phase work,
  results, bounded geometry iterations, and compare mismatches.
- Optional low-level AetherCore reference-geometry counters with no allocation
  and negligible work when no counter sink is supplied.
- Deterministic reference-versus-production comparison of query status, hits,
  ordering, and every public result field within frozen tolerances.
- Empty, small, 1,000-body, and 10,000-body sparse/dense fixtures; filter and
  ignored-body cases; add/update/remove churn; randomized/adversarial cases;
  and one real Sandbox capsule-movement sequence.
- Controlled baseline methodology, structural-work evidence, warm timing
  evidence, disabled-diagnostics overhead, and lasting Runtime Physics docs.
- Focused and cross-target validation required by the affected foundational
  runtime modules and Sandbox consumer.

## Non-Goals

- Adding a BVH, dynamic AABB tree, spatial hash, grid, sweep-and-prune, static
  versus moving partition, world AABB, or broad-phase proxy.
- Changing `FPhysicsScene` body storage, handle allocation, generation reuse,
  lookup complexity, or mutation complexity.
- Optimizing or replacing Ray/Box, Capsule/Box distance, overlap, penetration,
  or sweep algorithms.
- Adding new shape-pair support, compounds, convex hulls, triangle meshes,
  heightfields, cooked collision, physical materials, or per-face results.
- Adding rigid-body state, forces, contacts, constraints, fixed-step
  simulation, overlap events, worker threads, batch queries, or backend
  providers.
- Changing `DWorld` trace/sweep/overlap signatures, collision profiles,
  BodySetup/BodyInstance ownership, component registration, Sandbox movement
  policy, or public hit semantics.
- Exposing a public `IPhysicsBackend`, broad-phase interface, diagnostic UI,
  persistent profiling asset, console log stream, or editor overlay.
- Making wall-clock timing alone a correctness or performance gate, or setting
  M1 acceleration thresholds before M0 records controlled baselines.

## Design Decisions and Invariants

### Ownership and module boundaries

- The dependency chain remains `Core -> AetherCore -> Aether -> Engine`.
  AetherCore may own geometry-operation counter values because it owns the
  reference geometry math. Aether owns query policy, pipeline orchestration,
  scene diagnostics, parity comparison, and the flat body candidate source.
  Engine and Sandbox only consume value snapshots for integration tests and
  do not participate in query execution.
- `FPhysicsScene` remains the sole public scene facade. `DWorld` continues to
  call the same LineTrace, Sweep, and Overlap entry points and never selects a
  query implementation or interprets diagnostic internals.
- Pipeline, reference executor, candidate source, accumulator, and comparator
  types remain Aether-private. M0 does not publish an abstraction that a later
  second backend would be forced to implement.
- Scene diagnostics contain numeric values, operation kinds, handles, and
  bounded mismatch facts only. They never retain or expose Engine objects,
  components, Actors, BodySetup, or backend-native pointers.

### Reference, Production, and Compare execution

- `Reference` preserves the current end-to-end flat body walk, validation,
  filtering, pair dispatch, result construction, and stable ordering. Refactors
  must first be covered by characterization tests; no geometry or semantic
  cleanup is bundled into extraction.
- `Production` executes the explicit validation -> candidate enumeration ->
  filtering -> narrow phase -> accumulation -> final ordering pipeline. During
  M0 its candidate source enumerates the same current flat body records. The
  logical Accelerated path named in the roadmap is this Production slot; M1
  changes its candidate source without changing the public policy or query
  stages.
- `Compare` captures one immutable query input, executes Reference and
  Production synchronously against the same unmutated scene state, compares
  complete semantic outputs, records a bounded mismatch, and returns the
  Reference result if they differ. It never publishes a partially compared
  Production result.
- The normal runtime default is Production after parity qualification. Compare
  is explicit test/development behavior and is never enabled implicitly for a
  shipping World. Reference remains selectable for tests and bounded diagnosis.
- Execution policy changes occur only on the scene-owning thread and cannot
  mutate bodies, handles, filters, or query results. Invalid/off-thread policy
  changes reject without changing the previous policy.

### Query semantics are frozen before extraction

- Stage 0 records the existing validity rules for finite endpoints/deltas,
  valid shapes/transforms, channel bounds, off-thread calls, ignored handles,
  two-sided responses, Block-only closest-hit behavior, OverlapMulti response
  behavior, and output clearing on invalid or no-hit queries.
- Closest hit remains ordered by normalized `Time`, then stable
  `FPhysicsActorHandle`. OverlapMulti remains ordered by stable body handle.
  Reference and Production cannot use vector or candidate enumeration order as
  a semantic tie-break.
- Compare covers return status and all `FPhysicsQueryHit` fields: handle,
  response, time, distance, location, impact point, impact normal, penetration
  depth, user token, and initial-penetration flag. Multi results compare count,
  order, and every element.
- Stage 0 freezes exact-versus-tolerant comparison per field from current
  reference behavior and existing geometry tolerances. Handles, response,
  token, flags, count, and order are exact. Floating tolerances must be explicit
  per field and cannot be widened merely to make Compare pass.
- M0 does not alter `CollisionGeometry` contact tolerance or the 28-iteration
  reference searches. Those values become reported baseline facts for M2.

### Diagnostics and bounded failure behavior

- One `FPhysicsSceneQueryDiagnostics` value snapshot reports cumulative totals
  by query kind plus a bounded last-query record. Required structural counters
  are: submitted, invalid/off-thread rejected, reference/production/compare
  executions, body visits, candidates, ignored handles, filter rejects,
  narrow-phase pair tests, geometry distance evaluations or iterations, raw
  hits, returned results, fallbacks, compare mismatches, and scratch/capture
  high-water marks where applicable.
- Scene mutation counters record add, update, remove, failed lookup, and bodies
  present so the M0 baseline exposes current body-lifecycle cost. They do not
  pretend to measure M1 proxy/refit/rebuild work.
- Always-on structural counters are owning-thread plain integers; they add no
  atomics, locks, logging, allocation, or scene traversal. Detailed wall-clock
  sampling and bounded mismatch payload capture are opt-in. Capturing or
  resetting diagnostics is O(1) in body count.
- AetherCore geometry functions accept an optional counter sink or equivalent
  zero-allocation instrumentation seam. A null sink preserves ordinary API
  behavior without constructing a statistics object or invoking callbacks.
- The last mismatch record is fixed-size/value-only and includes query kind,
  compared status, winner handles/counts, and a field-difference mask. Full
  scene bodies, unbounded hit arrays, strings, and object paths are forbidden.
- Counter overflow saturates at the integer maximum and records overflow; it
  never wraps to a plausible lower value. Failed diagnostic capture does not
  change the query result.

### Fixture and measurement contract

- Stage 0 selects checked-in deterministic builders rather than persisted test
  assets for synthetic Aether scenes. Randomized tests record fixed seeds and
  generated distribution parameters in failure output.
- Minimum body scales are 0, 32, 1,000, and 10,000. Workloads include an empty
  scene, a sparse miss, a sparse closest hit, a dense crossing, equal-time
  overlaps, large ignored sets, filter rejection, OverlapMulti result growth,
  and add/update/remove churn. Rotated and positive non-uniformly scaled boxes
  remain represented.
- Adversarial inputs include axis-parallel and zero-length traces, query origin
  inside a body, initial capsule penetration, grazing/tangent contacts,
  coincident bodies, invalid handles, non-finite inputs, invalid transforms,
  equal-time hits, and target-order permutations.
- The Sandbox workload uses the real pawn/capsule movement integration fixture
  and reports its query mix and structural work for grounded movement, wall
  slide, ramp/step traversal, jump/landing, and empty-World fall. Aether tests
  do not depend on Sandbox types; Sandbox owns this consumer measurement.
- Timing uses optimized test binaries from one recorded Agent Build Profile,
  a warm-up, multiple samples, median and a tail percentile, stable fixture
  construction outside the timed query loop, and result consumption that
  prevents optimization removal. Stage 0 records machine/profile context and
  sampling counts before treating times as comparable.
- M0 records baselines but does not set an arbitrary absolute millisecond gate.
  It freezes enough evidence for M1 to select candidate-reduction, memory,
  mutation, small-scene regression, and large-scene speedup thresholds.

### Compatibility and handoff

- No package or serialized format changes are expected. Diagnostics and
  execution policy are transient scene state and must not enter DObject
  reflection or asset serialization.
- No editor-visible UI or gameplay behavior change is in scope. Therefore a
  full editor `all` build is not a planned gate. If implementation adds a
  user-visible surface, the plan must be revised before that work and the root
  full-build requirement becomes active.
- Because M0 changes two foundational runtime modules and validates both Engine
  and Sandbox consumers, focused targets are used during development and one
  final native `--target all` run is required after focused gates pass. The
  reason is cross-target shared query infrastructure, not routine default use.

## Current Foundations and Gaps

| Area | Existing foundation | M0 gap |
| --- | --- | --- |
| Scene facade | `FPhysicsScene` owns one World-independent synchronous body scene with stable public queries | Query orchestration, storage walk, filtering, pair dispatch, and accumulation are one implementation loop |
| Reference behavior | Flat deterministic LineTrace, Capsule Sweep, and Capsule Overlap implementations | No named retained oracle after production traversal changes |
| Ordering | Closest hit uses time then handle; overlaps sort by handle | No shared comparator or cross-policy parity assertion |
| Filtering | Two-sided channel responses and ignored handles are implemented | No counters distinguish visited, ignored, filtered, tested, and hit bodies |
| Geometry | AetherCore exposes Ray/Box and Capsule/Box reference functions with bounded iteration | No optional iteration/evaluation counters; expensive geometry work is invisible |
| Diagnostics | Body count/capture and Engine collision debug snapshot exist | Debug capture walks bodies and does not explain query work, comparison, or cost |
| Testing | `PhysicsSceneTests` cover names, geometry, filtering, thread rejection, lifecycle, BodySetup sharing, and debug bounds | No randomized parity, policy comparison, scale fixtures, churn baselines, or reconciled counters |
| Gameplay | `SandboxGameplayTests` cover authored scene movement and bounded sweeps | Real movement query mix and structural work are not recorded |
| Profiling | Core provides optional Tracy CPU-zone macros and native tests support recorded properties | Aether query zones, controlled warm timing, and baseline evidence are absent |

## Implementation Stages

### Stage 0: Freeze query semantics, metrics, and workloads

Dependencies: Completed first-slice collision plan, current Runtime Collision
contract, PhysicsScene tests, Sandbox movement tests, and baseline revision
recorded in Current Status.

- [ ] Characterize current LineTraceSingle, SweepSingle, and OverlapMulti input
  validation, filtering, result clearing, hit fields, penetration, and stable
  ordering without changing implementation.
- [ ] Freeze the exact/tolerant field comparison matrix and add failing
  fixtures for any current semantic ambiguity before pipeline extraction.
- [ ] Freeze query-kind and scene-mutation counter names, reconciliation
  equations, saturation behavior, optional timing behavior, reset/capture cost,
  and bounded mismatch fields.
- [ ] Freeze the Reference, Production, and Compare policy lifecycle,
  owning-thread mutation rule, normal runtime default, and reference-on-mismatch
  behavior.
- [ ] Define deterministic builders and recorded seeds for 0, 32, 1,000, and
  10,000-body sparse/dense/filter/churn/adversarial workloads.
- [ ] Freeze the Sandbox movement measurement cases and the controlled timing
  method, build profile, warm-up, samples, result consumption, and reported
  statistics.
- [ ] Record pre-refactor focused correctness and timing baselines from the
  source revision in Current Status before Stage 1 changes query structure.

#### Acceptance Gate

- Existing semantics, comparison tolerances, policy behavior, counters,
  reconciliation, fixture distributions, Sandbox cases, and measurement method
  are unambiguous; characterization tests pass; and reproducible pre-refactor
  evidence is recorded without claiming an acceleration target.

### Stage 1: Separate the reference and production query pipelines

Dependencies: Stage 0 semantic matrix, policy contract, and characterization
fixtures.

- [ ] Preserve the current flat implementation as a named private Reference
  executor with no acceleration, behavior cleanup, or geometry change.
- [ ] Introduce the private Production pipeline stages for validation, flat
  candidate enumeration, filtering, narrow-phase dispatch, accumulation, and
  deterministic final ordering.
- [ ] Centralize stable closest-hit and multi-result comparison semantics so
  neither executor depends on traversal order while retaining independent
  candidate discovery.
- [ ] Add owning-thread Reference, Production, and Compare policy selection;
  keep Production as the normal default and reject invalid/off-thread changes
  without mutation.
- [ ] Implement Compare against one immutable scene/query input, bounded
  complete-output comparison, mismatch recording, and Reference-result return
  on mismatch.
- [ ] Prove existing DWorld, BodyInstance, component, and Sandbox callers remain
  unchanged and cannot observe the selected internal execution policy.

#### Acceptance Gate

- All existing focused collision/gameplay fixtures pass in Reference and
  Production; Compare reports zero mismatches; deliberate test-only candidate
  omission, reordering, and result corruption are detected; and the production
  candidate source can be replaced by M1 without editing World APIs,
  component publication, filtering semantics, or narrow-phase functions.

### Stage 2: Add reconciled scene and geometry diagnostics

Dependencies: Stage 1 query stages and Compare lifecycle.

- [ ] Add cumulative per-kind and bounded last-query diagnostics with the Stage
  0 counter schema, saturation, overflow, reset, and value snapshot behavior.
- [ ] Instrument validation, body/candidate enumeration, ignore and filter
  rejection, narrow-phase dispatch, raw hits, result accumulation, compare,
  and fallback paths so reconciliation equations hold for every return path.
- [ ] Add optional zero-allocation AetherCore geometry counters for reference
  iteration/evaluation work without changing contact results.
- [ ] Add scene mutation/body-presence counters that expose current flat-store
  behavior without inventing future broad-phase terms.
- [ ] Ensure normal structural instrumentation uses no atomics, locks, logging,
  or allocation; detailed timing/mismatch capture remains explicit and bounded.
- [ ] Add optional profiling zones only at stable query/pair boundaries and
  verify builds with profiling disabled retain valid behavior.
- [ ] Prove capture/reset is O(1) in body count and disabled detailed
  diagnostics do not walk bodies or allocate.

#### Acceptance Gate

- Counter equations reconcile for valid, invalid, hit, miss, ignored, filtered,
  penetration, overlap, off-thread, compare, and injected-mismatch queries;
  geometry iterations become visible; capture/reset cost is body-count
  independent; and measured instrumentation overhead satisfies the Stage 0
  recorded bound or the bound is revised with evidence before proceeding.

### Stage 3: Qualify parity and record representative baselines

Dependencies: Stage 2 complete diagnostics and Stage 0 workload definitions.

- [ ] Run deterministic randomized and adversarial Reference-versus-Production
  parity across query kinds, body insertion permutations, transforms, filters,
  ignored sets, equal-time ties, penetration, invalid inputs, and scene churn.
- [ ] Record structural counters and warm timing for 0, 32, 1,000, and
  10,000-body sparse miss/hit, dense, filter, ignored, overlap, and mutation
  workloads.
- [ ] Record current Capsule/Box distance/sweep iterations separately from body
  visits so the roadmap can distinguish M1 broad-phase cost from M2 narrow-phase
  cost.
- [ ] Exercise the real Sandbox movement cases and record per-case query mix,
  body visits, filter rejects, pair tests, iterations, results, and timing with
  unchanged gameplay outcomes.
- [ ] Record retained diagnostic memory, last-mismatch capacity, scratch/capture
  high-water marks, and detailed-diagnostics disabled/enabled overhead.
- [ ] Derive evidence-backed proposed M1 entry budgets for candidate reduction,
  memory per body, moving update behavior, small-scene regression, and
  large-scene improvement without selecting the M1 data structure in this plan.

#### Acceptance Gate

- Reference and Production have zero unexplained semantic mismatch; all
  structural counters reconcile; representative body/query/mutation scales and
  Sandbox movement have reproducible recorded evidence; and M1 can select an
  acceleration structure using separate traversal and pair-cost facts rather
  than anecdotal frame time.

### Stage 4: Publish the M0 contract and complete handoff

Dependencies: Stages 0-3 and complete recorded qualification evidence.

- [ ] Move lasting query policy, diagnostics, comparison, measurement, and
  current performance-limit contracts into Runtime Physics documentation.
- [ ] Update the Aether Physics Evolution Roadmap Current Status and M0 row with
  completion evidence, link this plan, and leave M1 unselected until its entry
  budgets are accepted.
- [ ] Run focused `PhysicsSceneTests` throughout implementation and focused
  `SandboxGameplayTests` when the consumer measurement is added, following the
  root native-test guidance.
- [ ] Run final native `--target all` because the completed work changes shared
  AetherCore/Aether query infrastructure and crosses Engine and Sandbox test
  targets; diagnose any aggregate failure with focused target/case reruns.
- [ ] Run changed and all-plan documentation validation, record evidence in
  Current Status, close only passed checklists, and set this plan Completed only
  after every acceptance gate passes.
- [ ] Confirm no user-visible editor surface was introduced. If that scope
  changed, revise the plan and complete the root-required full `all` build
  before handoff.

#### Acceptance Gate

- Focused and full native tests, documentation validation, parity corpus,
  baseline capture, bounded diagnostics, and roadmap handoff all pass from one
  coherent build profile; long-lived behavior is documented outside the plan;
  and M1 receives explicit evidence without M0 silently implementing it.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Public compatibility | Existing `FPhysicsScene` and `DWorld` query signatures, FPhysicsQueryHit fields, collision filtering, BodyInstance publication, and Sandbox behavior remain unchanged |
| Execution policy | Owning-thread Reference/Production/Compare selection, normal Production default, invalid/off-thread rejection, immutable compare input, injected divergence detection, and Reference return on mismatch |
| Query semantics | Validation, output clearing, hit/no-hit, penetration, response, distance/time/location/normal, user token, equal-time winner, and Overlap ordering match the frozen matrix |
| Counter reconciliation | Submitted and rejected queries, body visits, candidates, ignores, filter rejects, pair tests, iterations/evaluations, raw hits, results, fallbacks, mismatches, overflow, and mutation/body totals reconcile on every path |
| Diagnostic cost | Structural counters allocate/log/lock never; detailed timing and mismatch capture are opt-in; capture/reset are O(1) in body count; memory and overhead are recorded and bounded |
| Reference parity | Fixed-seed randomized and adversarial scenes, insertion permutations, transform/filter/ignored variations, invalid data, equal ties, and churn produce zero unexplained mismatch |
| Scale evidence | 0, 32, 1,000, and 10,000-body sparse/dense/filter/ignored/overlap/mutation workloads record structural work and controlled warm timing |
| Gameplay evidence | Ground, wall, ramp/step, jump/landing, and empty-World Sandbox cases retain behavior and publish a reconciled real query mix |
| Module boundary | AetherCore owns optional geometry counters; Aether owns pipeline/policy/scene diagnostics; Engine and Sandbox consume values only; no reverse dependency or DObject leakage |
| Failure and lifetime | Invalid/off-thread input, scene teardown, World replacement, policy reset, counter saturation, compare mismatch, and diagnostic failure cannot mutate bodies or return a partial production result |
| Final qualification | Focused PhysicsScene/Sandbox tests, justified full native suite, documentation validation, plan/roadmap status, and clean commit provenance pass under root guidance |

## Definition of Done

- The original flat scene query behavior remains available as a named private
  Reference oracle and is covered by characterization tests.
- The normal Production path has explicit query stages and a flat candidate
  source that M1 can replace without changing World/component APIs or
  narrow-phase semantics.
- Compare mode executes both paths from one immutable input, detects deliberate
  divergence, records one bounded value-only mismatch, returns Reference on
  mismatch, and reports zero unexplained mismatch in qualification.
- Query and mutation counters reconcile across every supported and rejected
  path; Capsule/Box geometry iteration cost is separately visible.
- Structural instrumentation is allocation-, lock-, atomic-, and log-free;
  detailed diagnostics are opt-in; capture/reset work is independent of scene
  body count; retained memory and overhead are recorded.
- Synthetic scale, randomized/adversarial, mutation, and real Sandbox movement
  workloads have reproducible structural and timing baselines from a recorded
  build profile.
- Runtime Physics docs own the lasting M0 contract, the roadmap records M0
  completion and M1 entry evidence, required validation passes, and this plan
  is marked Completed only after all gates close.

## Deferred Follow-ups

- M1 `AetherSceneQueryAcceleration`: generation-checked dense body storage,
  explicit motion type, conservative world AABBs, static/moving broad phases,
  bounded scratch, and accelerated candidate traversal.
- M2 `AetherGeometryAndNarrowphase`: immutable shared geometry, compound
  primitives, shape-pair dispatch, common analytic fast paths, generic convex
  fallback, and replacement of pathological Capsule/Box production searches.
- M3 cooked convex and triangle-mesh geometry, asset-level BVHs, BodySetup
  derived data, authoring, serialization, and inspection.
- Rigid-body simulation, Engine dynamics/events, batch or parallel execution,
  alternate backends, and gameplay-specific character/moving-platform work
  remain conditional roadmap milestones.
- Persistent benchmark dashboards, CI hardware thresholds, editor profiler UI,
  and telemetry export require separate consumers and infrastructure decisions.

## Related Documentation

- [Aether Physics Evolution Roadmap](../Roadmaps/AetherPhysicsEvolution.md)
- [Runtime Collision](../Runtime/Physics/Collision.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Sandbox Gameplay](../Runtime/Gameplay/SandboxGameplay.md)
- [CPU Profiling](../Development/Build/Profiling.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Physics Scene And Character Collision Plan](PhysicsSceneAndCharacterCollision.md)

## Related Code

- [`Engine/Source/Runtime/AetherCore/Public/Physics/PhysicsTypes.h`](../../Engine/Source/Runtime/AetherCore/Public/Physics/PhysicsTypes.h)
- [`Engine/Source/Runtime/AetherCore/Public/Collision/CollisionGeometry.h`](../../Engine/Source/Runtime/AetherCore/Public/Collision/CollisionGeometry.h)
- [`Engine/Source/Runtime/AetherCore/Private/Collision/CollisionGeometry.cpp`](../../Engine/Source/Runtime/AetherCore/Private/Collision/CollisionGeometry.cpp)
- [`Engine/Source/Runtime/Aether/Public/Physics/PhysicsScene.h`](../../Engine/Source/Runtime/Aether/Public/Physics/PhysicsScene.h)
- [`Engine/Source/Runtime/Aether/Private/Physics/PhysicsScene.cpp`](../../Engine/Source/Runtime/Aether/Private/Physics/PhysicsScene.cpp)
- [`Engine/Source/Runtime/Engine/Public/Engine/World.h`](../../Engine/Source/Runtime/Engine/Public/Engine/World.h)
- [`Engine/Source/Runtime/Engine/Private/Engine/World.cpp`](../../Engine/Source/Runtime/Engine/Private/Engine/World.cpp)
- [`Engine/Tests/Native/EngineTests/Private/Physics/PhysicsSceneTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Physics/PhysicsSceneTests.cpp)
- [`Sandbox/Source/Runtime/Sandbox/Private/SimpleGroundMovementComponent.cpp`](../../Sandbox/Source/Runtime/Sandbox/Private/SimpleGroundMovementComponent.cpp)
- [`Sandbox/Tests/Native/Private/SandboxGameplayTests.cpp`](../../Sandbox/Tests/Native/Private/SandboxGameplayTests.cpp)
