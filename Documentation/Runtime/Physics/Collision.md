# Runtime Collision

Summary: Define Physics module ownership, World query behavior, immutable primitive and cooked geometry, filtering, component synchronization, and collision debugging.

Modules: PhysicsCore, Physics, Engine

## Ownership and dependencies

Runtime collision follows the one-way dependency chain `Core -> PhysicsCore ->
Physics -> Engine`. `PhysicsCore` owns Engine-independent shapes, handles,
filters, hits, validation, and reference Box/Capsule geometry math. `Physics`
owns the deterministic query-only `FPhysicsScene`; it stores numeric handles
and opaque user tokens and never includes Engine objects. Engine owns body
setups, body instances, profiles, gameplay results, component synchronization,
and the `DWorld` query facade.

Collision debug enablement and last-hit state belong to the registered
[World collision debug subsystem](../World/WorldSubsystems.md). The public
World facade remains the query entry point. Detachment clears old-Level hits
while preserving debug enablement; shutdown clears both.

Each `DWorld` constructs one game-thread-owned `FPhysicsScene`. Components
publish bodies only into their owning Level's current World and remove them
before unregistering. Level replacement, component destruction, and World
teardown therefore cannot leave a body visible in another World. Collision
queries remain available while play is paused or physics simulation is
disabled; that flag gates future simulation stepping, not query-only geometry.

## Geometry resources, shapes, and transforms

`FCollisionShape` remains the compact query-side Box, Sphere, or Z-axis Capsule
value. Bodies instead retain a copyable `FCollisionGeometryRef` to one
validated immutable PhysicsCore payload. A primitive payload contains one
shape. A compound contains 1 through 64 simple children in stable input order,
each with an analytic shape or a convex resource and one valid local transform;
triangle children and nested compounds are rejected. Every payload has
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

`FCollisionGeometryRef::BuildConvexHull` and `BuildTriangleMesh` accept an
optional borrowed cancellation predicate for synchronous authored construction.
Cancellation returns no geometry and reports `ECollisionGeometryBuildStatus::Cancelled`;
it does not change scene query outcomes. Triangle cleanup and BVH construction
check during linear work, sort comparisons and recursive partitions. Callers
must keep the predicate valid for the invocation; no callback is retained by the
immutable geometry. `MakeConvexHull`, `MakeTriangleMesh` and
`MakeCookedTriangleMesh` also accept borrowed predicates and return empty on
cancellation. Final topology/BVH validation and immutable triangle/bounds packing
check every 256 work units. Contiguous allocation/copy calls remain synchronous;
cooperative cancellation does not promise a hard wall-clock deadline.

## Assets and components

Tool [target policy](../Assets/TargetPlatforms.md) is separate from runtime
collision ownership. Physics capture owns an explicit payload target, and action
assembly preserves the existing Win64 identity. MacOS collision payloads remain
unsupported and fail before StaticMesh render/collision production. Cook prepares
detached payloads before serialization; runtime decoding requires no Developer
TargetPlatform service. PhysicsCore remains the algorithm authority for direct
and DDC construction.

`DBodySetup` owns source recipes, query settings and independent immutable
geometry caches. Simple authoring edits invalidate only simple caches; query
settings preserve all geometry and pending cooks. Generated-resource publication
preserves authored simple caches. `FScopedBodySetupUpdate` batches nested edits,
synchronous cooks and publication into one revision and one typed owner
notification at the outer scope exit. Inputs change immediately inside the scope;
this is publication batching, not rollback or concurrent editing. Invalid edits
are rejected before changing authored data. Material and render readiness do not
invalidate collision. Enum setters require declared values.
`IInterface_CollisionDataProvider` exposes `ContainsPhysicsTriMeshData` and
`GetPhysicsTriMeshData`. The latter synchronously returns owning
`FTriMeshCollisionData` arrays; it never returns a callback or deferred data source.
`CreatePhysicsMeshInputTask` is Durin's asynchronous preparation extension: its
repeatable `FPhysicsMeshInputTask::Execute` runs from detached inputs, with no
DObject, provider or render-resource references. Authored StaticMesh tasks capture
canonical source and normalization, then acquire/decode on the worker. The default
provider implementation captures actual arrays on the owner thread; source-less
procedural/debug meshes copy resident LOD0.

