# Spline V2 and Viewport Editing Plan

Summary: Replace the early spline data model with a geometry-first runtime and a reusable viewport edit-mode framework that culminates in transactional point and tangent authoring.

Last reviewed: 2026-08-05

Status: Completed
Completed: 2026-08-05

## Current Status

- Stages 1 and 2 are complete as one atomic implementation change. Directly
  replacing the public point/evaluation schema also invalidated every
  `DSplineComponent` and Level Editor call site, so the two stages were merged
  to keep the completion baseline buildable instead of committing an
  intentionally broken intermediate repository.
- Stage 1 handoff: the pre-stage baseline is `da376e48`; the completion baseline
  is this combined stage commit. The working set is `SplineTypes.h`,
  `SplineCurve.h/.cpp`, and `SplineTests.cpp`. V2 now has stable point IDs,
  independent interpolation/tangent modes, structured parameters/samples,
  immutable evaluation data, adaptive local-distance tables, conservative
  Bezier-hull bounds, nearest-parameter refinement, degeneracy rules, and
  concurrent read coverage. V1 Constant/rotation/scale/transform/fixed-step
  declarations have been removed. There are no open geometry questions.
- Stage 2 handoff: the same completion baseline integrates V2 through
  `SplineComponent.h/.cpp`, the passive Spline editor customization, and its
  viewport regression. `DSplineComponent` atomically publishes immutable
  snapshots, exposes revision/change flags, repairs point IDs, transforms
  samples without changing local distance, and rebuilds after mutation,
  reflection, load, duplication, Undo, and Redo. Default reflected Details is
  intentionally retained until Stage 4 replaces it with selected-point UI.
  Validation passed with 18/18 `SplineTests`, 47/47 `ViewportTests`, the
  all-scope plan validator, a full `all` build, and a two-tick hidden launch of
  the Debug DurinEditor against `Sandbox/Sandbox.dproject`.
- Stage 3 is complete. `FLevelEditorContext` now owns component and typed
  stable-ID sub-element selection; Details and viewport use that shared state
  and repair it when its actor, component, world, or document becomes invalid.
  Visualization primitives and hover hits preserve exact component/element
  identity. `FLevelViewportEditModeRegistry` stores only descriptors/factories,
  while each scene viewport owns an independent
  `FLevelViewportEditModeManager`; Select is the default mode and now owns Actor
  picking/manipulation after shared camera navigation.
- Stage 3 handoff: the pre-stage baseline is `405dd218`; the completion baseline
  is this stage commit. The working set is the Level Editor public selection,
  transform-target, customization, and viewport-mode contracts; context,
  Details tree, scene viewport, presentation, viewport client, and transform
  gizmo implementations; Viewport tests; and this plan. `FTransformGizmo`
  operates only on target sets, stable target identities, capability masks, and
  generic transactions; the Actor adapter preserves hierarchy filtering,
  package dirty restoration, snapping, cancel, Undo, and Redo behavior. The
  edit-mode selector remains distinct from render/raster and W/E/R controls.
  There are no open Stage 3 framework questions; Stage 4 can register Spline as
  the first contextual mode. Validation passed with 51/51 `ViewportTests`,
  18/18 `SplineTests`, the all-scope plan validator, a full `all` build, and a
  two-tick hidden launch of Debug DurinEditor against
  `Sandbox/Sandbox.dproject`.
- Stage 4 is complete. Spline is now the first contextual viewport edit mode:
  Details can activate it for the exact selected `DSplineComponent`, the
  visualizer emits adaptive curve, point, and manual-tangent primitives with
  typed stable-ID hits, and the mode owns point/tangent picking, Ctrl
  multi-selection, selected-point focus, and translation-only gizmo targets.
  Segment double-click performs linear or de Casteljau cubic splitting with a
  fresh point GUID. Details is selected-point driven and exposes transactional
  position/tangent fields plus append, delete, duplicate, reorder, loop,
  interpolation, and tangent-mode operations.
- Stage 4 handoff: the pre-stage baseline is `7e0efd3b`; the completion baseline
  is this stage commit. The working set is the Spline editor customization and
  module registration, shared multi-sub-element selection, scene viewport
  input/selection presentation, the Spline/Viewport test targets, and this
  plan. Spline point targets use component-local/world conversion and stable
  point-address identities while edits remain GUID-addressed. Structural edits
  use whole-authoring-data snapshot transactions; continuous point/tangent
  drags reuse the generic gizmo's one-transaction completion and Escape/target
  invalidation cancellation. There are no open Stage 4 questions. Validation
  passed with 54/54 `ViewportTests`, 18/18 `SplineTests`, a full `all` build,
  and a two-tick hidden launch of Debug DurinEditor against
  `Sandbox/Sandbox.dproject`.
