# Aether Physics Query Observability Plan

Summary: Establish an instrumented reference/production query pipeline, deterministic parity checking, and representative Aether collision baselines before scene acceleration changes.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

Selected as M0 of the
[Aether Physics Evolution Roadmap](../Roadmaps/AetherPhysicsEvolution.md).
All stages completed on 2026-08-11. The baseline source revision is
`cca78dbc30e0cc70a6d64e7a9d12d990c725fa2a`, where the completed first-slice
physics scene stores bodies in one flat vector and executes deterministic
LineTrace, Capsule Sweep, and Capsule Overlap queries by walking every body.

Pre-refactor characterization added six semantic/fixture tests without changing
the query implementation. `PhysicsSceneTests` passed in both
`Win64-Debug-DurinEditor` and `Win64-Release-DurinEditor`, and the unchanged
Sandbox consumer passed `SandboxGameplayTests`. The disabled Release baseline
entry recorded the flat-query timings below.

Stage 1 retained the original flat loops as Reference executors, made
Production the default explicit validation/candidate/filter/pair/accumulation
pipeline, and added synchronous Compare with complete-output comparison and
Reference fallback. Candidate and result fault injection is private to the
test friend. Focused `PhysicsSceneTests` and `SandboxGameplayTests`
qualification passed: candidate reversal preserved parity, while
candidate omission, result reversal, and field corruption were detected and
returned the Reference result.

Stage 2 added saturating per-kind query work, O(1) scene mutation/capture/reset
values, optional detailed timing/mismatch capture, zero-allocation AetherCore
distance/search counters, and disabled-safe query/pair profiling zones.
`PhysicsSceneTests` passed in Debug and Release and `SandboxGameplayTests`
passed. Exact counter tests cover invalid,
off-thread, hit, ignored, filtered, penetration, Compare, mismatch, saturation,
and mutation paths. The controlled overhead evidence below accepts the revised
20% non-empty structural-instrumentation bound.

Stage 3 passed fixed-seed randomized/adversarial Compare qualification over 16
churned 32-body scenes and fixed-scale LineTrace, Sweep, and Overlap Compare at
0, 32, 1,000, and 10,000 bodies with zero mismatch. `PhysicsSceneTests` passed
in Debug and Release and `SandboxGameplayTests` passed. The explicit Release
qualification entries recorded the structural, timing, mutation,
retained-memory, and real Sandbox evidence below.

Stage 4 published the lasting execution-policy, comparison, diagnostics,
measurement, current-limit, and M1-entry contracts in Runtime Physics and
updated the Aether roadmap to mark M0 complete while leaving M1 unselected.
Focused PhysicsScene/Sandbox targets, the required Debug native `--target all`
aggregate, changed/all documentation validation, and all-plan/all-roadmap
validation passed under the recorded build profile. No editor-visible surface
or gameplay behavior was introduced, so no full editor build gate applied.

The original implementation remains the semantic oracle. M0 separated it from
the Production pipeline, added bounded counters and comparison diagnostics,
and recorded small, sparse, dense, mutation-heavy, and Sandbox movement
baselines. It did not implement a scene index, replace current geometry
algorithms, or claim production speedup.

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

## Stage 0 Frozen Contract

### Result comparison matrix

Reference/Production comparison uses the following fixed field rules. These
tolerances compare complete semantic output; they do not change closest-hit
selection, whose `Time` comparison remains exact before the handle tie-break.

| Output | Rule |
| --- | --- |
| Query return status, result count, result order | Exact |
| Actor handle, response, user token, initial-penetration flag | Exact |
| Normalized time | Absolute tolerance `1.0e-12` |
| Distance | Absolute tolerance `1.0e-8` |
| Location and impact point, component by component | Absolute tolerance `1.0e-8` |
| Impact normal, component by component | Absolute tolerance `1.0e-8` |
| Penetration depth | Absolute tolerance `1.0e-8` |

The spatial tolerances are no wider than the existing `1.0e-8` contact
tolerance. Non-finite compared values never compare equal. The first bounded
mismatch records query kind, both return statuses, both counts, first differing
result index, reference/production winner handles, and a mask containing
Status, Count, Order, Handle, Response, Time, Distance, Location, ImpactPoint,
ImpactNormal, PenetrationDepth, UserToken, or StartPenetrating. It stores no
strings, body arrays, or Engine values.

### Query policy lifecycle