`DBodySetup::GetCookInfo` combines prepared geometry with source mode and cache
settings into the owning value `FCookBodySetupInfo`. `FPhysicsCookHelper::Cook`
reads that descriptor. Editor requests execute Engine's registered collision
function, which finishes immutable PhysicsCore cooked arrays before owner-side
geometry assembly. Shared outputs retain cold arrays; cached bytes undergo one
validated native-array conversion. Non-editor explicit cooking calls the linked
PhysicsCore implementation directly. `IMeshBuilderModule` only supplies render construction; its presence
and module lifetime do not affect physics cooking. Editor
cooks retain collision DDC reads/writes; non-editor explicit cooking uses the
linked geometry implementation without DDC. Async admission snapshots the settings first, and the worker fills
`TriangleMeshDesc` after preparation, before cooking. Neither data structure
contains executable preparation logic. `FPhysicsCookResult` owns only the cooked geometry. PhysicsCore's `FCollisionCookedData::GetBuildVersion()` returns the linked cooker's
nonzero opaque 64-bit identity, hashing its private algorithm revision and GLM version.
Engine includes it as the named `BuilderVersion` action constant and validates it
before recipe decoding. The adapter version and constants/output schemas are separate.
All contribute to the package cook fingerprint alongside the DCOL schema.
Collision build function, constants and shared-output schemas are version 3. Query
policy and trace mode belong to BodySetup publication settings, not geometry cook
inputs, keys or shared-output metadata. Changing them reuses the same geometry
cache record. DCOL carries geometry only; loading validates source mode independently
from the asset query settings.
Algorithm upgrades invalidate cached builds without rejecting compatible cooked data.
DCOL schema 3 uses a 60-byte header with no producer word; older payload schemas
require recook. Schema, target, layout, bounds and checksum remain compatibility gates.
Physics input preparation and cooking return ordinary diagnostic strings capped
at 4096 bytes, with the failure stage recorded in text. Cancellation belongs to
the shared invocation token. Recoverable cache issues are logged once
at execution with function, key, operation and bounded original cause; cook results
carry no warning list. Private cache codecs use the existing failure type rather
than expose a separate codec error domain.
`PhysicsDerivedData.h/.cpp` owns the DCOL payload and typed validation failures;
`PhysicsCookDerivedDataKey.h/.cpp` owns the collision cache key and key failures.
The shared archive platform identifier belongs to Asset. Payload identifiers,
canonical bytes and schema versions are independent of the DDC namespace. Collision
derived data uses the flat `StaticMeshCollision` bucket; its spelling is a cache
compatibility identity, not a code dependency.