- Stage 5 is complete. The lasting runtime contract now lives in
  `Runtime/World/SplineSystem.md`, viewport mode/selection/gizmo ownership lives
  in `Editor/Architecture/ViewportEditing.md`, shared proposal/transaction reuse
  is recorded in `ReflectedPropertyEditing.md`, and the user workflow lives in
  `SceneViewportNavigation.md`. Qualification found and closed one Stage 4 gap:
  Escape now cancels a drag, clears spline sub-selection, then requests a safe
  deferred return to Select on a later press; mode exit also cancels the live
  gizmo before releasing spline selection.
- Stage 5 handoff: the pre-stage baseline is `3452ca1b`; the completion baseline
  is this stage commit. The working set is the four lasting documents above,
  the viewport edit-mode public contract/manager, the Spline mode and its
  Viewport regression, and this plan. `ShouldExit()` deliberately defers a
  mode's self-requested switch until its `Tick()` returns, avoiding destruction
  of a live mode stack frame. There are no open Spline V2 foundation questions.
  Validation passed with 18/18 `SplineTests`, including closed-loop/seam cases;
  54/54 `ViewportTests`, including Spline point authoring and progressive
  Escape; the changed-document validator; a full `all` build from
  `Win64-Debug-DurinEditor-Tests`; and a two-tick hidden launch of Debug
  DurinEditor against `Sandbox/Sandbox.dproject`. Open/closed geometry and
  authoring behavior were exercised by the named direct suites, while the
  editor launch verified the same Profile's complete runtime load/exit path.
- Stage 0 is complete. Project owners confirmed there are no external spline
  assets and selected a direct schema break with no V1 conversion layer. A
  read-only asset audit of `Sandbox/Sandbox.dproject` found seven compatible
  packages and no persisted spline component; repository searches likewise
  found no serialized V1 spline data.
- The executable Stage 0 contract fixtures are
  `FSplineV2ContractTests` in `SplineV2ContractTests.cpp`. They lock linear and
  manual Hermite samples, chord-length automatic handles, clamped and
  degenerate behavior, manual aligned/broken coupling, seam policy, and the
  adaptive length defaults against a `2^18`-sample numeric reference.
- Stage 0 selects an absolute local-length error of `1e-4`, a relative error of
  `1e-5` times the segment Bezier control-polygon length, a maximum subdivision
  depth of `16`, and a degenerate-chord epsilon of `1e-9`. The recursive error
  budget is split equally between children.
- Regression baselines to preserve are the existing
  `FSplineCurveTests`, `FSplineComponentTests`, `FSplineReflectionTests`, and
  `FSplineEditingTests`; `FSplineComponentVisualizerTests`;
  `FTransformGizmoTests`; `FDetailsPanelTargetingTests`; and the Actor picking,
  selection, document revision, property transaction, and viewport lifecycle
  cases under `FLevelEditorViewportClientTests`, `FViewportSelectionTests`, and
  `FEditorTransactionManagerTests`.
- Stage 0 handoff: the pre-stage baseline is `89a209ac`; the completion baseline
  is this stage commit. Working set is this plan,
  `SplineV2ContractTests.cpp`, and the EngineTests source list. Key decisions
  are the clean break and numeric constants above. There are no open
  compatibility questions. Validation passed: the asset audit inspected seven
  compatible packages; `SplineTests` passed all 16 tests, including six V2
  contract cases; and the all-scope plan validator accepted 5 active, 6
  completed, and 50 archived plans. Stage 1 is next.

- The existing spline implementation is an editable, reflected
  `DSplineComponent` with cubic Hermite, linear, constant, and automatic
  tangent evaluation, a fixed-step local-distance table, Details editing, and
  draw-only viewport visualization.
- Reflection, object duplication, level-package persistence, shared property
  transactions, component-local/world-space conversion, and editor
  customization registration are established foundations and should be
  retained.
- Production code has no runtime spline consumer outside the Level Editor.
  Repository content and sandbox roots contained no serialized
  `DSplineComponent`, `SplineCurve`, or `SplineComponent` reference at plan
  authoring time. This plan therefore selects a clean V2 schema replacement;
  external-project compatibility is a Stage 0 confirmation gate rather than a
  default requirement.
- Viewport visualization hit results identify only an actor and component, the
  transform gizmo directly snapshots selected actors, and component selection
  is private to the Details panel. Point-level selection and manipulation
  cannot be added cleanly without first establishing shared component/sub-
  element selection and reusable viewport edit modes.
- The runtime and component now use only the V2 schema; viewport interaction
  still has actor/component-only hit identity and no edit-mode framework.

## Goal

Deliver a coherent Spline V2 authoring foundation with:

- an unambiguous geometry-only spline schema;
- deterministic cubic and linear evaluation with bounded-error local arc
  length, bounds, and nearest-point queries;
