# Road Surface Foundation Plan

Summary: Validate RoadWeaver graph semantics and generate asset-driven road previews on analytic planar and spherical surfaces using existing spline foundations.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

Selected implementation path for P0 of
[Road System Evolution](../Roadmaps/RoadSystemEvolution.md). Graph validation,
surface alignment snapshots and scene previews are implemented; stage acceptance
remains pending. RoadWeaver contract and integration tests pass, and plane/sphere
previews have been observed in the editor. Picking, complete transaction replay,
scene lifecycle qualification and documentation handoff remain outstanding.
Surface descriptors now use active-parameter comparison rather than stored GUIDs
or revision counters; tests cover inactive edits preserving snapshots and small
active edits rebuilding them. Existing Spline/SplineMesh coverage does not qualify
the remaining road integration gates.

Checkpoint validation (Win64-Debug-DurinEditor, 2026-09-07):
`DevTool.bat test affected` passed (including RoadWeaver contract and integration
targets); `DevTool.bat build` completed target `all`; changed-document validation
passed. The checkpoint commits source, tests and this plan. Editor-authored
content changes remain outside the code checkpoint pending scene qualification.

## Goal

Place one `DRoadNet` asset in a scene and preview its constant-width roads on
an analytic plane or sphere through the same road generation path. Reject
invalid authored topology before publication, preserve asset identity across
save/load, and expose deterministic distance and frame queries. This phase
does not generate junction surfaces or modify production terrain.

## Selected Decisions

- Preserve `FNode`, `FRoad`, `FLaneSection`, `FJunction`, and stable GUIDs.
  Extend their contracts where required; do not replace the graph wholesale.
  Keep `FRoadNetBuilder` as a value construction helper, not a mesh builder.
- Road coordinates are double-precision meters in network-local space.
  Initial instances reject scale other than identity and support rigid
  placement. Sphere center/radius and plane parameters have an explicit
  conversion into that same space.
- A road's final evaluated three-dimensional arc length owns lane stationing.
  Surface projection/fitting precedes this distance table. Do not reuse the
  unconstrained source spline's distance table for a changed evaluated path.
- Node positions own graph endpoints; route edits must synchronize incident
  curve endpoints within one candidate transaction. Validation rejects
  disagreement rather than silently moving either side. P0 permits only open
  road reference curves; closed routes use explicit graph segments.
- Legal lane sections cover the road's complete station interval without gaps
  or overlap under a documented tolerance. Adjacent sections need explicit
  directed continuity mappings where multiple sections exist. Graph endpoints
  select terminal sections according to endpoint and lane direction.
- Separate authored validation from surface-dependent build validation. Raw
  curve finiteness, IDs, references, and interval ordering are checked before
  asset replacement; final-length coverage and surface-fit validity are checked
  before derived publication. Invalid derived geometry is never marked ready.
- Reuse immutable spline evaluation, differential samples, and frame math.
  Road-specific evaluation supplies surface constraints and per-sample reference
  up, then continuous road orientation. A seed-up-only transport frame is not
  sufficient to enforce spherical surface alignment.
- Use one road-owned scene adapter referencing the asset. Generated components
  are transient; they are not a second editable copy of its reference lines.
  P0 generation is synchronous on the owning thread, with immutable query
  snapshots. No background workers or streaming are introduced in this phase.
- On failure, reject authored mutation atomically. A failed derived rebuild
  retains any last valid result explicitly marked stale with its revision and
  diagnostic; with no prior result it publishes no geometry. Never report the
  previous result as current. Asset unload/detachment releases preview state.

## Scope

Include constant-width bidirectional road previews, terminal lane queries,
analytic surface providers, explicit diagnostics, asset/scene lifecycle, and
qualified extension seams for later cross-section builders. Junction graph
validation is included; junction meshes, viewport point manipulation, variable
width, terrain deformation, bridges, navigation, asynchronous builds, and LOD
policy changes are excluded. Existing source-mesh LOD behavior may be reused.

## Stage 0 Contract Decisions

- Store a separate instance surface descriptor: unconstrained (legacy default),
  plane (origin and normal), or sphere (center and radius), all in network-local
  meters. Evaluate the source spline first, then orthogonally project onto the
  plane or radially project onto the sphere. Elevation is a constant signed
  normal offset; sphere effective radius is radius plus elevation. Nodes remain
  the source-space endpoint authority. A rigid instance transform maps both
  provider and road into world space; any scale unequal to one is unsupported.
- Bound position/control/tangent components and provider centers to +/-1e7 m;
  sphere effective radius is [1, 1e7] m. Endpoint and section adjacency tolerance
  is 1e-4 m. Require effective segment length greater than 1e-6 m. Raw non-finite
  tangents are invalid even in automatic mode. Open curves only; invalid point
  IDs and interpolation/tangent enums are rejected at the road boundary.