`DBodySetup::InvalidatePhysicsData` withdraws unstarted prior requests and clears
only generated geometry. Repeated invalidation before the next request/publication
reuses the same invalidated generation. Simple authoring and query edits preserve
that request generation; source recipe/input changes supersede it.
`CreatePhysicsMeshesAsync` accepts a provider capture and submits bounded work;
`CreatePhysicsMeshes` also waits, while `FinishPhysicsMeshes` waits for that setup
only. Accepted asynchronous work delivers `FOnAsyncPhysicsCookFinished` once on
the owner-thread pump. Its `FPhysicsCookCompletionResult` distinguishes Succeeded,
Failed, Cancelled and Superseded; only Failed carries diagnostic text.
The asynchronous interface returns void. Input/admission failures notify once inline,
as does disabled collision; accepted work notifies after task retirement. A synchronous
creation waits and exposes its outcome through the setup's status and error queries.
Callbacks may reenter creation and must tolerate owner destruction.
`AbortPhysicsMeshAsyncCreation` invalidates publication for the selected request
and withdraws only cooks whose worker has not started. Worker entry and withdrawal
compete through one atomic queued-to-running transition. Withdrawn work reports
Cancelled without executing its detached input task. Started cooks, including
DDC waits and CPU consumption, finish normally without owner cancellation being
forwarded to their task or request owner; stale generations report Superseded and
cannot install geometry or overwrite the current error. Abort does not wait.
Destruction invalidates owner identity/publication while detached records remain
alive until worker and DDC callback retirement. Selected finish, source-mutation
barriers and compiler shutdown still drain those records before releasing products
or returning their single completion notification. Shutdown closes admission and
withdraws queued work while waiting for started cooks to retire.
Retirement uses producer task completion together with `FRequestOwner::Poll()`;
the latter includes callback return and registered native dispatch cleanup. Result
consumption reacquires CPU capacity only after both conditions hold. Selected finish
uses owner `Wait()` after producer completion, without an asset completion event.
The setup owns `EPhysicsMeshBuildStatus` and an optional diagnostic string;
there is no second owner-error wrapper. Cancellation clears the stored failure.
`ApplyPhysicsMeshes` returns Succeeded, Superseded or Failed using the completion
status enum. Stale results do not
change live state; incompatible geometry on a current request stores an Installation
failure. Workers never mutate the setup. Physics records retain a shared cancellation
token for queued withdrawal, input cancellation and native/DDC infrastructure
cancellation, rather than a duplicate terminal status. Owner abort does not set
that token after worker entry. Finalization forms a local completion
result without replacing worker output with an installation error. After retirement
and record removal, the GameThread moves the cook outcome and any remaining DDC
completion into local ownership. Installation and notification use that local
outcome; ordinary destruction releases its products after notification or during
exception unwinding, without an output scope guard. Completion processing
uses the aggregate manager's count/deadline rather than a separate mesh time slice.
Physics uses the asset execution scheduler independently of StaticMesh. Pumping or
finishing physics can dispatch ready render CPU continuations, without accessing
the render manager or applying its results. Pending DDC requests occupy no asset
execution slots; their memory/record reservations remain until terminal retirement.

Source-recipe setters invalidate generated collision and schedule when a collision source
is available, even without CPU RenderData. Missing source retains configuration as
Unavailable; explicit `RebuildCollision` reports a missing-source failure.
Failure retains the new configuration without derived geometry and leaves rendering
usable. Mesh status/error accessors forward to BodySetup. Ready includes disabled
derived collision. Unchanged settings do not retry; use `RebuildCollision`.

Each collision request owns a detached input task and cook descriptor; the mesh retains
its setup and installed immutable geometry, without a per-generation snapshot cache. Collision is opt-in: `None`, `ConvexHullFromLOD0`, or
`TriangleMeshFromLOD0`; imported meshes never silently use render bounds or triangles.
`SimpleOnly`, `ComplexOnly`, and `SimpleAndComplex` retain their existing
representation-availability rules without changing component filters.
`FCollisionQueryParams::Complexity` (forwarded to `FPhysicsQueryFilter`) accepts
`Simple` or `Complex`, with `Simple` as the default. Triangle-only bodies require
an explicit Complex query or `ComplexAsSimple`; queries do not automatically
select the other representation.

