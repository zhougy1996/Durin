# StaticMesh Collision Build Separation Plan

Summary: Separate render and collision construction, remove FStaticMeshAuthoredCandidate, and keep atomic import publication in the owning operation.

Last reviewed: 2026-09-22

Status: Active
Completed:

## Current Status

Stage 0 is complete. Stage 1 now has independent render finalization and an
value-snapshot `FStaticMeshCollisionBuilder` cache adapter. All direct collision
builder consumers in the workspace have migrated. The combined candidate and
its provider metadata remain temporarily for the Stage 2/3 consumer migration.

Validation: `DevTool.bat test StaticMeshTests` passed 132 tests from 23 suites,
including new detached-input lifetime, cancellation, reservation and independent
render-success/collision-failure coverage. The first run found cancellation
precedence had changed; this was corrected before the passing full rerun.
`DevTool.bat build --target all` and changed-document validation also passed.
GPU/performance qualification was not executed: this change does not alter GPU
behavior or introduce a timing acceptance gate.
No independent runtime scheduling/publication or import/cook migration is claimed
yet; Stage 2 through Stage 4 remain open.

### Chosen lifecycle contract

- Ordinary `Build`, `AsyncBuild` and editor `PostLoad` finish at validated render
  publication. Collision failure cannot change that render completion. Pending
  render work alone does not imply pending collision.
- Extend the existing StaticMesh compiling manager with separate collision
  records using its task scope, cooperative cancellation, bounded admission and
  owner-thread mailbox pump. Do not introduce another generic scheduler.
  Each collision request owns a detached value snapshot of LOD0 positions,
  indices and collision settings. Capture copies the render streams once per
  request; moving the request into a worker transfers its arrays without another
  copy. Workers treat the snapshot as immutable. Retry and settings changes
  capture a fresh snapshot; no shared geometry handle or per-generation snapshot
  cache is introduced. Account for snapshot, construction and retained geometry
  until its worker has
  retired, including canceled records. Selected finish drains the selected
  owner's render and collision records; ordinary render completion callbacks do
  not wait for collision. Shutdown stops admission, cancels and drains both.
- Collision publication checks object key, render geometry generation, BodySetup
  object key, settings revision, mode, policy and request generation. Keep the
  request/geometry generations transient; do not repurpose serialized BodySetup
  revision fields as scheduler state. Retry supersedes older collision requests.
  Replacement, geometry invalidation, mode/policy changes, unload and destruction
  invalidate the matching generation and cancel its records before publication.
  Provider registration is checked independently for each detached operation.
- Invalidation clears only geometry-derived collision and immediately refreshes
  registered component physics bodies; successful installation refreshes them
  again. Unavailable/failed collision must leave no prior derived body queryable.
  Authored primitive shapes survive render replacement. BodySetup edits through
  both mesh setters and direct setters need the same owner notification boundary;
  installing or clearing geometry must not recursively schedule another build.
- Standalone import/reimport owns its required collision preparation alongside
  render work in the existing private compilation operation. Scene import owns
  independent outputs in its private prepared-output record. Both validate owner
  snapshots, material bindings, provenance, cancellation and resource preparation
  before the first mutation, then publish in one refresh boundary. Failure or
  owner edits discard the operation without automatic source-changing requeue.
- Cook constructs independent detached render and required collision projections,
  fails when either required output fails, and never publishes authored state.
  Cooked loading decodes and validates the existing payloads without editor recipe
  invocation, then installs them within one refresh boundary.
- Destructive test replacement retains its explicit destructive failure behavior,
  prepares bounds/ray data before publication and invalidates old collision before
  reconstruction. Debug mesh and cooked decode paths also prepare render bounds
  and acceleration before entering the publication function.

Audit evidence: `StaticMeshBuild.cpp`, `StaticMeshCompilingManager.cpp`,
`StaticMesh.cpp`, `StaticMeshCollision.cpp`, `StaticMeshCook.cpp`, `BodySetup.cpp`,
`StaticMeshImport.cpp`, `SceneDirectImport.cpp` and
`StaticMeshRenderStateRecreateContext.cpp`. Existing direct BodySetup notifications
only notify render compilation; they do not themselves refresh component bodies.
Existing cooked installation publishes render before collision, so its refresh
boundary must also be corrected during publication migration.

### UE ownership reference

The selected input model follows UE's
[FTriMeshCollisionData](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/PhysicsCore/FTriMeshCollisionData),
which owns typed vertex/index arrays, and
[FCookBodySetupInfo](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FCookBodySetupInfo),
which contains that description by value. Each local collision request likewise
owns its geometry snapshot. Request copies copy the arrays; moves transfer them.
The user selected this value-snapshot approach over the previously considered
shared immutable handle. UE's general-purpose `FSharedBuffer` is not the input
contract being adopted for this collision builder.

