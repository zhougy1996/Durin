# Spline System

Summary: Define spline component data, evaluation, scene representation, and viewport editing behavior.

Modules: Engine, LevelEditor

Durin's spline foundation provides an editable, persistent spatial curve, an
immutable query snapshot, and direct Level Editor point/tangent authoring. It
also provides value-only SplineMesh deformation, parallel-transport frame
math, a production SplineMesh primitive, and a native whole-path Actor. It does
not provide a path follower, procedural extrusion, or placement system.

## Support Summary

| Area | Current support |
| --- | --- |
| Curve data | Stable point GUID, position, arrive/leave tangents, outgoing interpolation, tangent mode, and loop state |
| Interpolation | Linear or cubic outgoing segments; automatic, automatic-clamped, manual-aligned, or manual-broken tangents |
| Runtime queries | Structured samples by parameter or local distance, parameter/distance conversion, local length and bounds, and nearest parameter |
| Coordinate spaces | Component-local and transformed world-space samples; distance remains component-local |
| Evaluation | Immutable snapshots with adaptive distance tables, conservative bounds, and concurrent read access |
| Editing | Selected-point Details editing and transactional viewport point/tangent manipulation, insertion, structural actions, Undo/Redo, and Cancel |
| Persistence | Reflection, object-graph duplication, level-package save/load, point-ID repair, and post-load snapshot publication |
| Higher-level consumers | `ASplineMeshActor` reconciles one transient segment component per outgoing point GUID |

## Authoring Model

`FSplineCurve` owns reflected authoring data only. Each `FSplinePoint` contains:

- a component-local stable `FGuid` identity;
- local `Position`, `ArriveTangent`, and `LeaveTangent` vectors;
- `OutgoingInterpolation`, which is `Linear` or `Cubic`; and
- `TangentMode`, which is `Automatic`, `AutomaticClamped`, `ManualAligned`, or
  `ManualBroken`.

The interpolation value belongs to the segment leaving the point. Open curves
have one fewer segment than point; closed curves add a last-to-first segment
identified by the last point's GUID. No synthetic seam point is serialized.
Empty and one-point curves have no segment.

Point GUIDs are unique within one curve. Existing IDs survive reorder,
component duplication, and package round trips. Append, insertion, and point
duplication create fresh IDs. Mutations and reflected load/edit boundaries
repair invalid or duplicate IDs before publishing evaluation data.

`FSplineCurve` supplies explicit point mutation helpers for set, add, insert,
duplicate, update, remove, move, clear, and ID repair. `DSplineComponent`
wraps those mutations, publishes a new evaluation snapshot, updates its spline
revision and change flags, and marks persistent component edits through the
normal package/transaction path.

## Segment Geometry And Tangents

Linear segments interpolate positions and have constant first derivative and a
zero second derivative. Cubic segments use Hermite geometry from the starting
point's leave tangent and ending point's arrive tangent. Tangents are
derivatives with respect to the segment-local parameter, not normalized
directions. `FSplineSample::Direction` is the normalized first derivative, or
zero when that derivative is degenerate.

Automatic tangents are chord-length aware. For non-degenerate incident chords
with lengths `h0` and `h1`, the knot derivative is:

```text
(h1 * normalize(P - Pprev) + h0 * normalize(Pnext - P)) / (h0 + h1)
```

The arrive and leave handles scale that derivative by `h0` and `h1`.
Open endpoints use their sole incident chord; closed curves wrap neighbors
across the seam. A degenerate chord produces a zero handle on that side, and
two degenerate chords produce two zero handles.

`AutomaticClamped` additionally zeros both handles when the derivative would
reverse against either incident chord and caps each handle at its incident
chord length. `ManualBroken` preserves independently authored handles.
`ManualAligned` keeps the two stored derivative vectors collinear in the same
path direction while preserving independent magnitudes; editing either handle
updates the direction of both. The arrive visualization handle is located at
`Position - ArriveTangent / 3`, and the leave handle at
`Position + LeaveTangent / 3`.

Spline geometry deliberately contains no constant spatial segment, point
rotation, point scale, transform sampling, roll, width, or orientation-frame
policy. A consumer that needs orientation must define it from differential
geometry and its own up/frame rules.

## Parameters And Samples

`FSplineParameter` contains `SegmentIndex` plus a segment-local `T` in
`[0, 1]`. `FSplineSample` contains position, first derivative, second
derivative, and normalized direction.