`EBodySetupCollisionTraceMode` selects `SimpleAndComplex`, `SimpleAsComplex`, or
`ComplexAsSimple`. BodySetup supplies available representations and this policy
through `PopulateCollisionGeometry`; `DPrimitiveComponent::BuildPhysicsBodyDesc`
publishes geometry, transforms, filters, and owner identity in one pass.
`FPhysicsBodyDesc` has only Simple/Complex geometry and their independent world
transforms. There is no third default geometry or legacy kind-based inference.
At scene registration/update, trace policy is resolved into per-representation
Simple/Complex query masks. SimpleAsComplex publishes only simple geometry when
available and enables both query masks; ComplexAsSimple does the equivalent for
complex geometry. If that preferred representation is absent, the other kind is
retained with its own query mask only, matching UE 5.8 shape creation. Asset
availability policy is applied before this resolution and cannot be bypassed.
Each query matches the published mask without reinterpreting TraceMode. The
reference, production and Compare executors share that selection.

Bounds, spatial-state comparisons, resource retention and debug snapshots use
the published representations. Debug entries identify their Simple/Complex
representation and include both independent offsets. Invalid or empty publications
are rejected atomically, preserving an existing registration on failed updates.
`RetainedBodyBytes` reports body-record capacity independently from
`RetainedSpatialBytes` (slot and tree capacity) and `RetainedGeometryBytes`
(unique immutable payloads); diagnostics reset preserves these resident values.

`DBodySetup::SetSimpleCollision` atomically replaces an aggregate of at most 64
mesh-local `FSimpleCollisionElement` values (Box, Sphere, Capsule, Convex).
Invalid dimensions, transforms, point sets or topology reject the whole edit.
Convex elements retain validated hull vertices and triangle topology in both
authored and cooked packages; immutable resources are reconstructed lazily
without imported source data or Developer modules. `SetBox`, `SetSphere`, and
`SetCapsule` write one element through the same aggregate validation and cache;
there is no separate runtime shape, dimensions, center, or primitive cache.
Empty aggregates remove authored simple geometry without exposing an older shape.
An authored aggregate pairs each base shape or hull with its element-to-mesh
local transform in a child. This does not require baking transforms into vertices
or storing the component's world transform in reusable geometry. Generated hulls
already use mesh-local vertices. Consumers apply the component transform outside
the aggregate. `PopulateCollisionGeometry` supplies these mesh-local resources
with separate component world transforms. StaticMesh components publish the complete
aggregate through this path; they expose no single-shape extraction override.
Box/Sphere/Capsule components retain their analytic shape API for movement queries.
The aggregate is independent of the triangle source recipe, allowing authored
simple elements and collision-LOD/proxy triangles in the same body. All authored
entry points use the same selection: authored simple elements take precedence
over a generated hull. Empty authored data uses generated simple geometry when
available. There are no per-entry-point selection flags or single-shape package
migration routes.
The qualified `/Engine/Models/Box` asset
continues to derive its authored Box setup from verified LOD 0 bounds.

`DStaticMesh::SetComplexCollisionMesh` optionally replaces complex triangle input.
The editor-only reference selects the proxy's own geometry in one hop; its own
proxy reference is ignored and self-reference is rejected. Simple primitives and
`ConvexHullFromLOD0` continue to use the primary asset. Clearing the reference
restores primary triangles; an unavailable selected proxy fails without silently
falling back to visual geometry. Existing defaults remain unchanged.

`LODForCollision` defaults to 0 and selects a fixed complex collision level,
clamped to the available range at input capture. It never follows camera distance
or render LOD selection. A selected proxy delegates to its own `LODForCollision`
in one hop; the primary asset's setting does not override it. Simple primitives
and convex cooking stay on primary LOD0. Changing the setting invalidates collision
input and direct consumers without republishing rendering, while deferred spline
edits keep their old body until application. Cook retains the selected CPU input
for non-default LODs as well as proxies; cooked runtime selection is fixed and
does not require editor geometry or GPU initialization. Collision enablement,
query policy and default LOD0 behavior are unchanged.

Authored proxies share the primary mesh's original source coordinate system.
Their vertices use the primary source bounds and `NormalizedSize`, never their
own bounds or size. Source-less procedural vertices are already canonical local
coordinates and are copied unchanged. Resolved proxy positions and indices are
persisted on the primary asset, including Cooked packages, for CPU-only runtime
spline deformation without loading the proxy or editor source. Cook declares a
direct source-package dependency and refreshes the resolved input before capture.

