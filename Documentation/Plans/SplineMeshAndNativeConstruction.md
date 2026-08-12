# Spline Mesh and Native Construction Plan

Summary: Add a production single-segment SplineMesh primitive and a reusable native construction system that lets a SplineMesh Actor reconcile stable generated segments without Blueprint, polling, or serialized derived components.

Last reviewed: 2026-08-12

Status: Completed
Completed: 2026-08-12

## Current Status

Stages 0 through 7 are complete. Durin now has deterministic CPU/GPU
SplineMesh deformation, typed scene integration, stable native generated
component reconciliation, a production `ASplineMeshActor` authoring workflow,
exact deformed collision, read-only generated hierarchy presentation, and
lasting Runtime/Editor documentation. Focused and full native tests, Vulkan
parity/reload tests, documentation validation, the full editor build, and an
editor startup smoke all pass in `Win64-Debug-DurinEditor`.

Frozen isolated measurements meet every Stage 0 budget: 32-segment native
reconstruction p95 is 3.4499 ms, SplineMesh GPU p95 is 0.246656 ms versus
0.197216 ms static (0.04944 ms delta), one road-segment collision rebuild p95
is 2.9709 ms, 128 retained collision segments use 1,736,704 bytes, and one
`DSplineMeshComponent` occupies 800 structural bytes. Performance qualification
uses `DURIN_RUN_SPLINE_PROFILE=1` so concurrent full-suite scheduling cannot
pollute wall-clock microbenchmarks.

## Goal

Deliver two reusable production contracts:

1. `DSplineMeshComponent` is an independently authorable `DMeshComponent` that
   bends one StaticMesh across one local cubic Hermite segment, participates in
   the normal material, visibility, LOD, lighting, picking, and collision paths,
   and updates deformation without rebuilding asset-owned GPU geometry.
2. `ASplineMeshActor` owns one editable `DSplineComponent` and deterministically
   reconciles one transient generated `DSplineMeshComponent` per outgoing spline
   segment through a general native-construction facility. Stable point GUIDs
   preserve generated identity across geometry edits, reorder, insertion,
   duplication, Undo/Redo, save/load, and PIE duplication.

After completion, a user can place `ASplineMeshActor`, assign a segment mesh and
materials, edit its spline in the Level Editor, save/reload the level, enter and
leave play, and obtain the same visible and collidable path without Blueprint or
manual component management.

## Scope

- A game-thread-only native actor construction/reconstruction lifecycle with
  stable keyed generated-component acquisition, reuse, retirement, attachment,
  registration, play-state routing, and failure cleanup.
- Explicit authored, native-default, instance, and generated component creation
  methods; generated components are transient derived state and never package
  authority.
- A synchronous, reentrancy-safe, coalescing reconstruction request path used by
  native runtime setters, reflected edits, spline mutations, load, duplication,
  Undo/Redo, and actor spawn.
- A single-segment SplineMesh authoring contract including mesh, materials,
  forward axis, start/end position and tangent, scale, roll, offset, up
  direction, and interpolation policy.
- One finite CPU reference deformation implementation shared by bounds, editor
  ray queries, collision construction, golden tests, and shader parity tests.
- GPU vertex deformation that reuses StaticMesh LOD vertex/index resources,
  adds no per-component copy of source geometry, and supplies deformation as
  detached proxy/draw data.
- A typed SplineMesh primitive scene family, prepared geometry, shader/pipeline
  identity, dynamic deformation update, material binding, visibility, LOD,
  opaque/masked/translucent pass, lighting, diagnostics, and lifecycle support.
- Conservative deformation-aware bounds and exact LOD 0 CPU picking.
- Triangle-mesh collision produced from the same deformed LOD 0 reference
  geometry, with revisioned rebuild and ordinary primitive physics-state
  replacement.
- `ASplineMeshActor` segment reconciliation keyed by each outgoing start-point
  GUID, including the closed-loop seam segment.
- Reflected actor defaults, Details presentation, generated-component hierarchy
  policy, spline editing workflow, Undo/Redo, package round trips, duplication,
  PIE, runtime mutation, and editor/runtime validation.

## Non-Goals

- Blueprint, a visual construction graph, arbitrary user script execution, or
  hot reload of construction code. The native lifecycle is designed so a later
  Blueprint system can use the same context and ownership rules.
- A whole-path `DSplineMeshPathComponent`, procedural extrusion component,
  landscape road tool, PCG/scatter system, or spline follower.
- Automatic mesh selection by tags, per-segment asset arrays, intersections,
  junction meshes, caps, lane metadata, terrain conformance, or navmesh baking.