- `Production` is the default for every new scene. `Reference` and `Compare`
  are explicit owning-thread selections; an invalid enum or off-thread change
  returns false and preserves the prior selection.
- Validation and output clearing occur before executor dispatch. If a call is
  both off-thread and otherwise invalid, it is classified as off-thread because
  owning-thread validation has priority.
- `Compare` reuses one immutable value input and unmutated owning-thread scene,
  runs Reference then Production synchronously, compares the complete output,
  and returns Production only on equality. A mismatch increments mismatch and
  fallback counters and returns the complete Reference output.
- Structural mismatch/fallback counts are always retained. The fixed mismatch
  payload and steady-clock sampling are captured only when detailed diagnostics
  are explicitly enabled. Enabling, disabling, capture, and reset are
  owning-thread operations and cannot alter bodies or query output.

### Counter schema and reconciliation

Each query kind (`LineTraceSingle`, `SweepSingle`, and `OverlapMulti`) owns
saturating cumulative values named `SubmittedQueries`, `InvalidQueries`,
`OffThreadQueries`, `ReferenceExecutions`, `ProductionExecutions`,
`CompareExecutions`, `BodyVisits`, `Candidates`, `IgnoredBodies`,
`FilterRejectedBodies`, `NarrowPhasePairTests`,
`GeometryDistanceEvaluations`, `GeometrySearchIterations`, `RawHits`,
`ReturnedResults`, `Fallbacks`, `CompareMismatches`, `ScratchHighWater`,
`CaptureHighWater`, `DetailedTimingSamples`, and
`DetailedTimingNanoseconds`. A bounded last-query value carries the same work
terms plus kind, policy, validity, and return status.

Scene mutation values are `AddCalls`, `AddSuccesses`, `AddRejected`,
`UpdateCalls`, `UpdateSuccesses`, `UpdateRejected`, `RemoveCalls`,
`RemoveSuccesses`, `RemoveRejected`, `FailedLookups`, `BodiesAtReset`, and
`BodiesPresent`. Reset zeros cumulative values in O(1), sets both body values
to the current vector size, and clears the last query/mismatch without walking
bodies. Capture is one value copy. Every addition that would overflow clamps to
the unsigned integer maximum and sets `bOverflowed`; no counter wraps.

The following equations and inequalities must hold cumulatively and for each
last-query execution where the terms apply:

```text
ValidSubmissions = SubmittedQueries - InvalidQueries - OffThreadQueries
ReferenceExecutions + ProductionExecutions
    = ValidSubmissions + CompareExecutions
Candidates = IgnoredBodies + FilterRejectedBodies + NarrowPhasePairTests
RawHits <= NarrowPhasePairTests
ReturnedResults <= RawHits
Fallbacks = CompareMismatches

AddCalls = AddSuccesses + AddRejected
UpdateCalls = UpdateSuccesses + UpdateRejected
RemoveCalls = RemoveSuccesses + RemoveRejected
BodiesPresent = BodiesAtReset + AddSuccesses - RemoveSuccesses
```

During M0 the flat candidate source also requires `BodyVisits = Candidates`.
M1 may reduce Production candidates but must preserve the remaining equations.
Compare work counts both internal executions while returned results count the
single public result, so `ReturnedResults <= RawHits` is intentionally not an
equality in Compare mode. Geometry evaluation/iteration values count only work
reported by the optional AetherCore sink.

### Fixtures and measurement method

Synthetic fixtures use seed `0xA37E202608110001` and body counts 0, 32, 1,000,
and 10,000. Sparse bodies use a deterministic four-unit grid outside the query
corridor; sparse-hit fixtures replace body zero with one corridor hit; dense
bodies use the recorded LCG to place rotated, positive non-uniformly scaled
boxes inside a 0.4-unit cube. Filter cohorts alternate two-sided Ignore,
Overlap, and Block responses; ignored cohorts select every third stable handle;
churn removes every third body, attempts the same removal again, updates the
next cohort, and adds replacements. Adversarial fixtures cover zero-length and
axis-parallel traces, inside starts, initial capsule penetration, strict tangent
non-overlap, coincident bodies, equal-time ties, invalid handles, invalid
channels/transforms, non-finite values, and insertion permutations. Any added
randomized cohort must print its seed and distribution parameters on failure.

