# Runtime Collision

Summary: Define Aether module ownership, World query behavior, immutable primitive and cooked geometry, filtering, component synchronization, and collision debugging.

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

## Geometry resources, shapes, and transforms

`FCollisionShape` remains the compact query-side Box, Sphere, or Z-axis Capsule
value. Bodies instead retain a copyable `FCollisionGeometryRef` to one
validated immutable AetherCore payload. A primitive payload contains one
shape. A compound contains 1 through 64 simple children in stable input order,
each with one valid local transform; compounds do not nest. Every payload has
a non-zero process-local identity, immutable local bounds, and retained-byte
facts. Scene bodies copy the two-word reference and never copy child data.

Box dimensions
are positive local half extents. Capsule half height includes both hemispherical
ends and must be at least its radius. Durin uses `+Z` as up. Physics transforms
must be finite, use a normalizable rotation, and have strictly positive scale;
mirrored, singular, or non-finite transforms are rejected without mutating the
scene.

Child world transforms are `FTransform::Combine(Body, Child)`. Bounds and
narrow phase both use Box component scale, Sphere maximum XYZ scale, and
Capsule maximum XY radius scale with Z half-height scale clamped to the scaled
radius.

Ray, Sweep, and Overlap support every Box/Sphere/Capsule query against primitive,
convex-hull, triangle-mesh, and compound targets. Hulls expose validated planes,
vertices, and stable features. Triangle meshes retain cleaned source-order
triangles plus a deterministic asset BVH with 32-byte nodes, at most eight
triangles per leaf, depth at most 64, and a bounded 128-entry traversal stack.
Mesh collision is double-sided; ties use source triangle ordinal. Production
traversal falls back to the complete Reference path on structural overflow and
records that exceptional path explicitly.

Sweeps report normalized time,
distance, location, impact point and normal, and bounded initial penetration.
Equal-time results use the monotonically assigned scene handle as their stable
tie-break. Compound closest hits first select `(Time, child index)`; Overlap
selects the lowest overlapping child and still emits one result per body.

## Assets and components

`DBodySetup` owns collision source mode, Simple/Complex query policy, build
revision/status, a local shape offset, optional independent immutable-resource
caches, and exact retained payload bytes. Repeated publication for one revision
returns the same identity; successful collision-relevant setters invalidate
geometry. Material, thumbnail, and render-readiness changes do not.

`DStaticMesh` retains its setup and a detached canonical LOD 0 collision snapshot
independently from render data. Collision is opt-in: `None`, `SimpleHull`, or
`ComplexMesh`; imported meshes never silently use render bounds or triangles.
`SimpleOnly`, `ComplexOnly`, and `SimpleAndComplex` select the published resource
without changing component filters. The qualified `/Engine/Models/Box` asset
continues to derive its authored Box setup from verified LOD 0 bounds.

Editor derived data uses the separate `StaticMeshCollision/Objects` namespace
and a key containing collision builder/schema/platform versions, exact source
identity, import-space settings, mode/policy, and canonical bytes. Builds and
reimports publish render data, BodySetup state, collision resources, revisions,
and diagnostics transactionally. A cache miss or corruption is rebuildable only
while detached source inputs exist.

Cook writes independently versioned DMSH render and optional required DCOL
collision companions in one `FCookContext` transaction. Cooked BodySetup state
contains policy and the exact DCOL descriptor but no source snapshot or DDC key.
Runtime validates both descriptors and payloads before publishing either, and
reconstructs collision without source, DDC, importers, editor modules, or
initialized render resources. Missing, corrupt, mismatched, or incompatible
required collision rejects the asset; there is no cooked-runtime fallback.

Every `DPrimitiveComponent` owns one reflected `FBodyInstance`. It contains the
collision enable state, object channel, profile name, responses, and one
transient physics handle. StaticMesh body publication remembers the published
BodySetup revision so policy, geometry, or reimport changes republish the body
without altering its profile/filter state. Register, unregister, transform,
mesh, shape, and profile changes synchronize that handle. A no-collision component or a
component without valid body geometry has no scene entry and can recover after
a later valid edit.