- Runtime mesh topology generation, tessellation, mesh shaders, Nanite-style
  rendering, GPU-driven indirect draws, or instancing generated segments into
  one draw.
- Deforming authored simple collision primitives or convex hulls. This plan's
  production collision mode is the deformed StaticMesh LOD 0 triangle surface.
- World-distance parameterization under non-uniform component scale. Spline and
  SplineMesh distances remain in their documented component-local domains.
- Changing `FSplineCurve` authoring points to store orientation, roll, width, or
  scale. Those are consumer policies.
- Hiding unsupported input through silent repair. Degenerate source forward
  extent, non-finite settings, missing render data, and invalid collision input
  have named validation/fallback behavior.

## Design Decisions and Invariants

### One component represents one independent Hermite segment

- `DSplineMeshComponent` derives from `DMeshComponent` and owns exactly one
  deformation interval. It does not own, reference, search for, subscribe to,
  or mutate a `DSplineComponent`.
- Start/end position and tangent are component-local. Tangents are Hermite
  derivatives with respect to segment-local `T` in `[0, 1]`; they are not unit
  directions and are not multiplied by an implicit segment length.
- Start/end scale and offset are finite 2D values in the two axes perpendicular
  to `ForwardAxis`. Roll is finite radians. `SplineUpDirection` is normalized
  during validated publication and has a documented deterministic fallback
  when it is degenerate or parallel to the evaluated tangent.
- `ESplineMeshAxis::{X,Y,Z}` identifies the source mesh longitudinal axis.
  Source forward minimum/maximum come from the StaticMesh's canonical LOD 0
  local bounds and apply to every LOD so LOD changes cannot slide the mesh along
  the spline. A non-finite or zero canonical forward extent is non-renderable.
- The default deformation is identity-compatible for a source mesh whose
  forward bounds map to a straight segment, unit scale, zero roll/offset, and a
  compatible up direction. Tests freeze the exact mapping and tangent-basis
  handedness.

### Deformation math has one CPU authority and one shader translation

- `FSplineMeshParams` is a finite value-only structure usable outside a
  component. `FSplineMeshDeformer` evaluates position, first derivative, frame,
  scale, roll, offset, normal, tangent, handedness, and conservative radius from
  those parameters.
- Cubic position and derivative use the same Hermite basis as the Spline
  foundation. Scale, roll, and offset use one explicitly selected interpolation
  function; Stage 0 freezes linear versus smooth interpolation and its boundary
  behavior before shader work.
- Frame construction projects the configured up direction against the tangent,
  chooses the least-aligned cardinal axis as a deterministic singular fallback,
  applies roll around the tangent, and reconstructs an orthonormal right-handed
  basis. Empty/zero derivatives use the preceding valid construction input in
  whole-path frame preparation and a named local fallback in independent
  component evaluation; NaN/Inf is never published.
- `FSplinePathFrameData` is a consumer-owned immutable whole-curve result built
  by parallel transport over the existing adaptive Spline distance samples.
  `ASplineMeshActor` uses it to derive each segment's up/roll endpoints and seam
  correction. It does not extend serialized `FSplinePoint` data.
- CPU reference fixtures are golden input/output authority. Slang implements
  the same equations and is qualified against CPU-transformed vertices and
  tangent bases. Rendering never uses a separate approximate curve definition.

### Rendering reuses source geometry and deforms in the vertex stage

- A SplineMesh proxy borrows immutable `FStaticMeshRenderData` under the same
  component render-state/asset-retirement fence contract as StaticMesh. Index,
  position, tangent, UV, and color buffers are not copied per component.
- `EPrimitiveSceneProxyKind::SplineMesh`, `FSplineMeshSceneProxy`, typed
  `FScene` membership, and typed access are explicit. Renderer does not pretend
  a SplineMesh is a StaticMesh or rediscover it with RTTI.
- Prepared mesh work gains an explicit vertex-factory/deformation domain. That
  domain participates in shader-map, graphics-pipeline, and draw sort identity;
  Local, Spline, and Skeletal shaders can never alias one cache entry merely
  because their material identity matches.
- Shared material resolution, blend/depth/raster policy, per-view lighting,
  environment lighting, view mode, and pass ordering remain common mesh-pass
  behavior. SplineMesh must not fork a stale copy of StaticMesh material policy.
- A deformation-only setter publishes one immutable dynamic-data update carrying
  normalized parameters, deformation revision, and local bounds. It does not
  recreate render state or source GPU resources. Mesh replacement, unavailable
  render data, or incompatible material-slot topology recreates the proxy.
