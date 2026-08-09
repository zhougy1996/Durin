# Spline System

Summary: Define spline component data, evaluation, scene representation, and viewport editing behavior.

Modules: Engine, LevelEditor

Durin's spline foundation provides an editable, persistent spatial curve, an
immutable query snapshot, and direct Level Editor point/tangent authoring. It
does not provide a spline mesh, path follower, placement system, or another
production consumer.

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
| Higher-level consumers | None in production code beyond Level Editor integration |

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

Consumers that retain derived data should compare the revision and react to
the relevant flags. They must not mutate or assume object identity for a
snapshot after a later revision is published.

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

The spline foundation has no production spline mesh, runtime renderer, follower,
placement, animation, navigation, physics, event, orientation-frame, width, or
metadata consumer. True world-distance traversal under non-uniform scale and
simultaneous editing across multiple spline components are also outside the
implemented contract.