- immutable evaluation snapshots safe for concurrent reads;
- reflected component persistence and explicit mutation/rebuild semantics;
- a reusable Level Editor viewport edit-mode framework with component and
  stable sub-element selection;
- a target-driven transform gizmo that preserves current Actor behavior; and
- an interactive Spline mode for point and tangent selection, insertion,
  movement, deletion, Details editing, Undo/Redo, and cancellation.

The completed result is the common geometry and authoring layer on which a
later spline-mesh, path-following, placement, animation, or gameplay feature can
depend without changing the core spline contract.

## Scope

- Replace `ESplinePointType` with independent outgoing-segment interpolation
  and point tangent-mode concepts.
- Give every authored point a component-local stable `FGuid` identity.
- Remove discontinuous constant position segments from the spatial-curve
  model.
- Remove point rotation, point scale, and transform sampling from the geometry
  core; retain only position and differential geometry.
- Replace the reflected fixed-step quality setting and mutable lazy cache with
  internal adaptive build settings and immutable evaluation data.
- Preserve open/closed curves, local authoring space, component world-space
  location/vector conversion, reflection, duplication, package persistence,
  and shared property transactions.
- Add structured parameter/sample types, local-distance conversion, local
  bounds, and nearest-point queries.
- Add shared selected-component and sub-element state to the Level Editor
  workspace context.
- Add registered viewport edit modes, mode lifecycle/input arbitration, typed
  visualization elements, and target-driven gizmo manipulation.
- Implement the Select and Spline modes and expose mode switching in the scene
  viewport toolbar.
- Replace the all-points-expanded Spline Details presentation with component
  settings plus the currently selected point or points.
- Update runtime/editor contract documentation and the user-facing viewport
  guide after behavior is implemented.

## Non-Goals

- Spline-mesh deformation, a spline vertex factory, road/cable/fence
  generation, or runtime spline rendering.
- Path followers, actor placement, animation tracks, navigation, physics, or
  gameplay events.
- A generic arbitrary-property metadata channel on spline points.
- Full quaternion orientation, roll, width, cross-section scale, banking, or
  parallel-transport frame generation. The first production consumer must
  select those semantics in a separate plan.
- True world-space arc-length traversal under non-uniform component scale. V2
  distance is explicitly spline-local; returned samples may still be converted
  to world space.
- Simultaneous editing of elements from multiple spline components.
- Rotation or scale gizmo operations for spline point selections. V2 Spline
  mode requires translation of points and tangent handles; gizmo targets expose
  a capability mask so later modes can add other operations deliberately.
- Long-lived V1/V2 dual-schema support or silent lossy migration.
- Refactoring unrelated Level Editor selection, toolbar, rendering, or Details
  behavior.

## Design Decisions and Invariants

### Geometry schema

- `FSplinePoint` remains the reflected authored point type and contains:
  `FGuid Id`, `FVector3 Position`, `FVector3 ArriveTangent`,
  `FVector3 LeaveTangent`, `ESplineSegmentInterpolation OutgoingInterpolation`,
  and `ESplineTangentMode TangentMode`.
- `ESplineSegmentInterpolation` contains `Linear` and `Cubic`. The value belongs
  to the segment leaving a point. A one-point or empty curve has no segment.
- `ESplineTangentMode` contains `Automatic`, `AutomaticClamped`,
  `ManualAligned`, and `ManualBroken`.
- Automatic tangents use chord-length-aware cubic interpolation. Automatic
  clamping must not reverse a tangent against either incident chord and must
  bound its magnitude by adjacent chord lengths. For non-degenerate incident
  chords of lengths `h0` and `h1`, the knot derivative is
  `(h1 * normalize(P-Pprev) + h0 * normalize(Pnext-P)) / (h0+h1)`;
  arrive and leave handles multiply its direction and magnitude by `h0` and
  `h1` respectively. Clamped mode zeros both handles if this derivative has a
  non-positive dot product with either incident chord direction, then caps each
  handle at its incident chord length. A single degenerate incident chord
  yields a zero handle on that side and the other chord on the valid side; two
  degenerate chords yield zero handles. Open endpoints use their sole incident
  chord as the segment-local tangent. Closed curves obtain both neighbors by
  wrapping across the seam.
- Manual-aligned edits keep arrive and leave directions collinear while
  retaining independently authored magnitudes. The two stored derivative
  vectors point in the same path direction; the arrive visualization handle is
  drawn at `Position - ArriveTangent / 3`. Editing either aligned handle applies
  its direction to both vectors without changing the opposite magnitude.
  Manual-broken edits are fully independent.
- Point IDs are unique only within their owning curve. Component duplication
  may preserve the duplicated curve's IDs; point insertion, append, and point
  duplication always create a new ID.