- SceneInfo replaces deformation data and local/world bounds atomically in FIFO
  order. Updates after removal are ignored; a later add creates a new identity.
  Renderer never reads a component, Actor, Spline, reflected asset, or editor
  object.
- LOD selection uses deformed world bounds and the existing projected-size
  policy. Every selected LOD uses the canonical LOD 0 forward mapping.

### Bounds, picking, and collision agree with visible deformation

- Fast local bounds are the cubic Bezier control hull expanded by a conservative
  maximum transformed cross-section radius derived from canonical source bounds,
  scale, and offset. The formula is finite, contains the CPU-deformed vertices
  of every renderable LOD, and is independent of view state.
- Editor triangle picking builds an immutable deformed LOD 0 acceleration
  payload from the CPU reference implementation. Bounds-only picking is not the
  final contract.
- Collision uses that same finite deformed LOD 0 position/index result to build
  one `FCollisionGeometryRef::BuildTriangleMesh` payload. Render and collision
  revision inputs include StaticMesh render-resource identity and every
  deformation field.
- A successful relevant mutation publishes new render dynamic data, editor
  picking data, and collision state from one normalized parameter snapshot.
  Failed collision construction disables/rejects collision according to the
  existing primitive contract but cannot suppress valid rendering.
- Generated child components default to `NoCollision` until the actor's
  reflected collision policy enables the deformed triangle mode; the actor
  applies one consistent policy to all segments.

### Native construction owns transient derived components

- Add `EComponentCreationMethod::{NativeDefault, Instance, Generated}`. Existing
  default and instance behavior remains persistent. Generated is explicit,
  transient, non-authoritative derived state.
- Generated components are retained by a dedicated transient actor collection
  and included in actor ownership, registration, visibility, transform,
  BeginPlay/EndPlay, destruction, lookup, and editor-picking lifecycles. They are
  excluded from package serialization, duplication source data, default-relative
  comparison, package dirtying, and independent transaction capture.
- Public enumeration distinguishes authored components from all live owned
  components. Existing ambiguous call sites are audited and migrated rather
  than silently changing whether they see generated children.
- `FActorConstructionContext` is game-thread-only and created by `AActor` for
  one reconstruction generation. Native code acquires a generated component by
  `(stable key, exact class)`. Equal keys reuse identity; a class mismatch is a
  construction error, not an implicit destructive replacement.
- The context validates unique keys and a complete desired set before commit.
  Newly constructed candidates remain unregistered until commit. Unclaimed old
  generated components retire only after the new desired set is valid. Failure
  destroys candidates and leaves the previous committed set live.
- Commit applies attachment before registration, then routes registration and
  BeginPlay consistently with the owner. Retirement routes EndPlay,
  unregistration, detachment, and destruction exactly once in the inverse
  lifecycle order.
- Reconstruction is synchronous on the game thread, guarded against recursive
  execution, and coalesces requests raised while a pass is active into at most
  one following pass. It never polls revisions on Actor or Component Tick.
- Reconstruction itself never marks a package dirty. The authored mutation that
  requested it owns dirty state and Undo/Redo. Load, duplication, PIE cloning,
  and spawn reconstruction are observational with respect to saved revision.
- Actor spawn, successful load, duplication completion, Undo/Redo replay, and
  relevant runtime/property mutations enter one public lifecycle path. A later
  Blueprint construction system must submit through the same context rather
  than inventing a second component ownership model.

### SplineMesh Actor is the native whole-path policy owner

- `ASplineMeshActor` creates one native-default `DSplineComponent` as root and
  exposes the shared segment mesh, material overrides, forward axis, scale/roll/
  offset policy, frame seed/up policy, deformation interpolation, visibility,
  and collision policy as persistent authored properties.
- Each non-empty outgoing spline segment has one generated key derived from its
  start `FSplinePoint::Id`; the closed-loop seam therefore uses the last point's
  GUID. Empty and one-point curves generate no SplineMesh components.
- Geometry-only changes reacquire and update the same generated components.
  Topology changes reconcile additions/removals by GUID. Reorder cannot transfer
  a component to a different logical start point.
- `DSplineComponent` exposes a game-thread mutation notification containing the
  published revision and flags. Notification occurs after the immutable
  evaluation snapshot is published, forbids reentrant Spline mutation, and
  tolerates listener removal during owner teardown. The actor requests native
  reconstruction from this notification.
- Generated components attach to the Spline root with identity relative
  transform. Segment deformation remains in Spline component-local space; Actor
  transform is applied once through normal primitive SceneInfo transform.