[CreatePhysicsMeshesAsync](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UBodySetup/CreatePhysicsMeshesAsync)
explicitly requires callers to create/update physics state after completion.
This supports keeping component-body refresh as a separate publication concern;
it does not justify moving editor scheduling or DDC into the local BodySetup.

The value-snapshot revision passed all 132 `StaticMeshTests` cases from 23
suites. Regression coverage verifies that the snapshot outlives its render source
and that copying, changing and releasing one request leaves another request
intact. Changed-document validation and all-plan validation passed.

## Goal

Render construction returns owned `FStaticMeshRenderData`. Collision construction
returns immutable collision geometry for a captured geometry/settings input.
Neither builder publishes assets or invokes the other builder. Asset compilation,
import and cook coordinate only the work required by their operation.

Ordinary render reconstruction can succeed when collision reconstruction fails.
Source-changing import remains transactional: when the operation requires new
collision, failure preserves the previous source, slots, provenance, render data
and collision together. No consumer may use collision derived from superseded
geometry as if it matched the current mesh.

## Selected Decisions

- Delete `FStaticMeshAuthoredCandidate`, `BuildCandidate`, `ApplyCandidate` and
  their combined request contract once callers are migrated. Do not introduce
  an equivalent public render-plus-collision wrapper under another name.
- Keep bounds, render validation and render ray-query acceleration in render
  construction. Move their current candidate-finalization work into that path;
  publication must not build bounds, acceleration structures or collision.
- Give collision construction its own entry point and value-only inputs. LOD0
  positions/indices may be captured from completed render data where the selected
  collision mode requires them. Borrowed spans must not outlive their owner;
  asynchronous work owns a detached value snapshot, read immutably by its worker.
- Keep physics geometry algorithms in their existing module. `DBodySetup` owns
  collision settings and installed immutable geometry, not editor DDC access,
  render building, provider registration or scheduling. Engine owns the editor
  collision build/cache adapter; do not add a Physics-to-Editor dependency.
- Ordinary render publication invalidates geometry-derived collision when its
  input changes. Independent collision reconstruction installs geometry only if
  captured mesh/geometry, BodySetup identity, settings and revisions still match.
  Primitive collision must not be discarded merely because render data changes.
- Keep operation-specific transaction state private to import/materialization.
  It may retain independently prepared render data, collision geometry, slot
  definitions and provenance until validation completes. This is not a shared
  public build product and cannot own recipe execution during commit.
- Preserve the previous error simplification: caller-facing messages are string
  arrays. Keep cancellation and revisions in internal control state. Report
  collision failure through collision status; a successful ordinary render build
  must not be relabeled as a render failure. Import/cook report failure when their
  required collision cannot be produced. Do not restore cache-origin observations.
- Reuse existing collision mode, query policy, geometry and cache formats when
  semantics are unchanged. Bump only affected provider interfaces or derived-data
  versions; this is not a blanket asset-format or physics-algorithm migration.

## Implementation Stages

### Stage 0: Define operation completion and revision contracts

Dependency: none. Outcome: concrete lifecycle decisions before changing callers.

- [x] Audit direct Build/AsyncBuild, PostLoad, import/reimport, scene import,
  direct collision edits, cook, cooked loading and destructive test replacement.
- [x] Specify each operation's completion point. Ordinary Build/AsyncBuild
  completion describes render publication; collision has independent readiness.
  Import waits for all required outputs before committing. Cook waits for or
  constructs required collision without publishing an authored projection.
- [x] Select the minimal scheduler implementation for independent collision
  work, including cancellation, selected finish, teardown and memory accounting.
  Reuse existing task infrastructure; do not add a generic compilation framework.
- [x] Define captured input identity and revision checks for collision results,
  including BodySetup replacement, geometry edits, mode None, policy changes,
  explicit retry, unload and destruction. Distinguish transient build state from
  serialized settings and geometry identity.
- [x] Define how unavailable/failed collision removes or refreshes existing
  component physics bodies. Queries must not retain stale published bodies while
  the asset reports collision unavailable.

Completion gate: document the chosen scheduler and per-operation state transitions
in this plan; resolve finish/cancel behavior and transaction failure policy before
implementing independent publication.

### Stage 1: Separate render and collision construction

Dependency: Stage 0. Outcome: independently usable builders with no live mutation.

- [x] Move render finalization out of combined candidate construction and into
  the render build path, preserving cancellation checkpoints and memory limits.
- [x] Extract the collision build/cache entry point from `FStaticMeshBuilder`.
  Preserve cache corruption recovery and independently reported cache warnings.
- [x] Make collision requests capture immutable geometry and collision settings
  without requiring ownership of an `FStaticMeshRenderData` build result.