- Closed-loop segments are identified by the stable ID of their starting
  point. Closing the loop does not create a synthetic serialized point.
- Point rotation and scale are not geometry. `GetRotation*`, `GetScale*`, and
  `GetTransform*` leave the spline API instead of returning a transform that is
  unrelated to the path tangent.

### Parameters, distance, and evaluation

- `FSplineParameter` contains `SegmentIndex` and a segment-local `T` in
  `[0, 1]`. Public APIs do not expose the old segment-index-plus-fraction naked
  `double` as the primary parameter type.
- `FSplineSample` contains position, first derivative, second derivative, and
  normalized direction. Degenerate derivatives produce a zero direction.
- Open curves clamp parameters and local distances. Closed curves wrap values;
  the explicit end parameter maps to the seam while an end-distance query may
  still report the full loop length.
- Spatial `Constant` interpolation is removed. Empty and one-point curves keep
  deterministic neutral/point results and zero length.
- Arc length is always measured in authored local space and APIs include
  `LocalDistance` or `LocalLength` in names where ambiguity would otherwise be
  possible. Transforming a returned sample to world space does not redefine
  its distance domain.
- `FSplineCurve` owns only reflected authoring data. `FSplineEvaluationData` is
  an immutable, non-reflected snapshot containing cubic coefficients,
  per-segment adaptive distance samples, cumulative segment lengths, and local
  bounds.
- `DSplineComponent` rebuilds and publishes an evaluation snapshot after a
  successful mutation and after reflected load/edit repair. Runtime queries
  read a captured `shared_ptr<const FSplineEvaluationData>` and never mutate
  caches through `const_cast`.
- Snapshot construction occurs on the owning game/editor thread. Once
  published, one snapshot supports concurrent read-only queries. A revision and
  `Topology`, `Geometry`, and `Build` change flags let later consumers identify
  invalidation without observing partially rebuilt data.
- Adaptive subdivision uses geometric error and maximum depth rather than a
  reflected samples-per-segment knob. Runtime distance error tolerance and
  viewport screen-space tessellation are separate concerns. The runtime defaults
  are `1e-4` absolute local units, `1e-5` relative to the cubic Bezier control-
  polygon length, and depth `16`; each recursion receives half its parent's
  error budget and accepts an interval when its midpoint split-chord excess is
  within that budget.
- Nearest-point lookup uses the immutable segment data for coarse rejection and
  a safeguarded local refinement. Bounds are conservative for every cubic and
  include analytic extrema or the equivalent Bezier control-hull bound.

### Viewport edit-mode ownership

- `FSceneViewportPanel` owns one `FLevelViewportEditModeManager` per Level Editor
  workspace. The global customization registry owns only immutable descriptors
  and factories; it never owns per-document or per-viewport mode state.
- `FLevelEditorContext` owns shared transient component and sub-element
  selection so Details and the viewport observe the same target. Component
  selection must belong to the primary selected actor. Sub-element selection
  must belong to the selected component. Repairing or clearing a parent
  selection clears invalid descendants.
- The Details component tree becomes a view/controller of the shared component
  selection rather than retaining a private authoritative selection.
- The customization registry gains registered viewport edit-mode descriptors
  with a stable `FName` ID, label/icon metadata, activation predicate, and
  per-manager factory. `Select` is always available; `Spline` is contextual to
  a selected `DSplineComponent`.
- Mode activation is explicit through the viewport mode selector or an
  `Edit Spline` Details action. A normal visualization click in Select mode
  continues to select an actor/component and does not silently change modes.
- Mode priority is camera navigation, active-mode interaction (including its
  gizmo target), then Select-mode actor/component picking. W/E/R select gizmo
  operations inside the active mode; they are not edit-mode identifiers.
- Mode exit cancels any active drag before releasing selection. Level/document
  change, PIE/read-only entry, target deletion, registry unregistration, and
  editor shutdown force the same exit path.
- Escape first cancels an active edit, then clears spline sub-selection, then
  exits Spline mode on a subsequent press.
- Active edit mode is transient and is not serialized in level content. The
  editor may remember the last toolbar preference, but every new document and
  PIE transition starts safely in Select mode.

### Hit testing and gizmo targets

- Visualization primitives and hits gain an optional typed element handle. A
  handle contains an element kind and stable point ID; spline kinds are
  `Point`, `ArriveTangent`, `LeaveTangent`, and `Segment`.
- Hit testing preserves the exact actor, component, and element instead of
  reducing a visualization hit to an actor before mode dispatch.
- Component visualizers remain stateless draw producers. Active-mode state,
  hover, selection, transactions, and manipulation live in the per-workspace
  mode instance.