Collision input has an asset-local revision independent of render-resource
publication and BodySetup cook completion. Source replacement, normalization and
proxy selection refresh direct proxy consumers and rebuild spline collision.
Pending component edits retain their applied physics body until `UpdateMesh`;
provider capture rejects a changed collision-source revision until application.
Collision-only changes preserve the spline render snapshot. Typed body changes
refresh StaticMesh components and only dependent simple-collision splines in one
object traversal. Source hull completion refreshes splines using that hull; triangle
completion does not rebuild splines from unchanged triangle inputs. Notifications
never recursively invalidate asset input sources.

Editor derived data uses the separate `StaticMeshCollision` namespace
and a key containing collision builder/schema/platform versions, exact source
geometry hash, source mode, and target platform. Asset commits with changed source
or normalization invalidate prior derived collision and schedule independently.
Same-geometry commits and pure render publication preserve physics resources.
Failure cannot roll back accepted render/source state.
Invalidation and successful collision installation refresh registered component
physics bodies through a collision-specific notification. Render-data notifications
refresh only rendering for StaticMeshComponent. Stale results are rejected using
BodySetup identity, request generation, source mode and provider registration.
Unrelated query/simple changes do not supersede in-flight generated work. Render-only pending work is
not collision readiness. Authored primitives survive render replacement.
`DBodySetup::InstallPhysicsMeshes` validates externally supplied immutable products,
supersedes prior requests, and publishes without accepting build provenance or
scheduling compilation. Async completion publishes through its matching request
generation. Late success or failure cannot overwrite an externally installed result.
Engine logs bounded cache warnings internally; cache-origin history is not exposed.
A cache miss or corruption is rebuildable only
while detached source inputs exist.

Cook writes independently versioned render and optional required collision
slots in one `FCookContext` transaction. Cooked BodySetup state contains policy
and the exact collision descriptor but no source snapshot or DDC key. Runtime
validates both descriptors and payloads before publishing either, and
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

Physics-state creation has an explicit primitive/world policy. `Eager` creates
the body during registration, `OnDemand` leaves registration body-free until a
tool requests readiness, and `DeferredRequired` schedules immutable CPU work
while requiring an owning-thread readiness barrier before gameplay. Analytic
and ordinary mesh primitives remain Eager. Policy selection is a component contract rather than a branch in primitive
registration.

Spline simple collision is enabled explicitly with `DeformedSimpleCollision` or
`DeformedSimpleAndComplex`. Elements come from the primary mesh BodySetup, never
its complex proxy or collision LOD. Boxes become eight-corner convex hulls;
convex points are transformed into mesh space, deformed, and rebuilt separately
with identity element transforms. Multiple elements retain their gaps instead
of becoming one enclosing hull. Spheres use the center slice and maximum axis
scale. Capsules use that radius approximation and transform the cylindrical
axis/length; Durin half-height includes caps, unlike UE Sphyl.Length. Forward
axes, roll, signed spline cross-section scale and element transforms participate
in this conversion. Component/world physics transforms still require positive
scale. These modes inherit the source trace mode; SimpleAsComplex skips unused triangle
construction, while ComplexAsSimple discards simple
elements and uses deformed complex geometry, including for simple queries.
Source settings and completed source hull cooks refresh applied spline bodies;
deferred edits retain their previous body until `UpdateMesh`.