- Generated segment objects are implementation detail in the hierarchy: they
  are visible for diagnostics but read-only, cannot be independently renamed,
  duplicated, deleted, reordered, or saved, and redirect selection/edit actions
  to the owning Actor/Spline plus stable segment key.

### Active renderer plans are dependencies, not alternate contracts

- Stage 0 records the exact integration state of
  `RendererLightSceneContract.md` and `ComputeRendererIntegration.md` before
  touching shared prepared-view, shader-map, pipeline, or base-pass code.
- SplineMesh consumes the then-current prepared light and post-process contracts.
  It does not restore single-light uniforms, per-draw lighting reconstruction,
  old output paths, or compatibility branches removed by those plans.
- If either active plan changes a shared identity or prepared-view contract,
  this plan records the new dependency and rationale before its affected stage
  continues. Independent native-construction and CPU deformation stages may
  proceed while renderer work is active.

## Current Foundations and Gaps

| Area | Existing foundation | Required end state |
| --- | --- | --- |
| Spline data | Stable GUID points, Hermite evaluation, distance tables, bounds, snapshots, revisions | Post-publication mutation notification and consumer-owned path frames |
| Actor components | Default and authored instance ownership, registration/play lifecycle | Explicit generated creation method, transient registry, keyed atomic reconciliation |
| StaticMesh | CPU LOD metadata, asset-owned GPU streams, materials, LOD, editor acceleration, collision data | Safe shared source borrow for SplineMesh, canonical forward bounds |
| Primitive scene | Detached proxies, stable IDs, typed Static/Skeletal membership, FIFO updates | Typed SplineMesh membership and atomic deformation/bounds update |
| Vertex factories | Local and Skeletal fetch/decode paths | Spline deformation shader domain with complete cache identity |
| Mesh renderer | Static/Skeletal preparation and material/pass execution | Common mesh-pass policy with Spline prepared work and diagnostics |
| Editor | Transactional Spline mode, component hierarchy, primitive picking | Native Actor workflow, read-only generated children, exact deformed picking |
| Physics | Revisioned component state and immutable triangle geometry | Deformed LOD 0 triangle payload rebuilt from normalized SplineMesh state |

## Implementation Stages

### Stage 0: Freeze contracts, inventories, equations, and baselines

- [x] Inventory every Actor component collection/enumerator, archive and
  duplication traversal, registration/play/destruction loop, hierarchy/picking
  consumer, `AddInstanceComponent` caller, package-dirty mutation, and test
  fixture affected by a generated creation method.
- [x] Freeze exact native construction entry points and ordering for runtime
  spawn, asset load, duplication, PIE, Undo/Redo, registration, BeginPlay,
  reconstruction during play, failure, recursive request, owner destruction,
  and editor shutdown.
- [x] Freeze generated key representation, creation-method serialization rules,
  authored/all-component enumeration names, transient object retention, atomic
  reconciliation failure behavior, and hierarchy editability.
- [x] Freeze `FSplineMeshParams` field types/defaults/units, forward-axis mapping,
  Hermite basis, longitudinal normalization, scale/roll/offset interpolation,
  frame fallback, tangent-basis handedness, UV/color preservation, finite
  validation, and degenerate-input behavior.
- [x] Record golden CPU deformation fixtures for straight, curved, twisted,
  scaled, offset, reversed tangent, near-up-parallel, zero derivative, and all
  three forward-axis cases.
- [x] Prove the conservative bound formula contains an exhaustive deterministic
  CPU vertex corpus for every LOD and record tolerance/error policy.
- [x] Inventory primitive kind/access, typed scene membership, visibility,
  prepared geometry, shader type, shader-map/pipeline key, material binding,
  LOD, lighting, pass execution, counters, editor observation, StaticMesh
  render-state recreation, and test doubles affected by SplineMesh.
- [x] Record current statuses of the active light and compute renderer plans and
  freeze the integration baseline that SplineMesh renderer stages must consume.
- [x] Select representative straight/curved path assets, segment/vertex counts,
  target GPU, resolution, editor drag workload, CPU reconstruction budget, GPU
  delta budget, generated memory budget, and collision rebuild budget.
- [x] Run and record focused Actor/component, Spline, StaticMesh, material,
  renderer, viewport, collision, serialization, and Vulkan baselines or name
  each pre-existing failure.

#### Acceptance Gate

- Component ownership, construction ordering, deformation equations, render
  identity, dynamic update, bounds, picking, collision, editor behavior,
  performance workloads, and failure semantics contain no unresolved choice.
