# StaticMesh Collision Build Separation Plan

Summary: Separate render and collision construction, remove FStaticMeshAuthoredCandidate, and allow independent render and collision completion for imports.

Last reviewed: 2026-09-22

Status: Completed
Completed: 2026-09-22

## Current Status

All stages are complete under the user's revised import completion contract.
Render construction/publication and collision construction/scheduling are separate;
the combined candidate/request APIs and their provider metadata are removed.
Imports keep successfully published render/source state when collision fails.
Cook independently requires matching render and collision projections. Runtime
contracts are maintained in the linked domain documents below.

Validation on `Win64-Debug-DurinEditor`:

| Target | Passed cases |
| --- | ---: |
| StaticMeshTests | 134 |
| PhysicsSceneTests | 43 |
| SceneImportTests | 20 |
| CookedMeshLoadingTests | 4 |
| StaticMeshMaterialTests | 10 |
| SplineTests | 48 |
| StaticMeshThumbnailTests | 10 |
| AssetCompilingManagerTests | 1 |
| AssetImportTests | 14 |
| AssetCookTests | 39 |
| CookFunctionalTests | 5 |

The 11 targets passed 328 distinct cases. StaticMesh, physics and scene import
were rerun after the final collision snapshot/persistence adjustment; unaffected
passing coverage was retained. All four `FStaticMeshSeparatedBuildTests` cases also
passed individually with `--isolate --test-jobs 1`. Coverage includes detached
snapshot lifetime, render success with collision failure, import source retention,
retry without render reconstruction, geometry/settings/BodySetup supersession,
mode None, cancellation/destruction, reservation retirement, cache persistence,
authored primitive preservation and immediate component-body invalidation in two
Worlds. Existing cache corruption, provenance, cook and loading regressions passed.

`DevTool.bat build --target all` passed after the final shared Engine API changes.
`StaticMeshBuildQualificationTests` and `PhysicsQualificationTests` compiled.
GPU and performance qualification were not executed: no GPU behavior or timing
acceptance gate changed. Searches across Engine, Sandbox and RoadWeaver source/test
roots found no old combined API consumers. Changed-document and all-plan validation
passed. Initial debug-bound preparation and outdated synchronous/requeue test
expectations were corrected before these passing runs.

### Decision revision and contract ownership

On 2026-09-22 the user removed the requirement to roll back a successful import
when collision preparation fails. Import now follows ordinary render completion;
cook still requires both outputs. This removes cross-product import transactions
without relaxing stale-result rejection or component-body invalidation.