`DSplineMeshComponent` adds an explicit
`ESplineMeshCollisionMode::{Disabled,DeformedTriangleMesh}` policy. Disabled is
the default and builds or retains no collision BVH. DeformedTriangleMesh builds
one immutable triangle resource from the same normalized LOD 0 positions and
indices used by exact editor picking and CPU/shader parity; its input identity
combines the source render-resource revision and deformation revision. Rendering
can remain valid when all deformed triangles are degenerate and collision is
therefore invalid. A successful deformation publishes the new immutable CPU
state before ordinary physics-state replacement, so the old handle is retired
and no stale handle remains queryable. Disable, source removal/reimport,
component retirement, Actor segment removal, PIE teardown, and World teardown
release the body and shared geometry normally.

`ASplineMeshActor` defaults generated segments to Disabled plus `NoCollision`.
Its reflected path collision policy enables DeformedTriangleMesh and QueryOnly
consistently for every segment. On the frozen 256-vertex/254-triangle road strip
in `Win64-Debug-DurinEditor`, 300 post-warm-up synchronous builds measured 2.97
ms p95; one payload multiplied by the 128-segment stress count retains 1,736,704
bytes, below the 8 ms and 32 MiB gates.

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
validation, hybrid broad-phase traversal, filtering, narrow phase,
accumulation, and stable final ordering. `Reference` retains the dense flat
loops as the semantic oracle. `Compare` copies one immutable query input, runs
Reference and Production synchronously against the unmutated scene, compares
the complete output, and returns the Reference output on any mismatch. Policy
changes reject invalid values and off-thread calls without changing the prior
policy. `DWorld` and component code never select or interpret a policy.

AetherCore owns operation/pair selection and compound iteration behind one
facade. Scene traversal validates, filters, and accumulates bodies but contains
no concrete pair algorithm calls. Internal outcomes distinguish Hit, Miss,
Invalid, Unsupported, and NonConverged. Analytic and finite-feature paths cover
the complete primitive matrix. Production Capsule/Box uses exact piecewise
segment/box distance and bounded conservative advancement; the nested search
is retained only as direct reference evidence and explicit recovery.

Production casts use at most 32 advancement iterations. Unsupported or
non-converged work is named and counted, and qualified recovery never becomes
a silent miss. Tangency is a Ray/Sweep contact only when motion enters the
target surface. Overlap requires penetration beyond `1.0e-8`; zero-delta Sweep
returns only initial penetration.

Closest hits remain ordered by exact normalized time followed by stable body
handle. Overlap results remain ordered by body handle. Compare treats status,
count, ordering, handle, response, user token, and initial-penetration state as
exact. It uses absolute tolerance `1.0e-12` for normalized time and `1.0e-8`
for distance, location, impact point, impact normal, and penetration depth.
Non-finite compared values never compare equal.

`CaptureQueryDiagnostics` returns one O(1), value-only
`FPhysicsSceneQueryDiagnostics` snapshot. Each query kind reports submitted,
invalid/off-thread, Reference/Production/Compare execution, body visit,
candidate, ignored, filter-rejected, pair-test, leaf/compound work,
analytic/generic selection, support/distance/iteration work,
unsupported/non-convergence/reference fallback,
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
Fallbacks >= CompareMismatches

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

AetherCore geometry functions accept an optional
`FCollisionGeometryCounters` pointer. A null pointer preserves ordinary behavior
without allocating or invoking callbacks. A penetrating Capsule/Box overlap
performs 59 distance evaluations and 28 search iterations. Its sparse
zero-delta Capsule/Box miss performs 3,422 and 1,652 per tested pair. Production
qualified Capsule/Box casts stay within 96 feature evaluations and 64 total
search/cast iterations and ordinary Sandbox movement never enters that nested
search.

## Indexed storage and query cost

Handles encode a one-based slot plus a non-zero generation. Slots resolve one
dense record directly; removal swap-removes the dense tail and repairs its
owning slot. Retired generations reject before dense access, and reusable slots
advance their generation before returning to the LIFO free list. A record owns
one outward-rounded six-float exact AABB and its slot index. The qualified
record remains at or below the M1 192-byte gate and adds only the two-word
shared reference, plus a 12-byte slot. Geometry diagnostics count unique
payloads and retained bytes, so instance count does not multiply child memory.