- `FTransformGizmo` consumes an `ITransformGizmoTarget`/target-set interface
  that supplies pivot, basis, supported operations, snapshots, delta
  application, commit, and cancel behavior.
- The existing Actor transform behavior moves behind an Actor target adapter
  before Spline uses the interface. Actor multi-selection, parent filtering,
  package dirty restoration, snapping, Undo/Redo, and cancellation remain
  behaviorally unchanged.
- Spline point and tangent targets convert world-space translation deltas into
  the component's local space and submit changes through the shared reflected
  property transaction path. One completed drag produces one transaction; a
  cancelled or net-zero drag produces none and restores prior package dirty
  state.

### Spline authoring interaction

- Spline mode edits exactly one `DSplineComponent`; Ctrl-click toggles stable
  point selection within that component.
- Point translation supports one or multiple selected points. A tangent handle
  is edited alone and updates the corresponding manual tangent; automatic
  tangents have no draggable handle.
- Double-clicking a segment inserts a point at the hit parameter. Linear
  segments split linearly. Cubic segments use an exact Hermite/Bezier split so
  the curve shape is unchanged immediately after insertion.
- Delete removes selected points in one structural transaction. Append,
  insertion, duplication, and reordering create or preserve IDs according to
  the geometry-schema rules.
- Switching tangent modes establishes deterministic values: entering a manual
  mode seeds handles from the currently evaluated automatic tangent; entering
  an automatic mode retains dormant manual values but evaluation ignores them.
- Details shows component settings and the active point selection. Single
  selection exposes all point fields; multi-selection exposes only common
  editable fields and mixed-value state. The unbounded all-points-expanded form
  is removed.
- Viewport curve drawing uses view-dependent or geometric tessellation and is
  independent of runtime arc-length build settings.

### Stage and handoff discipline

- Each completed stage records its baseline commit, working set, key symbols,
  decisions, open questions, and validation result in `Current Status` before
  the next stage begins.
- A stage does not start by rediscovering completed-stage architecture; it
  validates the recorded symbols and diff first.
- Any change to the selected schema, distance domain, ownership, or mode
  lifecycle is recorded with rationale in this plan before dependent code is
  implemented.

## Current Foundations and Gaps

| Area | Foundation to retain | Gap closed by this plan |
| --- | --- | --- |
| Curve mathematics | Cubic Hermite position/derivative evaluation, linear segments, open/closed parameter handling | Split segment interpolation from tangent mode; remove discontinuous constant geometry; add structured samples and robust automatic tangents |
| Distance | Local distance queries and binary lookup concept | Remove user-authored fixed steps; build bounded-error immutable per-segment data |
| Component | `DSceneComponent` ownership, local/world conversion, dirty marking, reflected edit/load repair | Publish immutable snapshots, explicit revision/change flags, and unambiguous local-distance API |
| Persistence | Reflected nested structs/arrays, duplication, package round trips | Stable point IDs and V2 schema coverage |
| Transactions | Shared reflected property proposals, continuous edit coalescing, Undo/Redo/Cancel | Reuse from viewport gizmo targets and structural point operations |
| Visualization | Registered component visualizers, shared line/icon collection and screen-space hit testing | Preserve component and typed element identity; separate passive drawing from active tools |
| Selection | Workspace-level Actor selection | Shared component/sub-element selection with repair rules |
| Gizmo | Native translation/rotation/scale handles, snapping, Actor transaction/cancel behavior | Target-driven manipulation with Actor parity and Spline translation targets |
| Spline UI | Details point values, add/remove/reorder, passive curve/point/tangent drawing | Contextual mode switching, direct point/tangent interaction, exact segment insertion, selected-point Details |
| Consumers | None in production | Explicitly deferred until the foundation is validated |

## Implementation Stages

### Stage 0: Freeze V2 contracts and compatibility boundary

Outcome: Numeric, schema, lifecycle, and compatibility decisions are recorded
as executable fixtures before public types change.

- [x] Reconfirm repository and configured project content contains no persisted
  V1 spline component; record any external compatibility requirement supplied
  by project owners.
- [x] Confirm clean V2 replacement. If external V1 data must survive, stop and
  add an explicit, testable conversion stage before modifying reflected fields;
  do not silently map Constant, Rotation, or Scale.
- [x] Add golden geometry cases for linear, manual cubic, chord-length automatic,
  automatic-clamped, aligned, broken, open endpoints, closed seam, duplicate
  positions, zero tangents, and one-point/empty curves.
- [x] Select default adaptive build tolerances and maximum subdivision depth by
  comparing the golden curves against a high-resolution numeric reference.
- [x] Record exact automatic-clamped direction and magnitude rules in the
  runtime contract draft and numeric fixtures.
- [x] Capture current Actor selection, gizmo, reflected editing, duplication,
  package round-trip, and Spline visualization tests as named regression
  baselines.