Sandbox measurement cases are frozen to grounded forward movement, wall stop
and slide, rotated ramp plus supported step traversal, jump/ceiling/landing,
raised-platform landing, and empty-World fall. They use the real
`DSimpleGroundMovementComponent` fixture at its existing 60 Hz sequence (plus
the existing 30/60/120 Hz comparison), not an Aether-owned surrogate.

Timing uses the `windows-msvc-x64` Agent Build Profile,
`Win64-Release-DurinEditor`, MSVC 14.44.35207, Ninja, Tracy disabled, fixture
construction outside the measured loop, three warm-ups, eleven samples,
steady-clock nanoseconds, median and P95 (the maximum of eleven), and a consumed
checksum. The recording machine was an Intel Core i5-13400F with 16 logical
processors on Windows 10.0.26200. Timings are evidence, not an absolute gate.

### Pre-refactor flat-query timing evidence

These nanoseconds-per-query values came from the disabled
`FAetherQueryBaselineBenchmarks.RecordsPrePipelineFlatQueryBaseline` entry at
baseline code revision `cca78dbc30e0cc70a6d64e7a9d12d990c725fa2a`.

| Fixture | Bodies | Median ns | P95 ns |
| --- | ---: | ---: | ---: |
| Line sparse miss | 0 | 36 | 44 |
| Line sparse closest hit | 0 | 37 | 41 |
| Sweep sparse miss | 0 | 107 | 112 |
| Sweep dense penetration | 0 | 106 | 201 |
| Overlap dense | 0 | 93 | 111 |
| Line sparse miss | 32 | 7,440 | 7,982 |
| Line sparse closest hit | 32 | 7,492 | 7,894 |
| Sweep sparse miss | 32 | 695,200 | 708,912 |
| Sweep dense penetration | 32 | 30,916 | 38,248 |
| Overlap dense | 32 | 29,535 | 31,238 |
| Line sparse miss | 1,000 | 236,265 | 239,096 |
| Line sparse closest hit | 1,000 | 236,528 | 239,831 |
| Sweep sparse miss | 1,000 | 22,091,500 | 22,785,100 |
| Sweep dense penetration | 1,000 | 1,005,800 | 1,397,800 |
| Overlap dense | 1,000 | 1,028,400 | 1,208,800 |
| Line sparse miss | 10,000 | 2,357,775 | 2,556,085 |
| Line sparse closest hit | 10,000 | 2,344,590 | 2,462,940 |
| Sweep sparse miss | 10,000 | 218,800,400 | 221,459,800 |
| Sweep dense penetration | 10,000 | 10,545,400 | 11,339,100 |
| Overlap dense | 10,000 | 10,127,300 | 10,466,000 |

The near-linear body-count growth and the roughly 20x sparse-sweep versus dense
initial-penetration cost at 10,000 bodies establish separate traversal and
geometry-work questions for later instrumentation; they do not select or set a
target for M1.

### Stage 2 instrumentation qualification

The same Release profile measured the always-on structural path with detailed
diagnostics disabled. The empty-scene change is a fixed 33 ns. At representative
non-empty scales the median increase is 15.9%-17.7%, so the accepted Stage 2
bound is at most 20% over the pre-refactor sparse-miss LineTrace median for
32, 1,000, and 10,000 bodies. This bound records M0 observability cost; it is
not an M1 small-scene or acceleration target.

| Bodies | Stage 0 median ns | Stage 2 detailed-off median ns | Change |
| ---: | ---: | ---: | ---: |
| 0 | 36 | 69 | +33 ns |
| 32 | 7,440 | 8,743 | +17.5% |
| 1,000 | 236,265 | 278,142 | +17.7% |
| 10,000 | 2,357,775 | 2,732,815 | +15.9% |

Detailed timing added 42 ns to the empty query (111 ns versus 69 ns) and no
positive median increase at 32, 1,000, or 10,000 bodies in this eleven-sample
run. The accepted detailed-timing bound is therefore an additive 75 ns for an
empty query and at most 5% for the non-empty cohorts; Stage 3 repeats the
measurement before proposing M1 budgets.

Diagnostic capture measured 24-25 ns P95 and reset measured 37-46 ns P95 at
all four body counts. Their medians were exactly 24 ns and 36 ns respectively
for 0, 32, 1,000, and 10,000 bodies, demonstrating body-count-independent
observation in the controlled harness.

### Stage 3 qualification evidence