- Golden fixtures and conservative-bound proof data are checked in or captured
  in the Stage 0 handoff before production C++ or shader implementation.
- Shared renderer dependencies name one current contract; no SplineMesh stage is
  planned against an interface already scheduled for removal.

### Stage 1: Implement deterministic deformation and path-frame foundations

- [x] Add reflected axis/interpolation enums and value-only `FSplineMeshParams`
  with strict finite normalization and equality/revision input support.
- [x] Implement `FSplineMeshDeformer` CPU position, derivative, frame,
  tangent-basis, scale, roll, offset, longitudinal mapping, and conservative
  bound calculations without component, renderer, editor, or physics ownership.
- [x] Add `FSplinePathFrameData` construction over immutable Spline evaluation
  snapshots, including open endpoints, closed-loop seam correction, duplicate
  positions, zero derivatives, frame seed, and deterministic fallback.
- [x] Add golden unit tests and randomized finite/property tests for CPU
  deformation, orthonormality, handedness, endpoint exactness, bound
  containment, repeatability, and path-frame seam continuity.
- [x] Document the lasting deformation/frame contract under Runtime World and
  keep `FSplineCurve` authoring free of consumer roll/scale state.

#### Acceptance Gate

- Every frozen golden fixture passes on the CPU implementation; randomized
  finite inputs produce finite results and bounds contain every transformed
  corpus vertex within the frozen tolerance.
- Open and closed path frames remain deterministic and continuous under stable
  GUID reorder/round-trip inputs, including the closed seam.
- The implementation has no Engine object, renderer, RHI, editor, or physics
  dependency beyond existing math/Spline value contracts.

### Stage 2: Add native construction and generated-component ownership

- [x] Add component creation-method state and split persistent authored
  ownership from transient generated retention without changing existing native
  default or instance package round trips.
- [x] Add explicit authored-versus-all component enumeration and migrate Actor,
  Level, World, hierarchy, picking, visibility, attachment, registration,
  ticking, play, destruction, GC, duplication, and archive call sites according
  to the Stage 0 inventory.
- [x] Implement `FActorConstructionContext` keyed acquisition, class validation,
  candidate staging, attachment, atomic commit, reuse, unclaimed retirement,
  rollback, generation diagnostics, and exact lifecycle routing.
- [x] Implement `AActor` construction request/coalescing/reentrancy state and
  entry routing after spawn/load/duplication/Undo/Redo plus relevant native
  runtime mutation.
- [x] Ensure generated acquisition/reconciliation never marks package dirty,
  never serializes derived objects, and never records independent transactions.
- [x] Add native test Actors/components covering key reuse, insert/remove/class
  mismatch, candidate failure, recursive request, registration, BeginPlay,
  EndPlay, owner destruction, save/load, duplication, PIE, Undo/Redo, GC, and
  editor hierarchy policy.
- [x] Publish the lasting native construction and generated-component lifecycle
  contract under Runtime World.

#### Acceptance Gate

- Generated components have stable identity for equal keys, exactly-once
  lifecycle transitions, and atomic previous-set preservation on failure.
- Package output, saved revision, duplication source data, and Undo history are
  byte/behavior equivalent whether deterministic reconstruction ran once or
  repeatedly.
- Existing default/instance component tests and level packages remain compatible;
  all internal callers intentionally choose authored-only or all-live traversal.

### Stage 3: Implement the independent SplineMesh component and derived CPU state

- [x] Add `DSplineMeshComponent` as a reflected `DMeshComponent` with validated
  setters, StaticMesh/material binding, normalized deformation snapshot,
  deformation revision, and precise property-edit hooks.
- [x] Reuse/generalize StaticMesh material-slot validation, default/ErrorMaterial
  fallback, asset render-resource readiness, and render-state recreation so both
  StaticMesh consumer component classes obey one asset-retirement protocol.
- [x] Build and atomically publish immutable deformation-derived state containing
  normalized parameters, conservative local bounds, exact deformed LOD 0
  positions/indices, editor acceleration, collision input identity, and
  diagnostic status.
- [x] Distinguish deformation-only, material-only, transform-only, mesh-resource,
  and collision-policy invalidation; avoid proxy recreation for deformation-only
  edits.
- [x] Add reflection, default-object, package, duplication, property validation,
  material, bounds, editor picking, source asset replacement/reimport, and
  revision tests.

#### Acceptance Gate

- A standalone component round-trips every authored field and produces the same
  normalized snapshot/bounds/picking result after load and duplication.
- Deformation edits advance only the required revisions; invalid proposals do
  not partially change authoring, render, picking, collision, package, or
  transaction state.