Motion is explicit and independent of filters. Low-level descriptions and
component-driven bodies default to `Kinematic`; qualified `AStaticMeshActor`
geometry publishes `Static`; `Dynamic` is accepted by the moving partition but
does not imply simulation. Static bodies use a deterministic median-split
contiguous BVH. Kinematic and Dynamic bodies use a deterministic incremental
AABB tree. Moving fat bounds are derived from the exact bound with 10 percent
extent margin and 0.01 minimum margin, so contained moves avoid reinsertion
without retaining a second bound. A depth threshold deterministically rebuilds
the moving tree before its supported traversal height can be exceeded.

Production traverses both partitions with a fixed 128-entry stack. Leaves are
stable slot references, so dense swaps do not invalidate either tree. Finite
segment, swept-shape, and overlap AABBs cull nodes and exact body bounds;
closest queries additionally prune only when conservative node near time is
strictly greater than the current exact winner. Equal times remain eligible for
the documented complete-handle tie-break. Scratch overflow records an explicit
fallback and executes the complete flat Reference path.

On the qualified `Win64-Release-DurinEditor` profile at revision based on
`07b9bc567b0deaa3b744755047d14f89a4711dce`, 10,000-body sparse LineTrace and
Capsule sweep misses emitted zero candidates and measured 0.120 and 0.300
microseconds median, versus the M0 2.533 and 217.593 milliseconds. A sparse
closest-hit trace emitted one candidate. Dense overlap returned all 10,000
results in handle order. The 32-body filter-only update P95 was 0.017
microseconds; 10,000-body update and stable remove/add P95 were 0.030 and 1.430
microseconds. Actual all-static, all-moving, and mixed retained capacities at
0/32/1,000/10,000 bodies fit `64 * live bodies + 64 KiB`; qualified queries had
zero mismatch, scratch overflow, or spatial fallback.

For M2 on `Win64-Release-DurinEditor`, the controlled sparse zero-delta
Capsule/Box pair measured 11,000 ns reference median and 437 ns Production
median, a 25.17x improvement. Focused Release PhysicsScene and Sandbox suites
reported zero mismatch, unsupported, non-convergence, overflow, or unexpected
pair fallback.

For M3 on `Win64-Release-DurinEditor`, the 100,352-triangle deterministic grid
contains 32,767 BVH nodes at depth 15, retains 4,270,912 bytes, and reports a
23,147,864-byte estimated builder peak. A sparse outside ray performs one asset
node test and zero feature tests instead of the Reference path's 100,352 feature
tests; an interior ray stays below 128 node and 64 feature tests. Ten thousand
bodies share one resource identity and retained-byte charge. The full Release
PhysicsScene matrix passes with zero mismatch, false negative, unsupported,
non-convergence, overflow, or ordinary fallback.

## Debugging and current limits

Collision debugging is disabled by default. When enabled,
`CaptureCollisionDebugSnapshot` returns at most 4096 current shapes together
with transforms, channels, owners, and the last blocking hit/normal. When
disabled, the capture path returns immediately without walking scene bodies.
The Level Editor viewport's **View mode > Overlays > Collision** toggle consumes
the renderer-independent snapshot to draw Box, Sphere, and Capsule wire shapes
plus hull/mesh feature lines and the latest blocking impact normal without
exposing mutable scene storage. Feature detail is globally capped at 256
triangles per snapshot. StaticMesh Inspector reports mode/policy/status, source
and retained counts, node/depth/bounds, payload/runtime bytes, versions,
revision, key, and diagnostics without providing mutation controls.

The implementation is synchronous and query-only. Programmatic low-level
compounds are qualified; reflected compound authoring is deferred. Writable
collision editing, multiple hulls, convex decomposition, per-feature materials,
public feature IDs, dynamic rigid bodies, forces, constraints, asynchronous
stepping, moving platforms, heightfields, overlap events, and project-defined
profiles remain future work.
The cross-plan sequencing for scalable queries, geometry, cooked collision,
and evidence-gated simulation or backend work is maintained in the
[Aether Physics Evolution Roadmap](../../Roadmaps/AetherPhysicsEvolution.md).