- [x] Record the Stage 0 handoff and baseline commit in `Current Status`.

Dependencies: None.

#### Acceptance Gate

- V2 schema and all removal decisions are unambiguous.
- Automatic tangent/clamping and adaptive error expectations have numeric
  fixtures, not prose-only descriptions.
- The compatibility decision is evidence-backed and no unknown serialized
  input is knowingly discarded.
- Existing editor behavior that must survive has an identified regression test.

### Stage 1: Implement geometry authoring and immutable evaluation

Outcome: The runtime has a standalone, tested V2 curve/evaluation layer with no
editor dependency.

- [x] Replace the point/interpolation schema and establish stable point-ID
  creation, validation, insertion, duplication, removal, and reorder helpers.
- [x] Implement `FSplineParameter`, `FSplineSample`, cubic coefficients, linear
  and cubic evaluation, first/second derivatives, and all tangent modes.
- [x] Implement immutable per-segment evaluation data, adaptive local arc-length
  tables, cumulative lengths, conservative local bounds, and safeguarded
  distance-to-parameter inversion.
- [x] Implement nearest-parameter/location queries with coarse segment rejection
  and bounded local refinement.
- [x] Remove the reflected reparameterization-step field and the mutable lazy
  cache/`const_cast` path.
- [x] Remove Constant geometry and rotation/scale/transform sampling APIs rather
  than retaining deprecated ambiguous behavior.
- [x] Add unit coverage for every Stage 0 fixture, adaptive error bounds,
  monotonic distance conversion, seam behavior, degenerates, ID invariants,
  nearest queries, and snapshot concurrent reads.
- [x] Record the Stage 1 handoff and baseline commit in `Current Status`.

Dependencies: Stage 0.

#### Acceptance Gate

- Runtime Spline tests pass without linking Level Editor code.
- Curved-segment local-length error remains within the selected tolerance for
  every golden and stress fixture.
- Published evaluation data is immutable and concurrent read tests do not
  mutate shared state.
- No production declaration or implementation references the removed V1
  interpolation, quality, rotation, scale, or transform APIs.

### Stage 2: Integrate the V2 component, reflection, and persistence

Outcome: `DSplineComponent` owns V2 authored data, publishes evaluation
snapshots, and preserves current object/property lifecycle guarantees.

- [x] Adapt component mutation/query APIs to V2 parameter, sample, and explicit
  local-distance semantics.
- [x] Rebuild/publish snapshots after setters, reflected edits, duplication,
  PostLoad, Undo, and Redo; expose revision and change flags without adding a
  consumer dependency.
- [x] Preserve local/world conversion for positions, derivatives, and directions
  while documenting that distance remains local under component scale.
- [x] Register and serialize point GUIDs and new enums; validate duplicate,
  package save/load, and reflected nested array editing.
- [x] Reject or repair duplicate/invalid point IDs deterministically at the
  mutation/load boundary and report non-recoverable data through existing error
  paths.
- [x] Port the existing transactional tests to V2 and add component revision,
  snapshot publication, dirty-state, and edit-repair coverage.
- [x] Keep a temporary passive V2 visualizer only if needed for intermediate
  editor buildability; do not implement point interaction in this stage.
- [x] Record the Stage 2 handoff and baseline commit in `Current Status`.

Dependencies: Stage 1.

#### Acceptance Gate

- Reflection, object-graph duplication, package round trips, setters, property
  edits, Undo, and Redo all produce valid V2 snapshots and point identities.
- Local/world sample conversion and local-distance behavior are covered under
  translation, rotation, uniform scale, and non-uniform scale.
- Component revisions change exactly when published evaluation behavior changes.
- A Level Editor build can inspect and passively visualize a V2 component
  without V1 compatibility fields.

### Stage 3: Establish reusable viewport edit modes and gizmo targets

Outcome: Select behavior runs through a generic mode/target framework with no
Actor regression, and Level Editor modules can register contextual modes.

- [x] Promote selected-component state into `FLevelEditorContext`, adapt the
  Details component tree, and add generic typed sub-element selection with
  parent/target repair rules.
- [x] Extend visualization primitives, hover state, and hit results with exact
  component and optional element identity.
- [x] Add viewport edit-mode descriptors/factories to the customization registry
  and implement a per-viewport/workspace mode manager with the selected
  activation, exit, cancellation, and invalidation lifecycle.
- [x] Move current Actor picking and manipulation behind the Select mode while
  keeping camera navigation in the shared viewport path.
- [x] Refactor `FTransformGizmo` around target sets and a supported-operation
  capability mask; implement the Actor adapter first.
- [x] Add a distinct viewport edit-mode selector without conflating it with
  Lit/Unlit, Solid/Wireframe, or W/E/R gizmo controls.
