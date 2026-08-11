# Runtime Collision

Summary: Define Aether module ownership, World query behavior, simple body geometry, filtering, component synchronization, and collision debugging.

Modules: AetherCore, Aether, Engine

## Ownership and dependencies

Runtime collision follows the one-way dependency chain `Core -> AetherCore ->
Aether -> Engine`. `AetherCore` owns Engine-independent shapes, handles,
filters, hits, validation, and reference Box/Capsule geometry math. `Aether`
owns the deterministic query-only `FPhysicsScene`; it stores numeric handles
and opaque user tokens and never includes Engine objects. Engine owns body
setups, body instances, profiles, gameplay results, component synchronization,
and the `DWorld` query facade.

Each `DWorld` constructs one game-thread-owned `FPhysicsScene`. Components
publish bodies only into their owning Level's current World and remove them
before unregistering. Level replacement, component destruction, and World
teardown therefore cannot leave a body visible in another World. Collision
queries remain available while play is paused or physics simulation is
disabled; that flag gates future simulation stepping, not query-only geometry.

## Shapes and transforms

The first slice supports Box, Sphere, and Z-axis Capsule values. Box dimensions
are positive local half extents. Capsule half height includes both hemispherical
ends and must be at least its radius. Durin uses `+Z` as up. Physics transforms
must be finite, use a normalizable rotation, and have strictly positive scale;
mirrored, singular, or non-finite transforms are rejected without mutating the
scene.

Ray/Box and Capsule/Box reference paths support arbitrarily rotated Boxes and
positive non-uniform Box scale. Capsule sweeps report normalized time,
distance, location, impact point and normal, and bounded initial penetration.
Equal-time results use the monotonically assigned scene handle as their stable
tie-break.

## Assets and components

`DBodySetup` owns reusable simple geometry, a revision, and local shape offset.
`DStaticMesh` retains its setup independently from render data. The qualified
`/Engine/Models/Box` asset derives one Box setup from its verified LOD 0 bounds;
arbitrary imported meshes do not silently use render bounds or triangles.

Every `DPrimitiveComponent` owns one reflected `FBodyInstance`. It contains the
collision enable state, object channel, profile name, responses, and one
transient physics handle. Register, unregister, transform, mesh, shape, and
profile changes synchronize that handle. A no-collision component or a
component without valid body geometry has no scene entry and can recover after
a later valid edit.

`AStaticMeshActor` selects `WorldStatic`; an independently created
`DStaticMeshComponent` remains `NoCollision`. `DBoxComponent`,
`DSphereComponent`, and `DCapsuleComponent` publish analytic geometry without
render ownership.

## Profiles, queries, and results

Built-in profiles are `NoCollision`, `BlockAll`, `WorldStatic`, `Pawn`, and
`Trigger`. Responses are resolved from both the querying channel and body
object channel. Ignore removes a candidate, Overlap participates only in
overlap queries, and Block participates in closest-hit queries.

Gameplay calls `DWorld::LineTraceSingleByChannel`,
`SweepSingleByChannel`, or `OverlapMultiByChannel`. Inputs and outputs are
copied values. Query parameters can ignore Actors or components; pawn movement
always ignores the moving pawn to avoid self-collision. Invalid or off-thread
queries return no result and do not alter scene state.

## Query execution and observability

`FPhysicsScene` keeps three owning-thread execution policies behind the same
public query methods. `Production` is the normal default and runs explicit
validation, flat candidate enumeration, filtering, narrow phase, accumulation,
and stable final ordering. `Reference` retains the original flat loops as the
semantic oracle. `Compare` copies one immutable query input, runs Reference and
Production synchronously against the unmutated scene, compares the complete
output, and returns the Reference output on any mismatch. Policy changes reject
invalid values and off-thread calls without changing the prior policy. `DWorld`
and component code never select or interpret a policy.

Closest hits remain ordered by exact normalized time followed by stable body
handle. Overlap results remain ordered by body handle. Compare treats status,
count, ordering, handle, response, user token, and initial-penetration state as
exact. It uses absolute tolerance `1.0e-12` for normalized time and `1.0e-8`
for distance, location, impact point, impact normal, and penetration depth.
Non-finite compared values never compare equal.