The randomized parity base seed was `0xA37E504154590001`; all 16 derived seeds
are printed with their scenario on failure. The corpus permuted insertion and
candidate order, varied positive non-uniform scale, rotation, channels,
two-sided responses and ignored handles, exercised zero-length/axis-parallel
traces, capsule sweeps/overlaps, invalid/non-finite inputs, then removed,
updated, and re-added bodies. The separate scale run compared all three query
kinds at 0, 32, 1,000, and 10,000 bodies. Both corpora recorded zero mismatch.

Retained values measured 888 bytes for `FPhysicsScene`, including one 840-byte
diagnostic snapshot; one flat body record is 176 bytes. The fixed last-mismatch
payload is 56 bytes with capacity one. Production query scratch high water is
zero for single queries and equals the returned result count for OverlapMulti;
the dense 10,000-body overlap therefore recorded 10,000 result elements without
an additional hidden body capture.

Structural work remained exactly linear in the flat source. The representative
10,000-body facts are:

| Fixture | Visits/candidates | Ignored | Filter rejects | Pair tests | Distance evaluations | Search iterations | Raw/returned |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Line sparse miss | 10,000 | 0 | 0 | 10,000 | 0 | 0 | 0 / 0 |
| Line sparse closest hit | 10,000 | 0 | 0 | 10,000 | 0 | 0 | 1 / 1 |
| Line dense crossing | 10,000 | 0 | 0 | 10,000 | 0 | 0 | 10,000 / 1 |
| Line filter mix | 10,000 | 0 | 6,667 | 3,333 | 0 | 0 | 0 / 0 |
| Line ignored thirds | 10,000 | 3,334 | 0 | 6,666 | 0 | 0 | 0 / 0 |
| Sweep sparse miss | 10,000 | 0 | 0 | 10,000 | 34,220,000 | 16,520,000 | 0 / 0 |
| Overlap dense | 10,000 | 0 | 0 | 10,000 | 590,000 | 280,000 | 10,000 / 10,000 |

One sparse-miss capsule pair costs 3,422 distance evaluations and 1,652 search
iterations, while one penetrating overlap pair costs 59 and 28. Those constants
also reconciled at 32 and 1,000 bodies. The distinction is the evidence for
separating M1 candidate reduction from later M2 geometry replacement.

The following controlled Release times are median / P95 microseconds per
operation. Mutation measurements target the last flat-vector handle; churn
removes that handle and adds one replacement while keeping body count stable.

| Bodies | Line miss | Line hit | Line dense | Filter mix | Ignored thirds | Sweep miss | Dense overlap | Update last | Remove/add last |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0.069 / 0.070 | 0.069 / 0.077 | 0.065 / 0.068 | 0.069 / 0.070 | 0.064 / 0.066 | 0.132 / 0.137 | 0.118 / 0.118 | n/a | n/a |
| 32 | 7.836 / 8.080 | 7.900 / 7.980 | 11.290 / 11.981 | 2.800 / 2.872 | 5.826 / 6.069 | 700.167 / 1,006.548 | 29.896 / 33.022 | 0.130 / 0.133 | 0.146 / 0.187 |
| 1,000 | 248.768 / 266.597 | 247.595 / 262.318 | 351.879 / 359.527 | 86.642 / 89.575 | 316.039 / 326.604 | 21,573.200 / 22,788.000 | 974.600 / 1,056.800 | 0.641 / 0.642 | 0.657 / 0.834 |
| 10,000 | 2,532.995 / 2,730.765 | 2,426.535 / 2,562.180 | 3,605.845 / 3,922.575 | 896.160 / 1,001.645 | 14,357.305 / 15,881.275 | 217,592.600 / 222,754.800 | 10,109.600 / 11,487.200 | 5.350 / 6.300 | 5.380 / 5.390 |

The 10,000-body churn cohort removed 3,334 bodies, updated 3,333, and added
3,334 replacements, ending at 10,000 with zero failed lookup. The 32- and
1,000-body cohorts used the same every-third distribution and also reconciled.

The real Sandbox fixture produced only SweepSingle calls in these movement
sequences. Times are median / P95 milliseconds for the complete 60 Hz tick
sequence; setup, teardown, and garbage collection are outside the timed region.