- StaticMesh replacement and retirement leave no proxy/CPU-derived borrow past
  the existing targeted fence contract.

### Stage 4: Integrate SplineMesh into scene representation and GPU rendering

- [x] Add explicit SplineMesh primitive kind, detached proxy, typed SceneInfo
  access/list membership, attach/replace/detach/release behavior, and editor
  primitive observation family.
- [x] Add FIFO SplineMesh dynamic-data mutation that atomically replaces copied
  deformation parameters and bounds without component reads or render-state
  recreation.
- [x] Generalize prepared mesh geometry and base-pass policy around explicit
  Local/Spline/Skeletal vertex domains while preserving family-specific pose or
  deformation payloads and typed scene iteration.
- [x] Add Spline vertex decode/deformation shader module and shader types; extend
  shader-map, pipeline, resource-cache, and sort identities with the vertex
  domain and complete deformation bindings.
- [x] Reuse source StaticMesh LOD buffers/indices and declarations safely; bind
  one immutable per-primitive deformation payload and preserve UV, color,
  material, masked discard, tangent-space normal mapping, winding, and
  front-face behavior.
- [x] Integrate visibility, deformed bounds, automatic/forced LOD, opaque/masked/
  translucent ordering, Unlit/Lit modes, current prepared lighting,
  environment lighting, main/auxiliary/offscreen views, and resource recovery.
- [x] Add counters for visible candidates, rejected/invalid deformation,
  selected LOD/triangles/sections, dynamic updates, fallbacks, and retained
  deformation bytes with conservation assertions.
- [x] Add CPU-versus-shader image/readback fixtures, scene lifecycle tests,
  material/pass tests, LOD/culling tests, multi-view tests, device/shader reload
  tests, and Vulkan validation coverage.

#### Acceptance Gate

- Straight and curved SplineMesh pixels, silhouette, normal response, and
  tangent-space material output match frozen CPU references within tolerance in
  opaque, masked, and translucent passes.
- Deformation-only updates preserve primitive identity and source GPU resources,
  update bounds before later FIFO visibility, and leave no stale typed member
  after retirement.
- Every supported view and current lighting path renders SplineMesh through the
  shared material/pass policy with complete cache identity and clean Vulkan
  validation.

### Stage 5: Add native SplineMesh Actor reconciliation and editor workflow

- [x] Add `ASplineMeshActor` with native-default Spline root and persistent path
  mesh, material, axis, scale/roll/offset, frame, interpolation, visibility, and
  collision policies.
- [x] Add post-publication `DSplineComponent` mutation subscription with safe
  listener lifetime, revision/flags payload, non-reentrant mutation contract,
  and tests for setters, reflected edits, load, duplication, Undo/Redo, and
  teardown.
- [x] Build complete desired segment specs from one captured immutable Spline
  snapshot and path-frame result, key each by outgoing start-point GUID, then
  reconcile through `FActorConstructionContext`.
- [x] Cover open/closed, empty/one-point, insertion, duplicate, delete, reorder,
  loop toggle, interpolation/tangent edit, geometry drag, Actor transform,
  StaticMesh replacement, property edit, Undo/Redo, Cancel, load, duplication,
  PIE, and runtime Spline mutation.
- [x] Add Details layout/actions, generated count and diagnostic display,
  read-only generated hierarchy presentation, selection redirection, direct
  `Edit Spline` entry, and actionable invalid mesh/deformation/collision errors.
- [x] Add editor functional tests proving geometry drags update existing segment
  identities, topology edits reconcile only changed GUID keys, Cancel restores
  output/dirty state, and generated children cannot become saved authoring.

#### Acceptance Gate

- One placed Actor supports the complete no-Blueprint authoring workflow and
  produces exactly one stable generated component per outgoing segment,
  including the closed seam.
- Save/load, duplication, PIE, Undo/Redo, and repeated reconstruction preserve
  authored data and regenerate equivalent derived components without serialized
  generated objects or false dirty state.
- Continuous Spline editing meets the frozen Stage 0 CPU/editor responsiveness
  budget with no Tick polling or full source GPU geometry rebuild.

### Stage 6: Integrate deformed collision and runtime mutation

- [x] Add the reflected SplineMesh collision mode and normalize its relationship
  to `ECollisionEnabled`, profile, object channel, response table, and generated
  Actor policy.
- [x] Build immutable triangle collision from the exact deformed LOD 0 CPU
  result, reuse it while the full input identity is unchanged, and replace
  physics state only after successful construction.