- Fit projected curves with bounded adaptive Hermite intervals. Position and
  radial error budget is 1e-3 m; arc-length budget is 1e-4 m plus 1e-6 times
  length; per-interval recursion depth is at most 16 and total intervals at most
  65536 per road. Check quarter, midpoint, and three-quarter samples and reject
  exhausted budgets. Reject projection through the sphere center, zero projected
  derivatives, tangent/reference-up degeneracy, and adjacent frame direction/up
  sign reversals. Antipodal endpoints require explicit intermediate controls
  that avoid the center; no arbitrary great-circle choice is made.
- Use projected per-sample normal and differential tangent to construct
  right-handed forward/side/up frames. Orthonormal residual must be <=1e-8;
  preview frame angular error must be <=1e-3 radians. The final fitted curve owns
  the distance table; source spline station tables never station a projected road.
- Preserve explicit section stations after edits. Authored validation requires
  nonempty contiguous sections starting at zero; derived validation requires
  final coverage within max(1e-4 m, 1e-6 times final length). A route/node edit
  submits one complete candidate through SetDefinition or the reflected root.
  No implicit endpoint synchronization or station repair occurs.
- Schema 2 adds directed section transitions; schema 1 defaults to an empty
  transition list and unconstrained placement. Valid single-section assets keep
  every GUID. Older invalid graphs fail admission with the entity diagnostic;
  repair via a complete candidate with synchronized endpoints, explicit stations,
  and lane mappings, then save schema 2. Never auto-generate replacement graph IDs.
  The checked-in RoadWeaver content currently contains only L_RoadNet, a level
  referencing Engine SplineBox; no checked-in DRoadNet corpus needs conversion.
- Register project tests under RoadWeaver/Tests/Native using add_durin_test and
  durin_register_native_test, linking RoadWeaver. Use an in-memory rectangular
  strip for CPU deformation tests and /Engine/Models/SplineBox.SplineBox for
  editor visual qualification. Native construction uses keyed transient
  DSplineMeshComponent values. Existing construction rollback retires new
  candidates but does not restore mutated reused components: preflight all
  geometry/resources before reuse and explicitly qualify rollback before
  claiming atomic preview publication.
- Representative fixtures: 100 m flat and sloped roads; a manual cubic; sphere
  radius 1000 m with 90-degree, seam-crossing, polar and >180-degree routes using
  intermediate controls; rigid translation (123,-47,9) and 37-degree rotation;
  radius/coordinate limits, antipodal center crossing, zero derivatives and
  subdivision exhaustion. These are acceptance inputs, not validation evidence.

## Implementation Stages

### Stage 0: Freeze spatial and compatibility contracts

- [x] Inspect applicable Engine math/scene, asset lifecycle, transaction, and
  native-test ownership guidance before selecting implementation APIs.
- [x] Define whether constrained surface authoring stores projected controls
  or a separate surface-alignment descriptor; select one representation with
  deterministic plane/sphere evaluation and a documented elevation convention.
  Preserve existing Cartesian roads through an explicit unconstrained mode.
- [ ] Specify endpoint/section tolerances, sphere fitting error and maximum
  subdivision, frame continuity checks, degenerate/antipodal input behavior,
  and finite supported coordinate/radius limits. Record numeric thresholds and
  representative fixtures before implementing the algorithms.
- [x] Define station reconciliation after length-changing edits: preserve
  explicit authored stations or remap through stable anchors, with a selected
  rule and atomic failure behavior. Avoid silent clamping of section boundaries.
- [ ] Inspect current assets and choose versioned migration/defaults for new
  fields and stricter invariants; specify an actionable repair path for older
  assets rejected by the stronger contract. Preserve all existing valid IDs.
- [ ] Select project-owned native-test registration and preview mesh fixture;
  confirm supported generated-component and resource publication APIs.

Completion: these decisions are recorded here, with no unresolved representation,
distance, migration, or error-threshold choice left to later stages. No stage is
complete merely because a preliminary decision is written down.

### Stage 1: Enforce graph and mutation invariants

Depends on Stage 0.

- [ ] Add lookup ownership for lane -> section -> road and node incidence;
  validate incoming/outgoing terminal lanes against the junction node and
  traffic direction, including connector endpoints where authored.
- [ ] Validate curve point/tangent finiteness, supported interpolation values,
  effective nonzero geometry, endpoint agreement, and section invariants;
  validate explicit section-to-section lane mappings and enum values.
- [ ] Route setters, reflected editing, load, and Undo/Redo through consistent
  validation and revision publication. Failed candidates leave authored state,
  package dirty state, and published revision unchanged.
- [ ] Add schema compatibility handling selected in Stage 0 and diagnostics
  identifying the offending road/lane/section/connection.