| Case | Sweep queries | Body visits | Pair tests | Distance evaluations | Search iterations | Returned | Sequence ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Grounded forward | 360 | 720 | 720 | 1,644,271 | 797,160 | 240 | 10.958 / 11.967 |
| Wall stop | 626 | 1,878 | 1,878 | 5,637,450 | 2,733,248 | 507 | 36.982 / 38.467 |
| Rotated ramp | 342 | 1,026 | 1,026 | 2,712,053 | 1,312,360 | 221 | 18.038 / 18.374 |
| Supported step | 280 | 840 | 840 | 2,212,323 | 1,070,440 | 171 | 14.684 / 15.690 |
| Jump, ceiling, landing | 250 | 750 | 750 | 2,065,177 | 999,880 | 207 | 13.845 / 15.133 |
| Raised-platform landing | 270 | 540 | 540 | 1,228,557 | 595,616 | 181 | 8.297 / 8.506 |
| Empty-World fall | 90 | 90 | 90 | 0 | 0 | 0 | 0.120 / 0.133 |

Gameplay outcomes retained the existing grounded, wall, ramp, step, ceiling,
platform, and fall assertions. The empty-World pairs perform no geometry work,
which exposes dispatch/filter overhead separately from Box narrow phase.

### Proposed M1 entry budgets

These are evidence-backed proposal gates for selecting and qualifying M1, not
a data-structure choice:

- A 1,000-body sparse miss should emit at most 32 Production candidates, and a
  10,000-body sparse miss at most 100. Dense overlap must still return all
  10,000 actual results; candidate reduction may not be an arbitrary cap.
- Incremental broad-phase retained memory should fit within 64 bytes per body
  plus 64 KiB fixed scene state, in addition to the current 176-byte body
  record and fixed 840-byte diagnostics.
- At 32 bodies, median query time should regress no more than 10% from the M0
  Production medians above. The 32-body update P95 proposal is 0.160 us (20%
  over 0.133 us).
- At 10,000 bodies, moving-update P95 should be at most 12.600 us and
  remove/add P95 at most 10.780 us (2x the M0 values) while preserving the
  stable body count and handle semantics.
- At 10,000 bodies, sparse LineTrace and Sweep should improve by at least 4x:
  median ceilings of 0.633 ms and 54.398 ms respectively, alongside the
  candidate budgets. Timing alone cannot pass if structural candidates do not
  fall.
- M1 must preserve zero Compare mismatch, all counter equations, O(1)
  diagnostic capture/reset, fixed mismatch capacity, and the Sandbox outcomes.

## Implementation Stages

### Stage 0: Freeze query semantics, metrics, and workloads

Dependencies: Completed first-slice collision plan, current Runtime Collision
contract, PhysicsScene tests, Sandbox movement tests, and baseline revision
recorded in Current Status.

- [x] Characterize current LineTraceSingle, SweepSingle, and OverlapMulti input
  validation, filtering, result clearing, hit fields, penetration, and stable
  ordering without changing implementation.
- [x] Freeze the exact/tolerant field comparison matrix and add failing
  fixtures for any current semantic ambiguity before pipeline extraction.
- [x] Freeze query-kind and scene-mutation counter names, reconciliation
  equations, saturation behavior, optional timing behavior, reset/capture cost,
  and bounded mismatch fields.
- [x] Freeze the Reference, Production, and Compare policy lifecycle,
  owning-thread mutation rule, normal runtime default, and reference-on-mismatch
  behavior.
- [x] Define deterministic builders and recorded seeds for 0, 32, 1,000, and
  10,000-body sparse/dense/filter/churn/adversarial workloads.
- [x] Freeze the Sandbox movement measurement cases and the controlled timing
  method, build profile, warm-up, samples, result consumption, and reported
  statistics.
- [x] Record pre-refactor focused correctness and timing baselines from the
  source revision in Current Status before Stage 1 changes query structure.

#### Acceptance Gate

- Existing semantics, comparison tolerances, policy behavior, counters,
  reconciliation, fixture distributions, Sandbox cases, and measurement method
  are unambiguous; characterization tests pass; and reproducible pre-refactor
  evidence is recorded without claiming an acceleration target.

### Stage 1: Separate the reference and production query pipelines

Dependencies: Stage 0 semantic matrix, policy contract, and characterization
fixtures.

- [x] Preserve the current flat implementation as a named private Reference
  executor with no acceleration, behavior cleanup, or geometry change.
- [x] Introduce the private Production pipeline stages for validation, flat
  candidate enumeration, filtering, narrow-phase dispatch, accumulation, and
  deterministic final ordering.
- [x] Centralize stable closest-hit and multi-result comparison semantics so
  neither executor depends on traversal order while retaining independent
  candidate discovery.