- [x] Cover zero-area triangles, mirrored scale, closed seam adjacency,
  non-uniform Actor scale, source reimport, collision disable/enable, failed
  build, body retirement, and runtime deformation while registered/playing.
- [x] Add ray, sweep, overlap, body-count, stale-handle, retained-geometry, and
  render-versus-collision hit fixtures for standalone and generated segments.
- [x] Measure collision build time and retained bytes on the frozen workloads;
  exceeding either budget keeps the stage open and requires a separately
  selected asynchronous/cooked strategy rather than an unbounded synchronous
  editor path.

#### Acceptance Gate

- Visible and collision surfaces agree on the frozen corpus, mutation replaces
  bodies without a stale query window after commit, and disabled/failed
  collision cannot create a partial body.
- Geometry/body retention counters return to baseline after segment removal,
  Actor destruction, level teardown, PIE exit, and StaticMesh replacement.
- Runtime Spline mutations remain deterministic and meet the frozen collision
  budget or the plan records and implements the selected bounded build strategy
  before completion.

### Stage 7: Qualify production behavior and publish lasting documentation

- [x] Remove obsolete compatibility branches, duplicated StaticMesh-only mesh
  policy, temporary diagnostics, and migration scaffolding after all consumers
  use the selected contracts.
- [x] Publish native construction/generated-component ownership under Runtime
  World; update Spline, scene representation, StaticMesh/mesh rendering,
  collision, reflected editing, viewport editing, and user workflow documents.
- [x] Run focused native targets for each owner, documentation validation,
  relevant Vulkan integration and image tests, then the repository-required
  full `all` build/test validation because the completed feature crosses Actor,
  serialization, Engine, RenderCore, Renderer, shader, physics, and editor test
  targets.
- [x] Launch the editor executable from the validated Agent Build Profile and
  complete an 8-second startup smoke; cover place/edit/save/reload/duplicate/
  PIE/runtime-mutation/collision and opaque/masked/translucent/Lit/Unlit paths
  through the focused automated workflow and Vulkan fixtures.
- [x] Record measured CPU/GPU/memory/collision results against Stage 0 budgets,
  close every acceptance row, update plan lifecycle metadata, and hand off the
  verified editor executable.

#### Acceptance Gate

- All ownership, serialization, construction, deformation, renderer, material,
  LOD, bounds, picking, collision, editor, runtime, recovery, performance,
  Vulkan, build, and manual workflow rows pass.
- Lasting documentation owns the implemented contracts; no required behavior
  survives only in this plan.
- A clean editor session can author and run a production SplineMesh path without
  Blueprint, polling, serialized generated segments, or manual child creation.

## Completion Evidence

- Focused native validation passed: Spline 41/41, World 102/102, Viewport
  88/88, PhysicsScene 43/43, StaticMesh 69/69, Material 79/79,
  EditorRendering 40/40, LevelAuthoring 11/11, and Renderer scene contract
  12/12.
- Vulkan validation passed for exact StaticMesh/SplineMesh CPU-shader parity,
  shared skeletal/SplineMesh resources and profiling, and renderer resource
  reload. Opaque, masked, translucent, Lit, and Unlit paths are included in the
  fixtures.
- `DURIN_RUN_SPLINE_PROFILE=1` isolated qualification passed with 3.4499 ms
  32-segment reconstruction p95, 0.04944 ms GPU p95 delta, 2.9709 ms collision
  p95, 800 bytes per component, and 1,736,704 retained collision bytes for 128
  segments.
- `DevTool.bat doc validate` passed 101 files, `test --target all` passed the
  complete native target matrix, and `build --target all` passed for
  `Win64-Debug-DurinEditor`.
- The validated `DurinEditor.exe` remained running for the 8-second hidden
  startup smoke and was then stopped cleanly by the smoke harness.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Straight identity mapping | Endpoints, longitudinal mapping, cross-section, normals, tangents, UVs, and colors match the frozen reference | Engine deformation tests |