Open-curve parameters clamp to the curve endpoints. Closed-curve parameters
wrap; the explicit end parameter evaluates at the first point. Empty curves
return neutral finite samples. A one-point curve returns that point's position
with zero derivatives, direction, and length.

`FSplineEvaluationData` supports:

- `Evaluate()` and `EvaluateAtLocalDistance()`;
- parameter/local-distance conversion;
- local length and conservative local bounds; and
- nearest-parameter lookup for a local position.

`DSplineComponent` exposes the same sampling operations and can transform a
sample between local and world coordinate space. World conversion transforms
positions as points, derivatives as vectors, and recomputes direction from the
transformed first derivative. Nearest lookup accepts either local or world
input.

## Local Distance, Bounds, And Nearest Queries

Distance is always measured in authored component-local space. API names use
`LocalDistance` and `LocalLength` to make this domain explicit. Translation and
rotation do not change local length. Uniform and non-uniform component scale
change returned world positions and derivatives but do not redefine the
distance domain; sampling equal local-distance steps therefore does not
guarantee equal world-space travel under non-uniform scale.

Each cubic segment receives an adaptive monotonic distance table. The default
builder uses an absolute local-length tolerance of `1e-4`, a relative tolerance
of `1e-5` times the segment Bezier control-polygon length, and maximum depth
`16`. Recursive children split the parent error budget equally. Linear and
degenerate segments remain deterministic.

Open-curve local distances clamp to the endpoints. Closed-curve distances wrap,
while the explicit end-distance query can still represent the full loop
length. Parameter/distance inversion searches the immutable monotonic tables.

Segment bounds conservatively include the complete curve, using the equivalent
Bezier control hull for cubic geometry. Nearest lookup uses those immutable
segment records for coarse rejection, then performs safeguarded local
refinement. It returns a parameter; callers evaluate that parameter when they
need the corresponding sample.

## Immutable Evaluation And Invalidation

`FSplineCurve::BuildEvaluationData()` builds a
`shared_ptr<const FSplineEvaluationData>`. `DSplineComponent` atomically
publishes that immutable snapshot after construction on the owning
game/editor thread. Queries capture one shared pointer and may read it
concurrently without lazy cache mutation or `const_cast`.

Every component starts with a valid published snapshot. Successful component
mutations, reflected edits, `PostLoad`, duplication, Undo, and Redo repair
authoring data as needed and republish. `GetSplineRevision()` identifies the
latest publication, and `GetLastSplineChangeFlags()` reports the reason:

- `Topology` for point/segment identity or ordering changes;
- `Geometry` for shape changes; and
- `Build` for an explicit rebuild of unchanged authoring data.

Consumers may subscribe to the post-publication mutation event. The callback
receives the revision, change flags, and exact immutable snapshot already made
authoritative. Publication forbids reentrant spline mutation, snapshots
listener identities, and permits listener removal during callback delivery.
`ASplineMeshActor` uses this event instead of Tick polling. Other consumers that
retain derived data should compare revisions and must not mutate a snapshot.

## SplineMesh Deformation and Path Frames

`FSplineMeshParams` describes one independent local cubic Hermite interval.
Positions and tangents are component-local; tangents are derivatives with
respect to segment `T` and receive no implicit length multiplier. Roll is in
radians. Scale and offset use the two cyclic cross axes perpendicular to the
selected X, Y, or Z source forward axis. The default attribute interpolation
is linear; smoothstep is available explicitly.

`FSplineMeshDeformer` is the finite CPU authority for position, derivative,
orthonormal frame, direction/normal transformation, and conservative bounds.
It rejects non-finite parameters and a non-positive canonical LOD 0 forward
extent atomically. A singular up projection uses the least-aligned cardinal
axis; a zero derivative falls back to the endpoint chord and then the selected
source axis. Frames are right-handed with `Forward × Side = Up`.

`FSplinePathFrameData` builds immutable consumer-owned frames over the existing
adaptive distance samples. It uses deterministic minimal-rotation transport,
retains the preceding direction across zero derivatives, and distributes
closed-loop seam correction by local distance. This state is derived and does
not add roll, scale, or orientation fields to `FSplinePoint`.

## Level Editor Authoring

Selecting a spline component exposes component settings and the current stable
point selection in Details. The contextual Spline viewport mode visualizes the
adaptive curve, point markers, and manual tangent handles. It supports stable
GUID selection, point multi-selection, translation-only gizmo targets,
shape-preserving linear or cubic segment insertion, append, duplicate, delete,
reorder, loop, interpolation, and tangent-mode edits.