Implemented ownership and completion contracts live in
[StaticMesh building](../Runtime/Assets/StaticMeshBuilding.md),
[asset compilation](../Runtime/Assets/AssetCompilation.md#staticmesh-completion),
[collision](../Runtime/Physics/Collision.md#assets-and-components) and
[rendering](../Runtime/Rendering/StaticMeshRendering.md#asset-lifecycle).
The collision input remains a per-request value snapshot; no shared geometry
handle, per-generation snapshot cache or separate generic scheduler was introduced.

## Goal

Render construction returns owned `FStaticMeshRenderData`. Collision construction
returns immutable collision geometry for a captured geometry/settings input.
Neither construction entry point publishes assets or invokes the other builder. Asset compilation,
import and cook coordinate only the work required by their operation.

Ordinary render reconstruction can succeed when collision reconstruction fails.
Source-changing import also completes at render publication. Collision failure
does not roll back newly published source, slots, provenance or render data. No consumer may use collision derived from superseded
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
- Keep operation-specific preparation state private to import/materialization.
  It retains prepared render data, slot definitions and provenance until render
  validation completes; collision is a subsequent independent request. This is not a shared
  public build product and cannot own recipe execution during commit.
- Preserve the previous error simplification: caller-facing messages are string
  arrays. Keep cancellation and revisions in internal control state. Report
  collision failure through collision status; a successful ordinary render build
  must not be relabeled as a render failure. Cook reports failure when required collision cannot be produced; import reports
  collision failure independently. Do not restore cache-origin observations.
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
  Import completes at render publication and schedules collision independently. Cook waits for or
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
- [x] Keep provider lifetime/registration checks appropriate to each operation;
  remove checks and retained metadata used solely by the combined candidate.
- [x] Add focused tests for independent success, failure, cancellation and
  geometry input lifetime. Verify rendering can complete with collision disabled
  or failing, and collision-only edits do not invoke render construction.

Completion gate: neither builder invokes the other or changes a live asset, and
render construction returns a fully validated CPU render result.

### Stage 2: Separate runtime publication and collision lifecycle

Dependency: Stage 1. Outcome: independent readiness with coherent physics bodies.

- [x] Replace combined candidate handling in ordinary Build/AsyncBuild/PostLoad
  with render-result handling and explicit collision invalidation/scheduling.
- [x] Remove collision construction from the render publication boundary; use a
  render publication interface whose inputs are already prepared and validated.
- [x] Give collision work independent pending/failure/cancellation state.
  `GetCollisionBuildStatus` must not infer readiness from any pending render task.
- [x] Reject stale collision results and refresh affected component bodies on
  invalidation and successful publication. Preserve applicable primitive setups.
- [x] Exercise geometry changes during collision work, BodySetup replacement,
  policy/mode edits, mode None, failure/retry, unload and destruction. Verify no
  stale body participates in queries and no retiring task retains leaked storage.

Completion gate: render remains usable after collision failure; collision-only
changes preserve render data and render resource revision; stale collision never
overwrites newer state or survives in component physics bodies.

### Stage 3: Migrate import and cook; remove the combined candidate

Dependency: Stage 2. Outcome: render-complete imports and removal of old APIs.

- [x] Migrate standalone import/reimport and scene import to publish prepared render
  outputs and schedule collision independently. Preserve prepared
  material-slot bindings and provenance inside their existing transaction scope.
- [x] Ensure resource preparation and other fallible work precede commit. Use one
  render refresh boundary, then schedule independent collision.
- [x] Preserve previous render state on render preparation failure, cancellation or owner
  edits. Collision failure must not roll back successful import. Do not automatically requeue stale source-changing operations.
- [x] Migrate cook and cooked loading: required collision failure blocks cook;
  cooked runtime consumes cooked data without invoking editor builders. Preserve
  render/collision payload compatibility and load validation.
- [x] Delete `FStaticMeshAuthoredCandidate`, `FStaticMeshAuthoredBuildRequest`,
  `BuildCandidate` and `ApplyCandidate`, plus obsolete combined result fields,
  flags, helpers and tests. Remove or simplify `FStaticMeshCollisionBuildProduct`
  if its remaining fields can be expressed using existing geometry types.
- [x] Search all source/test roots declared in `Durin.dworkspace` and migrate
  every consumer. No compatibility alias or renamed public combined wrapper may
  remain.

Completion gate: old combined APIs have no source/test references; import permits
independent collision failure and cook produces the required matching payloads.

### Stage 4: Integration validation and contract documentation

Dependency: Stage 3. Outcome: verified behavior and updated long-lived contracts.

- [x] Run relevant StaticMesh, scene import, physics, cooked loading, material,
  Spline, thumbnail and asset-compilation tests using the
  [testing workflow](../Agents/Testing.md). Select any additional cook/lifecycle
  targets from the test registry according to the affected behavior.
- [x] Verify render-success/collision-failure behavior, collision-only rebuilding,
  cache hit/corruption behavior, import render failure preservation, independent
  collision failure, stale result rejection, component body coherence and task teardown with focused regression coverage.
- [x] Complete the shared Engine API `all` build using the
  [build workflow](../Agents/BuildAndRun.md). Compile affected qualification
  fixtures; execute GPU/performance qualification only when its documented gate
  applies, and record execution omissions explicitly.
- [x] Update [StaticMesh building](../Runtime/Assets/StaticMeshBuilding.md),
  [asset compilation](../Runtime/Assets/AssetCompilation.md),
  [collision](../Runtime/Physics/Collision.md) and
  [rendering](../Runtime/Rendering/StaticMeshRendering.md) with implemented
  ownership, completion, invalidation and publication behavior.
- [x] Validate changed documentation and plan lifecycle metadata; record concrete
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