| Curved/twisted mapping | CPU and shader positions/bases agree; no NaN/Inf or unintended handedness flip | Engine/Renderer tests |
| Degenerate input | Named validation or deterministic finite fallback; no partial publication | Engine tests |
| Bounds | Conservative local/world bounds contain every deformed vertex for every LOD | Engine/visibility tests |
| GPU resource ownership | Segment instances reuse source buffers and survive/release with the StaticMesh fence protocol | Engine/Renderer lifecycle tests |
| Dynamic deformation | Primitive/resource identity is stable; FIFO data and bounds update together | Scene contract tests |
| Materials and passes | Default, override, ErrorMaterial, opaque, masked, translucent, Lit, and Unlit match shared policy | Material/Renderer tests |
| LOD and views | Automatic/forced LOD, culling, main, auxiliary, offscreen, thumbnail-capable path, and sequential views are isolated | Renderer tests |
| Lighting integration | SplineMesh consumes the current prepared light ABI and environment path | Renderer image/integration tests |
| Generated identity | Equal start GUID reuses one component; topology edits add/remove only changed keys | World/Spline tests |
| Construction failure | Previous committed generated set remains live; candidates retire exactly once | World tests |
| Serialization | Only authored Actor/Spline/policy data round-trips; generated components do not appear in package data | Asset/World tests |
| Duplication and PIE | New owner regenerates new object identities with equivalent stable keys and output | World/editor tests |
| Undo/Redo and Cancel | Geometry, generated set, output, selection, package dirty state, and history restore coherently | Spline/viewport tests |
| Editor picking | Deformed LOD 0 surface, not undeformed mesh or bounds alone, owns the hit | Viewport tests |
| Collision | Ray/sweep/overlap results agree with deformed visible surface and retire without stale handles | Engine/Aether tests |
| Runtime mutation | Registered/playing Actor updates render and physics without Tick polling | World/Engine tests |
| Recovery/reload | Shader/device/resource recreation does not retain stale proxy, pipeline, buffer, or deformation state | Renderer/Vulkan tests |
| Performance | Frozen editor drag, render, memory, and collision workloads meet Stage 0 budgets | Stage handoff measurements |
| End-to-end editor | Place, configure, edit, save/reload, duplicate, PIE, mutate, collide, and exit cleanly | Validated editor smoke |

## Definition of Done

- Every stage acceptance gate and validation-matrix row is closed with recorded
  evidence.
- `DSplineMeshComponent` is a standalone one-segment component with stable
  reflection, materials, GPU deformation, bounds, picking, collision, runtime
  update, and asset lifetime behavior.
- Native construction is a documented reusable Actor facility with atomic
  stable-key generated reconciliation and no generated serialization/dirtying.
- `ASplineMeshActor` provides the complete no-Blueprint whole-path workflow and
  uses stable Spline point GUID identity.
- Renderer treats SplineMesh as an explicit typed primitive and shares current
  mesh material/pass/lighting contracts without cache aliasing or component
  reads.
- Focused tests, full native tests, full `all` build, Vulkan validation, image
  evidence, performance budgets, and editor smoke pass.
- Lasting Runtime and Editor documentation is authoritative and this plan is
  marked completed with the verified executable linked in the final handoff.

## Deferred Follow-ups

- Blueprint/visual construction graphs using `FActorConstructionContext`.
- Per-segment mesh/material policy assets and rule-driven junction/cap creation.
- Authored simple/convex collision deformation, offline cooked path collision,
  and navigation integration.
- Segment batching/instancing after measured draw-count evidence, without
  changing authored or generated identity.
- Runtime asynchronous deformation/collision work if Stage 0 production budgets
  demonstrate that bounded synchronous rebuild cannot meet requirements.
- Whole-path extrusion, ribbon, pipe, placement, follower, metadata, and event
  consumers built on the same Spline/path-frame foundation.

## Related Documentation

- [Stage 0 Handoff](SplineMeshAndNativeConstructionStage0.md)
- [Spline System](../Runtime/World/SplineSystem.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Tick Scheduling](../Runtime/World/TickScheduling.md)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Runtime Collision](../Runtime/Physics/Collision.md)
- [Viewport Editing Architecture](../Editor/Architecture/ViewportEditing.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Scene Viewport Navigation](../Editor/Guides/SceneViewportNavigation.md)
- [Renderer Light Scene Contract Plan](RendererLightSceneContract.md)
- [Compute Renderer Integration Plan](ComputeRendererIntegration.md)
- [Spline V2 and Viewport Editing Plan](Archive/2026-08/SplineV2AndViewportEditing.md)
- [Actor Component System Plan](Archive/2026-07/ActorComponentSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Actor.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/ActorComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/MeshComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/SplineComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/SplineComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Spline/SplineCurve.h`
- `Engine/Source/Runtime/Engine/Private/Spline/SplineCurve.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/StaticMeshComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Public/Engine/FPrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/LocalVertexFactory.h`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Shaders/Slang/VertexFactory/LocalVertexFactory.slang`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/SplineEditorCustomizations.cpp`
- `Engine/Tests/Native/EngineTests/Private/SplineTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SplineV2ContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportCustomizationTests.cpp`