Continuous point and tangent drags commit one transaction. Cancellation and
net-zero drags restore the original values and package dirty state without a
history entry. Structural operations snapshot the complete authoring point set
and loop state so Undo/Redo restores geometry and republishes evaluation data.
Selection remains GUID-addressed, so reorder and transaction replay do not
silently select a different logical point.

See [Scene Viewport Navigation](../../Editor/Guides/SceneViewportNavigation.md)
for the user workflow and
[Viewport Editing Architecture](../../Editor/Architecture/ViewportEditing.md)
for mode, selection, input, and gizmo ownership.

## Standalone SplineMesh Component

`DSplineMeshComponent` is a reflected `DMeshComponent` that owns authored
`StaticMesh`, positional material overrides, and `FSplineMeshParams`. Its
validated setter derives the canonical forward interval from StaticMesh LOD 0;
the hidden interval is never trusted as independent authoring. Invalid finite,
axis, interpolation, or extent proposals are rejected before the authored
object, package state, or published CPU state changes.

Every accepted mesh-resource or deformation change atomically publishes an
immutable `FSplineMeshDerivedState`. The state copies normalized parameters,
conservative all-LOD local bounds, exact deformed LOD 0 positions and indices,
an exact-query acceleration hierarchy, the source resource revision, a
monotonic deformation revision, collision input identity, optional immutable
deformed triangle collision, and a diagnostic
status. Consumers retain this value snapshot rather than borrowing mutable CPU
asset data. Missing assets and temporarily unavailable source data are explicit
diagnostic states; malformed indexed geometry is never published as valid.

StaticMesh and SplineMesh components share positional material-override
validation and trailing-null canonicalization. StaticMesh resource exchange
uses the same scoped retirement protocol for both consumer classes: registered
components release old render state before exchange and rebuild their CPU state
after the new resource publication. Deformation-only edits retain the source
asset and component identity, update exact editor picking and collision input,
and do not request proxy recreation. The component publishes deformation and
bounds together through one FIFO scene update while retaining primitive and
source GPU identities. `ESplineMeshCollisionMode::DeformedTriangleMesh`
explicitly opts into collision construction; Disabled retains no collision BVH.

The renderer classifies SplineMesh as its own typed primitive family. Prepared
mesh work carries an explicit Local/Spline/Skeletal vertex-deformation domain
through shader-map, pipeline, and draw-sort identity. The Spline vertex shader
borrows StaticMesh LOD streams and applies the CPU-authority Hermite/frame
translation without copying source geometry per component. Material slots,
opaque/masked/translucent ordering, visibility, LOD, lighting, auxiliary views,
and resource recovery remain shared mesh-pass policy.

## Native SplineMesh Actor

`ASplineMeshActor` owns a native-default `DSplineComponent` root and persistent
mesh, material, axis, scale, roll, offset, frame seed, interpolation,
visibility, and collision policies. A construction pass captures one immutable
spline snapshot, builds one parallel-transport frame result, prepares the
complete desired segment set, and acquires generated components by
`("SplineMeshSegment", outgoing-start-point GUID)`. Closed loops include the
last-to-first seam; empty and one-point curves produce no generated component.

Geometry edits reuse identities. Insert, delete, reorder, loop toggle, load,
duplication, PIE cloning, Undo/Redo, Cancel, and runtime mutation all enter the
same synchronous native-construction path. Generated segments are transient,
attach to the spline root, never serialize or dirty the package during
reconstruction, and route registration and play state with the owner. Details
reports generated count and construction diagnostics and exposes **Edit
Spline**. Generated hierarchy rows remain visible for diagnostics but are
labeled read-only and cannot be renamed, duplicated, deleted, reordered,
reparented, or edited as authoring.

## Verification Coverage

Native tests cover golden linear and cubic evaluation; every tangent mode;
open, closed, empty, one-point, duplicate-position, and zero-tangent cases;
adaptive local-length error and monotonic inversion; bounds and nearest lookup;
concurrent snapshot reads; point-ID mutation and repair; reflection,
duplication, package round trips, revision/change flags, local/world conversion,
and Undo/Redo. Viewport tests cover typed hits, selection, shape-preserving
insertion, point/tangent targets, transactions, cancellation, read-only and
invalid-target exits, and multi-component isolation.

## Current Boundaries

SplineMesh is a single-segment deformation primitive and the Actor is a simple
shared-mesh whole-path policy owner. It does not implement extrusion, junctions,
caps, per-segment assets, navigation, animation, events, width metadata, or
world-distance traversal under non-uniform scale. Simultaneous editing across
multiple spline components is also outside the implemented contract.