- [ ] Keep provider lifetime/registration checks appropriate to each operation;
  remove checks and retained metadata used solely by the combined candidate.
- [x] Add focused tests for independent success, failure, cancellation and
  geometry input lifetime. Verify rendering can complete with collision disabled
  or failing, and collision-only edits do not invoke render construction.

Completion gate: neither builder invokes the other or changes a live asset, and
render construction returns a fully validated CPU render result.

### Stage 2: Separate runtime publication and collision lifecycle

Dependency: Stage 1. Outcome: independent readiness with coherent physics bodies.

- [ ] Replace combined candidate handling in ordinary Build/AsyncBuild/PostLoad
  with render-result handling and explicit collision invalidation/scheduling.
- [ ] Remove collision construction from `CommitRenderDataCandidate`; use a
  render publication interface whose inputs are already prepared and validated.
- [ ] Give collision work independent pending/failure/cancellation state.
  `GetCollisionBuildStatus` must not infer readiness from any pending render task.
- [ ] Reject stale collision results and refresh affected component bodies on
  invalidation and successful publication. Preserve applicable primitive setups.
- [ ] Exercise geometry changes during collision work, BodySetup replacement,
  policy/mode edits, mode None, failure/retry, unload and destruction. Verify no
  stale body participates in queries and no retiring task retains leaked storage.

Completion gate: render remains usable after collision failure; collision-only
changes preserve render data and render resource revision; stale collision never
overwrites newer state or survives in component physics bodies.

### Stage 3: Migrate import and cook; remove the combined candidate

Dependency: Stage 2. Outcome: operation-owned transactions and removal of old APIs.

- [ ] Migrate standalone import/reimport and scene import to construct independent
  outputs and validate them before the first live mutation. Preserve prepared
  material-slot bindings and provenance inside their existing transaction scope.
- [ ] Ensure resource preparation and other fallible work precede commit. Reuse
  existing refresh/rollback boundaries; do not implement atomicity as two public
  setters with a fallible build between them.
- [ ] Preserve import rollback on render/collision failure, cancellation or owner
  edits. Do not automatically requeue stale source-changing operations.
- [ ] Migrate cook and cooked loading: required collision failure blocks cook;
  cooked runtime consumes cooked data without invoking editor builders. Preserve
  render/collision payload compatibility and load validation.
- [ ] Delete `FStaticMeshAuthoredCandidate`, `FStaticMeshAuthoredBuildRequest`,
  `BuildCandidate` and `ApplyCandidate`, plus obsolete combined result fields,
  flags, helpers and tests. Remove or simplify `FStaticMeshCollisionBuildProduct`
  if its remaining fields can be expressed using existing geometry types.
- [ ] Search all source/test roots declared in `Durin.dworkspace` and migrate
  every consumer. No compatibility alias or renamed public combined wrapper may
  remain.

Completion gate: old combined APIs have no source/test references; import retains
atomic failure behavior and cook produces the required matching payloads.

### Stage 4: Integration validation and contract documentation

Dependency: Stage 3. Outcome: verified behavior and updated long-lived contracts.

- [ ] Run relevant StaticMesh, scene import, physics, cooked loading, material,
  Spline, thumbnail and asset-compilation tests using the
  [testing workflow](../Agents/Testing.md). Select any additional cook/lifecycle
  targets from the test registry according to the affected behavior.
- [ ] Verify render-success/collision-failure behavior, collision-only rebuilding,
  cache hit/corruption behavior, import rollback, stale result rejection, component
  body coherence and task teardown with focused regression coverage.
- [ ] Complete the shared Engine API `all` build using the
  [build workflow](../Agents/BuildAndRun.md). Compile affected qualification
  fixtures; execute GPU/performance qualification only when its documented gate
  applies, and record execution omissions explicitly.
- [ ] Update [StaticMesh building](../Runtime/Assets/StaticMeshBuilding.md),
  [asset compilation](../Runtime/Assets/AssetCompilation.md),
  [collision](../Runtime/Physics/Collision.md) and
  [rendering](../Runtime/Rendering/StaticMeshRendering.md) with implemented
  ownership, completion, invalidation and publication behavior.
- [ ] Validate changed documentation and plan lifecycle metadata; record concrete
  evidence and mark the plan complete only after all required gates pass.

Completion gate: all required checks pass, remaining limits are documented, and
the plan no longer competes with runtime documents as a behavioral specification.

## Primary Code Locations

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshBuilder.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshBuild.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshBuildProvider.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCompilingManager.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCollision.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCook.cpp`
- `Engine/Source/Runtime/Engine/Public/Physics/BodySetup.h`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp`

Implementation commits must update this plan's status/checklists and use its
exact path and the applicable stage title for repository-required Plan/Stage
commit provenance. Creating this plan does not complete Stage 0.