- [x] Add test-only edit modes/targets to verify registration, activation
  predicates, input priority, hover identity, mode switching, forced exit,
  cancellation, and unregistration.
- [x] Re-run Actor multi-selection, snapping, parent filtering, transaction,
  net-zero, Escape, package dirty, picking, toolbar, and document/PIE lifecycle
  tests through the new framework.
- [x] Record the Stage 3 handoff and baseline commit in `Current Status`.

Dependencies: Stage 0. Stage 2 must be complete before Spline registers against
the framework, but the generic framework itself must not depend on Spline.

#### Acceptance Gate

- Select mode is behaviorally equivalent to the previous Actor selection and
  transform path for all recorded baselines.
- Component selection is shared between Details and viewport and repairs safely
  after actor/component deletion or document change.
- Registered mode state is per workspace, never stored in singleton visualizers,
  and cannot leak across documents or PIE.
- A target-driven gizmo completes, cancels, and records transactions without
  knowing whether the target is an Actor.

### Stage 4: Deliver interactive Spline authoring mode

Outcome: Users can explicitly enter Spline mode and author points/tangents
directly in the scene viewport with stable selection and correct transactions.

- [x] Register the contextual Spline mode and add `Edit Spline` activation from
  the selected component's Details presentation.
- [x] Draw adaptive curve segments, point markers, selected/hovered states, and
  manual tangent handles with typed hit elements and screen-stable tolerances.
- [x] Implement single/Ctrl multi-point selection by point GUID, blank-click
  clearing, focus-selection behavior, and exact component targeting when an
  actor owns multiple splines.
- [x] Implement translated point target sets and single manual-tangent targets,
  including world/local delta conversion and capability reporting.
- [x] Implement segment double-click insertion with shape-preserving linear or
  cubic splitting and a new stable point ID.
- [x] Implement append, delete, duplicate, reorder, loop, interpolation, and
  tangent-mode operations through shared property transactions.
- [x] Replace the expanded point list with selected-point Details, mixed-value
  multi-selection fields, and deterministic mode-transition seeding.
- [x] Ensure one drag/structural action creates one useful transaction, Undo/Redo
  restores evaluation snapshots and selection where valid, and Cancel restores
  values plus package dirty state.
- [x] Add viewport hit, overlapping-priority, selection, insertion shape,
  tangent, transaction, read-only, target deletion, multi-component, and mode
  exit tests.
- [x] Record the Stage 4 handoff and baseline commit in `Current Status`.

Dependencies: Stages 2 and 3.

#### Acceptance Gate

- Point reorder, insertion, and Undo/Redo do not move selection to a different
  logical point.
- Cubic insertion preserves sampled shape within the Stage 0 numeric tolerance.
- A completed drag creates one transaction; cancelled and net-zero drags create
  none and restore prior dirty state.
- Spline mode never edits a non-target component and exits safely when its
  actor/component/document becomes invalid or read-only.
- Select mode remains the default and behaves unchanged after leaving Spline
  mode.

### Stage 5: Qualify the foundation and publish lasting contracts

Outcome: Runtime and editor contracts are documented, end-to-end behavior is
verified, and the repository is ready for a separately planned first consumer.

- [x] Update the runtime Spline contract to replace V1 behavior and describe
  schema, evaluation, distance, snapshot, mutation, and supported-query rules.
- [x] Add an Editor Architecture contract for viewport edit-mode ownership,
  registration, selection, input priority, gizmo targets, and lifecycle.
- [x] Update the scene viewport guide with mode switching, Spline selection,
  point/tangent manipulation, shortcuts, cancellation, and read-only behavior.
- [x] Update reflected-property editing documentation where viewport targets
  reuse shared proposals/transactions.
- [x] Run the focused runtime, persistence, editor transaction, viewport,
  toolbar, document/PIE, and Spline authoring suites from cold direct targets.
- [x] Follow the repository build/run contract and complete a successful full
  `all` build because the plan changes user-visible editor behavior.
- [x] Launch the editor from the same Agent Build Profile, exercise one open and
  one closed spline end to end, and record the verified editor executable.
- [x] Confirm no V1 spline symbols, fields, Details controls, documentation, or
  test assumptions remain outside intentionally retained history.
- [x] Record the Stage 5 handoff and validation evidence, move lasting rules to
  their owning documents, and mark this plan completed only after every gate
  passes.

Dependencies: Stages 1-4.

#### Acceptance Gate

- Focused unit, persistence, transaction, viewport, and end-to-end editor tests
  pass.
- A full `all` build succeeds and the verified editor executable launches.
- Runtime, editor architecture, and user-facing guides describe the implemented
  behavior without relying on this plan as the lasting contract.