`CaptureQueryDiagnostics` returns one O(1), value-only
`FPhysicsSceneQueryDiagnostics` snapshot. Each query kind reports submitted,
invalid/off-thread, Reference/Production/Compare execution, body visit,
candidate, ignored, filter-rejected, pair-test, geometry evaluation/iteration,
raw-hit, returned-result, fallback/mismatch, high-water, and optional timing
values. Scene mutation values report add/update/remove calls, successes,
rejections, failed lookups, the reset body baseline, and current body count.
All additions saturate at the unsigned maximum and set `bOverflowed` instead of
wrapping.

The current flat source obeys these reconciliation rules:

```text
ValidSubmissions = SubmittedQueries - InvalidQueries - OffThreadQueries
ReferenceExecutions + ProductionExecutions
    = ValidSubmissions + CompareExecutions
BodyVisits = Candidates
Candidates = IgnoredBodies + FilterRejectedBodies + NarrowPhasePairTests
RawHits <= NarrowPhasePairTests
ReturnedResults <= RawHits
Fallbacks = CompareMismatches

AddCalls = AddSuccesses + AddRejected
UpdateCalls = UpdateSuccesses + UpdateRejected
RemoveCalls = RemoveSuccesses + RemoveRejected
BodiesPresent = BodiesAtReset + AddSuccesses - RemoveSuccesses
```

Structural counters are ordinary integers with no atomics, locks, logging, or
diagnostic allocation. Detailed steady-clock timing and the one fixed-size
last-mismatch payload are explicit opt-in behavior. Diagnostic reset preserves
the current body count and detailed-enabled state without walking bodies.
Off-thread diagnostic capture returns a default snapshot.

AetherCore Capsule/Box reference functions accept an optional
`FCollisionGeometryCounters` pointer. A null pointer preserves ordinary behavior
without allocating or invoking callbacks. A penetrating Capsule/Box overlap
currently performs 59 distance evaluations and 28 search iterations. A sparse
Capsule/Box sweep miss performs 3,422 and 1,652 per tested pair. These values are
reference cost facts, not convergence targets for future production geometry.

## Current query cost and acceleration entry

Production still enumerates every body. On the qualified Release profile, a
10,000-body sparse LineTrace miss visited/tested all 10,000 bodies and took
2.533 ms median; a sparse Capsule sweep miss tested all 10,000 pairs, performed
34.22 million distance evaluations plus 16.52 million search iterations, and
took 217.593 ms median. A dense 10,000-result overlap took 10.110 ms median.
The same scene's last-handle update and remove/add medians were 5.350 us and
5.380 us. One body record is 176 bytes; fixed scene diagnostics retain 840
bytes and one 56-byte mismatch slot.

The accepted M1 proposal requires at most 32 candidates for a 1,000-body sparse
miss and 100 for a 10,000-body sparse miss, while dense overlap still returns
all real results. Proposed incremental index memory is at most 64 bytes per body
plus 64 KiB fixed. A 32-body query may regress at most 10%; 10,000-body sparse
LineTrace and Sweep medians must improve by at least 4x while satisfying the
candidate limits. Update/remove-add P95 proposals at 10,000 bodies are
12.600 us and 10.780 us. These budgets select and qualify a future M1; they do
not imply a chosen spatial data structure.

## Debugging and current limits

Collision debugging is disabled by default. When enabled,
`CaptureCollisionDebugSnapshot` returns at most 4096 current shapes together
with transforms, channels, owners, and the last blocking hit/normal. When
disabled, the capture path returns immediately without walking scene bodies.
The Level Editor viewport's **View mode > Overlays > Collision** toggle consumes
the renderer-independent snapshot to draw Box, Sphere, and Capsule wire shapes
plus the latest blocking impact normal without exposing mutable scene storage.

The implementation is synchronous, query-only, and uses a flat Production
candidate source. Dynamic rigid bodies,
forces, constraints, asynchronous stepping, moving platforms, triangle meshes,
heightfields, overlap events, and project-defined profiles remain future work.
The cross-plan sequencing for scalable queries, geometry, cooked collision,
and evidence-gated simulation or backend work is maintained in the
[Aether Physics Evolution Roadmap](../../Roadmaps/AetherPhysicsEvolution.md).