- [ ] Qualify disconnected junction references, reversed flow, mismatched
  endpoints, NaN/infinite tangents, duplicate IDs, invalid sections, and valid
  multi-section transitions; verify save/load and rejected-mutation atomicity.

Completion: invalid fixtures fail with specific diagnostics, valid fixtures
round-trip, and ownership queries select the correct endpoint lanes.

### Stage 2: Build surface-aware alignment snapshots

Depends on Stage 1.

- [x] Add a value/query boundary for surface sampling and coordinate conversion
  with analytic plane and sphere implementations. Surface descriptors are pure
  parameters without GUIDs or revision counters; snapshots retain their inputs.
  Compare active parameters exactly to detect generation changes (plane ignores
  sphere radius; sphere ignores plane normal). Preserve inactive authored values
  for mode switching without rebuilding the current preview.
- [ ] Build constrained position, tangent, final arc-length, and frame snapshots
  from the selected alignment representation. Enforce numeric error bounds and
  subdivision limits with explicit failure diagnostics.
- [ ] Enforce road-up alignment to projected reference up while maintaining
  frame sign continuity; handle degenerate samples according to Stage 0.
- [x] Validate lane intervals against final length and expose samples by meters
  containing position, direction, up, side, and lane lateral extents.
- [ ] Qualify flat, curved, sloped, sphere-seam/polar, long-arc, and rigidly
  transformed fixtures. Check spherical radial deviation, orthonormality,
  station monotonicity, and local/world query agreement numerically.

Completion: both providers pass the same query contract, supported spherical
fixtures meet recorded error bounds, and invalid fits cannot publish as ready.

### Stage 3: Connect assets to scene previews

Depends on Stage 2.

- [ ] Add a reflected road scene adapter and editor placement/configuration
  entry using the existing asset. Keep surface placement separate from source
  graph authoring and reject unsupported instance transforms.
- [ ] Produce constant-width road previews through SplineMesh components using
  bounded evaluated intervals and stable generated identities. Validate that
  deformation between endpoints meets the surface/frame error budget; subdivide
  further or diagnose unsupported geometry when it does not.
- [ ] Rebuild on asset/surface changes through explicit notifications or an
  existing revision mechanism; reuse components when identities survive and
  remove obsolete components without serializing derived state.
- [ ] Expose ready/stale/error state and diagnostics. Read generation asset
  revisions from alignment snapshots for debugging; do not duplicate or expose
  a revision property on the scene actor.
  Verify replacement, invalid rebuild, missing mesh/provider, detach, and
  unload behavior. Keep picking and any enabled collision tied to the displayed
  geometry revision.

Completion: plane and sphere scenes display roads from `DRoadNet`; rebuilding
does not create duplicate components or dirty assets just to regenerate them;
unsupported inputs and stale results are visible to the user.

### Stage 4: Qualify and hand off the foundation

Depends on Stage 3.

- [x] Run focused native contract and integration tests under the repository
  [testing workflow](../Agents/Testing.md); use the
  [build workflow](../Agents/BuildAndRun.md) before building or running targets.
- [ ] Verify save/reload, instance placement, asset mutation, Undo/Redo,
  generated-component retirement, material display, and picking in plane and
  sphere fixtures. Record exact commands, configurations, and visual evidence.
- [ ] Record unsupported cases and numerical limits; distinguish analytic
  surface evidence from production-terrain support and source SplineMesh LOD
  support from a road-specific LOD system.
- [ ] Publish implemented runtime/editor contracts in the owning documentation
  domains, update roadmap P0 and this plan with evidence, and identify the
  bounded P1 entry scope without starting its implementation implicitly.

Completion: all previous gates pass, evidence is recorded, and implemented
contracts are discoverable outside this plan. Only then mark P0 complete.

## Related Code

- [Road types](../../RoadWeaver/Source/Runtime/RoadWeaver/Public/RoadNet/RoadNetTypes.h)
- [Road validation and publication](../../RoadWeaver/Source/Runtime/RoadWeaver/Private/RoadNet/RoadNet.cpp)
- [Value builder](../../RoadWeaver/Source/Runtime/RoadWeaver/Private/RoadNet/RoadNetBuilder.cpp)
- [Asset creation dialog](../../RoadWeaver/Source/Editor/RoadWeaverEditor/Private/Dialogs/RoadNetCreateDialog.cpp)
- [Spline evaluation](../../Engine/Source/Runtime/Engine/Public/Spline/SplineCurve.h)
- [Spline frame foundation](../../Engine/Source/Runtime/Engine/Public/Spline/SplineMeshDeformer.h)
- [Generated spline mesh actor](../../Engine/Source/Runtime/Engine/Private/Actors/SplineMeshActor.cpp)
- [Implemented spline contracts](../Runtime/World/SplineSystem.md)