- [x] Add owning-thread Reference, Production, and Compare policy selection;
  keep Production as the normal default and reject invalid/off-thread changes
  without mutation.
- [x] Implement Compare against one immutable scene/query input, bounded
  complete-output comparison, mismatch recording, and Reference-result return
  on mismatch.
- [x] Prove existing DWorld, BodyInstance, component, and Sandbox callers remain
  unchanged and cannot observe the selected internal execution policy.

#### Acceptance Gate

- All existing focused collision/gameplay fixtures pass in Reference and
  Production; Compare reports zero mismatches; deliberate test-only candidate
  omission, reordering, and result corruption are detected; and the production
  candidate source can be replaced by M1 without editing World APIs,
  component publication, filtering semantics, or narrow-phase functions.

### Stage 2: Add reconciled scene and geometry diagnostics

Dependencies: Stage 1 query stages and Compare lifecycle.

- [x] Add cumulative per-kind and bounded last-query diagnostics with the Stage
  0 counter schema, saturation, overflow, reset, and value snapshot behavior.
- [x] Instrument validation, body/candidate enumeration, ignore and filter
  rejection, narrow-phase dispatch, raw hits, result accumulation, compare,
  and fallback paths so reconciliation equations hold for every return path.
- [x] Add optional zero-allocation AetherCore geometry counters for reference
  iteration/evaluation work without changing contact results.
- [x] Add scene mutation/body-presence counters that expose current flat-store
  behavior without inventing future broad-phase terms.
- [x] Ensure normal structural instrumentation uses no atomics, locks, logging,
  or allocation; detailed timing/mismatch capture remains explicit and bounded.
- [x] Add optional profiling zones only at stable query/pair boundaries and
  verify builds with profiling disabled retain valid behavior.
- [x] Prove capture/reset is O(1) in body count and disabled detailed
  diagnostics do not walk bodies or allocate.

#### Acceptance Gate

- Counter equations reconcile for valid, invalid, hit, miss, ignored, filtered,
  penetration, overlap, off-thread, compare, and injected-mismatch queries;
  geometry iterations become visible; capture/reset cost is body-count
  independent; and measured instrumentation overhead satisfies the Stage 0
  recorded bound or the bound is revised with evidence before proceeding.

### Stage 3: Qualify parity and record representative baselines

Dependencies: Stage 2 complete diagnostics and Stage 0 workload definitions.

- [x] Run deterministic randomized and adversarial Reference-versus-Production
  parity across query kinds, body insertion permutations, transforms, filters,
  ignored sets, equal-time ties, penetration, invalid inputs, and scene churn.
- [x] Record structural counters and warm timing for 0, 32, 1,000, and
  10,000-body sparse miss/hit, dense, filter, ignored, overlap, and mutation
  workloads.
- [x] Record current Capsule/Box distance/sweep iterations separately from body
  visits so the roadmap can distinguish M1 broad-phase cost from M2 narrow-phase
  cost.
- [x] Exercise the real Sandbox movement cases and record per-case query mix,
  body visits, filter rejects, pair tests, iterations, results, and timing with
  unchanged gameplay outcomes.
- [x] Record retained diagnostic memory, last-mismatch capacity, scratch/capture
  high-water marks, and detailed-diagnostics disabled/enabled overhead.
- [x] Derive evidence-backed proposed M1 entry budgets for candidate reduction,
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

- [x] Move lasting query policy, diagnostics, comparison, measurement, and
  current performance-limit contracts into Runtime Physics documentation.
- [x] Update the Aether Physics Evolution Roadmap Current Status and M0 row with
  completion evidence, link this plan, and leave M1 unselected until its entry
  budgets are accepted.
- [x] Run focused `PhysicsSceneTests` throughout implementation and focused
  `SandboxGameplayTests` when the consumer measurement is added, following the
  root native-test guidance.
- [x] Run final native `--target all` because the completed work changes shared
  AetherCore/Aether query infrastructure and crosses Engine and Sandbox test
  targets; diagnose any aggregate failure with focused target/case reruns.
- [x] Run changed and all-plan documentation validation, record evidence in
  Current Status, close only passed checklists, and set this plan Completed only
  after every acceptance gate passes.
- [x] Confirm no user-visible editor surface was introduced. If that scope
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
- [Physics Scene And Character Collision Plan](Archive/2026-08/PhysicsSceneAndCharacterCollision.md)

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
