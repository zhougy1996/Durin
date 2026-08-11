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

## Debugging and current limits

Collision debugging is disabled by default. When enabled,
`CaptureCollisionDebugSnapshot` returns at most 4096 current shapes together
with transforms, channels, owners, and the last blocking hit/normal. When
disabled, the capture path returns immediately without walking scene bodies.
The Level Editor viewport's **View mode > Overlays > Collision** toggle consumes
the renderer-independent snapshot to draw Box, Sphere, and Capsule wire shapes
plus the latest blocking impact normal without exposing mutable scene storage.

The implementation is synchronous and query-only. Dynamic rigid bodies,
forces, constraints, asynchronous stepping, moving platforms, triangle meshes,
heightfields, overlap events, and project-defined profiles remain future work.