`DSplineMeshComponent` adds an explicit
`ESplineMeshCollisionMode` policy. Disabled is
the default and builds or retains no collision BVH. DeformedTriangleMesh obtains
source triangles through the StaticMesh collision provider, deforms them with
the applied Spline parameters, and cooks one immutable triangle resource through
a transient component-owned `DBodySetup`. It does not inherit disabled collision
or Simple/Complex query policy from the source asset. The component uses the
BodySetup revision for physics publication; request generations reject stale
build results. `UpdateMesh()` retains synchronous collision application and
disables persistent cache writes for dynamic segment products. Rendering
can remain valid when all deformed triangles are degenerate and collision is
therefore invalid. A successful deformation publishes the new immutable render
state before ordinary physics-state replacement, so the old handle is retired
and no stale handle remains queryable. Disable, source removal/reimport,
component retirement, Actor segment removal, PIE teardown, and World teardown
release the body and shared geometry normally. CPU collision capture never
constructs an editor-picking acceleration hierarchy; viewport selection uses
the renderer's GPU HitProxy pass.

`ASplineMeshActor` defaults generated segments to Disabled plus `NoCollision`.
Its reflected path collision policy enables DeformedTriangleMesh and QueryOnly
consistently for every segment. The 256-vertex/254-triangle road-strip
qualification measures geometry construction and retained collision resources;
it does not measure the component provider and BodySetup cook lifecycle.

`AStaticMeshActor` selects `WorldStatic`; an independently created
`DStaticMeshComponent` remains `NoCollision`. `DBoxComponent`,
`DSphereComponent`, and `DCapsuleComponent` publish analytic geometry without
render ownership.

## Profiles, queries, and results

Built-in profiles are `NoCollision`, `BlockAll`, `WorldStatic`, `Pawn`, and
`Trigger`. `FBodyInstance::CollisionProfileName` selects an authoritative preset;
an empty name selects Custom. Editing an individual filter setting leaves the
preset. The response container and every channel response participate in reflected
serialization, including Custom settings. Struct post-deserialization resolves
named presets before runtime publication and rejects unknown names. Setters and
editor callbacks use the same side-effect-free profile resolver.

The editor collapses `FBodyInstance::Responses` by default. The response array
declares `ArrayIndexEnum=Durin::ECollisionChannel` metadata, so the generic property
view displays only named channel indices, with an Ignore/Overlap/Block enum
selector for each channel. All 32 response slots remain reflected and serialized;
the index metadata changes presentation only.

`CollisionProfileName` is the only persisted profile field. Repository packages
were resaved before removing `ProfileName_DEPRECATED` and its load conversion.
Named profiles still restore their complete filter data; physics handles and
publication revisions remain transient.

Responses are resolved from both the querying channel and body
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

PhysicsCore owns operation/pair selection and compound iteration behind one
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

PhysicsCore geometry functions accept an optional
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
with transforms, channels, owners, resource identity/bytes, and the last
blocking hit/normal. When
disabled, the capture path returns immediately without walking scene bodies.
The Level Editor viewport's **View mode > Overlays > Collision** toggle consumes
the renderer-independent snapshot to draw Box, Sphere, and Capsule wire shapes
plus bounded hull/mesh feature lines and the latest blocking impact normal
without exposing mutable scene storage. Feature detail is globally capped at
256 triangles per snapshot. StaticMesh Inspector reports mode/policy, actual geometry readiness, source
and retained counts, nodes/bounds, runtime bytes, versions, and
revision coherence without retaining or displaying cache-operation history.
Complex source counts use resident selected collision input, including proxies
and lower LODs; unavailable input is not compared against visual LOD0 counts.

The implementation is synchronous and query-only. Programmatic low-level
compounds are qualified; reflected compound authoring is deferred. Writable
collision editing, multiple hulls, convex decomposition, per-feature materials,
public feature IDs, dynamic rigid bodies, forces, constraints, asynchronous
stepping, moving platforms, overlap events, and project-defined
profiles remain future work.
The cross-plan sequencing for scalable queries, geometry, cooked collision,
and evidence-gated simulation or backend work is maintained in the
[Physics Evolution Roadmap](../../Roadmaps/Archive/2026-08/PhysicsEvolution.md).
