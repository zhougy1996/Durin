# Spline System

Summary: Define spline authoring, immutable evaluation, and generated SplineMesh ownership.

Modules: Engine, LevelEditor

## Authoring Model

- `FSplineCurve` owns reflected points and loop settings. The array index
  addresses an authored point, and `InputKey` addresses evaluation.
- Keys are finite and strictly increasing. Position-only insertion assigns a
  key, and reordering moves points through existing key slots. Load and
  reflected mutations normalize invalid keys before publication. Bulk point
  updates validate a detached curve and optional loop key before committing.
  Auto-key runs interpolate between explicit neighbors or use unit steps at the
  ends. Invalid candidates leave authoring, evaluation, history, and Dirty
  state intact.
- Each point owns the interpolation of its outgoing segment. An open curve
  has one fewer segment than points. A closed curve adds a last-to-first
  segment with a default end key of last key plus one and an optional override.
  Empty and one-point curves have zero segments.

## Evaluation And Invalidation

- Authored tangents differentiate against `InputKey`. Hermite coefficients
  multiply a tangent by its segment key span. `Evaluate({SegmentIndex, T})`
  differentiates against segment-local `T` and preserves that segment's
  endpoint derivative. `EvaluateAtInputKey` differentiates against Key and
  clamps to the key domain. SplineMesh and RoadWeaver consume segment-local
  derivatives.
- Position supports linear, cubic, and constant segments. Constant position
  holds until the next key. Its distance jumps by the endpoint chord at the
  end of the segment, and inverse queries inside the jump return its start.
  Automatic tangent modes use neighboring keys, with per-axis clamping in
  AutomaticClamped. ManualAligned shares direction with independent lengths.
- Rotation and scale are independent automatic cubic channels on the same
  keys. Rotation uses quaternion Squad and scale uses vector Hermite.
  Controls wrap across the seam. Transform sampling combines the position
  tangent with sampled rotation applied to the component default Up.
- `LocalDistance` means component-local arc length. Distance-along-spline
  queries account for component scale and cache by source snapshot and scale.
  Time maps duration to the key range or to scaled distance at constant speed.
  Local-distance queries wrap on closed curves. Closest-to-world-location
  transforms the input to local space before measuring proximity.
- Adaptive distance samples bound both geometric length error and
  parameter-to-distance interpolation error. Segment bounds use the cubic
  Bezier control hull. Keep collinear nonuniform speed and inflections covered
  when changing the distance builder.
- `DSplineComponent` publishes immutable `FSplineEvaluationData` on the
  owning game/editor thread. Readers take one shared snapshot. Batch edits
  publish once. Mutations, load, duplication, and history replay rebuild
  derived state and advance the revision. Listeners receive the published
  snapshot and change flags. Callbacks may unsubscribe during publication and
  must avoid reentrant spline mutation.

## Level Editor Authoring

The viewport and Details edit persistent points through the component mutation
API and transactions. Selection and live transform targets address points by
index and are invalidated by topology revisions. Insert and duplicate may
select the newly created point, and undo and redo restore authored point order.
A committed point gesture can replay after a structural undo in the same
history. Point edits record the component once per transaction. After an unrecorded
or foreign-history reorder, Undo restores the captured curve and Redo restores
the curve observed at Undo. A gizmo gesture addresses a point by index, so
a foreign topology change expires that custom command. The
[transaction buffer](../../Editor/Architecture/Transactions/Transactors.md#structural-history-and-barriers)
skips an expired gesture as a whole, including its other targets.
Editable keys use the runtime validation rules. The Up/Right overlay samples
the runtime transform on segments adjoining selected points and stays outside
the hit-test pass. Linear and cubic insertion preserves centerline geometry,
seeds the new point with the sampled rotation and scale, and recomputes
automatic attribute tangents. A continuous drag creates one history entry.
Cancellation and net-zero edits restore the original values and Dirty state.

See [Viewport Editing](../../Editor/Architecture/LevelEditing/ViewportEditing.md)
for ownership and [Scene Viewport Navigation](../../Editor/Guides/SceneViewportNavigation.md#editing-splines)
for controls.

## Standalone SplineMesh Component

- `FSplineMeshParams` defines one local cubic Hermite interval. Its tangents
  differentiate against segment `T`, roll uses radians, and the forward axis
  selects the two cross axes. The source interval comes from visual LOD 0
  regardless of collision LOD or proxy. The deformer handles degenerate
  derivative and Up inputs with deterministic finite fallbacks.
- `DSplineMeshComponent` owns authored mesh, positional material overrides,
  and deformation parameters. Validation rejects invalid proposals before
  mutation. `bUpdateMesh=false` retains the applied state until `UpdateMesh`.
  A failed deformation application keeps the accepted authored values and
  invalidates old rendering, picking, and collision. Collision failure clears
  the body and can leave rendering valid. Inspect `GetMeshUpdateError()` and
  `GetCollisionBuildError()` separately.
- The render snapshot publishes normalized parameters and conservative
  all-LOD bounds together. Deformation-only updates retain proxy and source
  mesh identity while ready. Resource exchange and readiness transitions retire
  old render state before replacement. GPU HitProxy picking uses the same
  spline deformation as drawing.
- Collision is opt-in and uses a transient component-owned BodySetup. Capture
  uses the applied parameters and current source geometry, with owned data
  for detached work and no retained CPU picking cache. Deferred source edits
  block new captures until application. Follow
  [Collision](../Physics/Collision.md#assets-and-components) for trace modes,
  provider semantics, cooking, and physics publication.

## Generated Paths And Validation

`ASplineMeshActor` owns the persistent path and transient segments. It
rebuilds non-constant outgoing segments by segment index, including the closed
seam. It uses parallel-transport frames with seam correction,
independent of authored rotation and scale. Reconstruction follows
[Native Actor Construction](NativeConstruction.md) and driven road geometry
follows [Road Geometry](RoadGeometry.md).

Use `SplineTests` for production curve, package round-trip, mesh deformation,
and collision contracts. Use `ViewportTests` for index selection, key-aware
insertion, previews, picking, and transaction replay. Mesh lifetime and road
station changes also warrant the relevant World and RoadWeaver tests.