- The V2 foundation exposes enough stable geometry, invalidation, and authoring
  behavior for a first-consumer plan without reopening core schema decisions.

## Validation Matrix

| Area | Validation | Required result |
| --- | --- | --- |
| Segment evaluation | Numeric unit tests for linear and every cubic tangent mode | Position and first/second derivatives match golden values at endpoints and interior samples |
| Degenerates | Empty, one-point, duplicate positions, zero tangents, zero-length segments | Finite deterministic samples, zero direction when undefined, no invalid lookup state |
| Closed seams | Parameters and distances before, at, and after the seam | Documented clamp/wrap behavior and full loop length without synthetic points |
| Adaptive length | Golden/stress curves against high-resolution reference | Error stays inside the Stage 0 bound and distance lookup remains monotonic |
| Nearest/bounds | Analytic/simple shapes plus randomized dense references | Conservative bounds and nearest results inside selected tolerance |
| Snapshot reads | Concurrent repeated queries against one published snapshot | No mutation, race-visible partial build, or result drift |
| Point identity | Add/insert/duplicate/reorder/delete, duplication, save/load | IDs follow component-local uniqueness and preservation rules |
| Component lifecycle | Setter, reflected edit, PostLoad, duplication, Undo/Redo | One valid published snapshot and correct revision/change flags per semantic change |
| Coordinate behavior | Component translation, rotation, uniform/non-uniform scale | World vectors/positions are correct and local-distance semantics remain explicit |
| Select-mode regression | Actor pick, multi-select, parent filter, W/E/R, snapping, focus | Existing behavior and transactions remain equivalent after mode extraction |
| Mode lifecycle | Activate, switch, Escape, delete target, document switch, PIE, read-only, unregister | Active work cancels safely and state does not leak |
| Visualization hits | Curve, point, tangent, overlapping depth/priority cases | Exact actor/component/element identity reaches the active mode |
| Spline transactions | Continuous point/tangent drag, structural edit, net-zero, Cancel, Undo/Redo | One logical history entry or none, with values/cache/dirty state restored correctly |
| Cubic insertion | Dense before/after sampling around inserted parameter | Shape preserved within the Stage 0 tolerance |
| Details integration | Component and single/multi-point selection | Viewport and Details show/edit the same stable target and mixed values correctly |
| Full qualification | Focused cold targets, full `all` build, editor launch/manual workflow | All pass from one Agent Build Profile |

## Definition of Done

- V2 geometry, evaluation snapshots, component APIs, reflection, persistence,
  and tests satisfy Stages 0-2 without V1 ambiguity.
- Select mode, shared component/sub-element selection, typed hits, registered
  edit modes, and target-driven gizmos satisfy Stage 3 with no Actor regression.
- Spline mode provides the Stage 4 interaction set with stable identity,
  correct hit targeting, transactional edits, Undo/Redo, and Cancel.
- No runtime or editor API exposes Constant spatial interpolation, reflected
  reparameterization steps, point rotation/scale, or tangent-independent spline
  transforms.
- Runtime/editor/user documentation owns every lasting contract.
- Focused validation and a full `all` build pass, and the verified editor
  executable completes the manual Spline workflow.
- A separate first-consumer plan can depend on the finished V2 contract without
  adding behavior to this completed plan.

## Deferred Follow-ups

- Select and plan the first production consumer: spline mesh, follower,
  placement, animation, or another concrete feature.
- Add orientation/frame policy only when that consumer selects between authored
  roll, fixed-up, Frenet-like, parallel-transport, or another explicit model.
- Add width/scale/custom attributes only with consumer-owned semantics.
- Add true world-distance evaluation and transform-revision caching if a
  non-uniformly scaled consumer requires constant world-space speed.
- Add multi-component spline editing, point rotation/scale operations, marquee
  selection, snapping to scene geometry, and segment/tangent numeric overlays
  after the single-component workflow is proven.
- Generalize viewport modes for Mesh Paint, Landscape, Volume/Brush, or other
  tools through separate bounded plans.

## Related Documentation

- [Spline System](../Runtime/World/SplineSystem.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Viewport Editing Architecture](../Editor/Architecture/ViewportEditing.md)
- [Scene Viewport Navigation](../Editor/Guides/SceneViewportNavigation.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Spline/SplineTypes.h`
- `Engine/Source/Runtime/Engine/Public/Spline/SplineCurve.h`
- `Engine/Source/Runtime/Engine/Private/Spline/SplineCurve.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/SplineComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/SplineComponent.cpp`
- `Engine/Source/Editor/LevelEditor/Public/LevelEditorCustomizations.h`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/SplineEditorCustomizations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/TransformGizmo.h`
- `Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorContext.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsComponentTree.h`
- `Engine/Tests/Native/EngineTests/Private/SplineTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportCustomizationTests.cpp`
