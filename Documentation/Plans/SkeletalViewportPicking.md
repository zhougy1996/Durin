# Skeletal Viewport Picking Plan

Summary: Add deterministic current-pose CPU surface picking for `DSkeletalMeshComponent` behind the established semantic viewport picking service.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

M2 is complete. The private Level Editor reference backend now registers a
current-pose SkeletalMesh provider beside the retained StaticMesh provider.
It snapshots one immutable `FSkeletalPosePalette`, validates compatibility and
all CPU shapes, resolves skeleton bone indices through the mesh palette, rejects
against current pose bounds, skins each referenced LOD0 vertex once, and tests
all indexed triangles double-sided. Both families compete through the existing
world-distance and stable-key comparator without changes to Select mode, the
viewport client, public request/result types, Renderer state, or selection
ownership.

The frozen request-wide limits are 250,000 skinned vertices and 500,000 tested
triangles. Exact-limit work is allowed; over-limit work fails the full geometry
completion without a partial winner. Private diagnostics distinguish
applicable/invalid skeletal targets, bounds rejection, budget failure, skinned
vertices, and tested triangles. Missing, incompatible, incomplete, malformed,
non-finite, or singular candidates remain component-local skips with no bind
pose or bounds-only fallback.

Qualification passed in the `windows-msvc-x64` Agent Build Profile with the
`Win64-Debug-DurinEditor` preset: `ViewportTests` 74/74,
`SkeletalAssetTests` 33/33, changed-document validation, a successful full
`all` build, and an 8-second startup smoke of the resulting `DurinEditor.exe`.
M3 `ViewportPickingSpatialAcceleration` is the next ready required milestone.

## Goal

Make an ordinary Level Editor click select the exact Actor and
`DSkeletalMeshComponent` whose current deformed LOD0 surface is closest to the
request ray, while preserving the M1 semantic contract and deterministic
competition with StaticMesh and editor-visualization candidates.

## Scope

- A private built-in SkeletalMesh geometry provider owned by the Level Editor
  reference picking backend.
- One immutable current-pose snapshot per candidate component and request.
- Pose-derived local-bounds rejection, value-only CPU skinning, and exact
  double-sided ray/triangle intersection for LOD0.
- Palette-bone-to-palette-slot resolution for up to the existing supported
  per-vertex influence count.
- World-space hit distance and the existing stable cross-component winner
  ordering under translation, rotation, non-uniform scale, and mirroring.
- Explicit handling for missing assets, malformed CPU geometry, singular
  component transforms, incompatible or incomplete poses, non-finite values,
  and reference-query budget exhaustion.
- Focused native coverage, lasting viewport architecture documentation, parent
  roadmap status, full editor build, and runtime startup qualification.

## Non-Goals

- Bone, socket, vertex, triangle, section, material, animation-track, or other
  skeletal sub-element selection.
- A scene broad phase, mesh BVH, bind-pose BVH, refitted BVH, influence-bound
  hierarchy, pose cache, or persistent skinned-vertex cache; those require M3
  acceleration evidence and parity against this reference path.
- A GPU ID pass, RHI readback, hardware ray tracing, or render-thread query.
- Exact parity with material culling, masked discard, translucency, or the
  renderer-selected visible LOD. M2 follows the existing M1 CPU oracle policy;
  visible-surface GPU parity remains conditional M5 work.
- Changes to `FLevelEditorContext`, Select mode, gizmo/visualizer arbitration,
  Details targeting, transaction ownership, or the public semantic picking
  contract.
- A general runtime collision, physics, or scene-query API.

## Design Decisions and Invariants

### Ownership and thread boundary

- `FViewportPickingService` remains owned by one
  `FLevelEditorViewportClient`; the built-in reference backend remains its
  private implementation detail.
- The SkeletalMesh provider runs synchronously on the editor/game thread in M2.
  It may dereference the request-local weak component only during `Submit` and
  retains no Actor, component, Level, mesh, view, or render-data pointer after
  returning.
- The provider acquires one `shared_ptr<const FSkeletalPosePalette>` from the
  component and uses that immutable value for the entire component query. It
  never asks the animation instance to evaluate another pose and never reads
  the renderer's proxy or scene info.
- The production reference backend remains immediate. Existing pending-ticket
  and stale-result rules are unchanged and remain proven by M1's fake backend.

### Precision, LOD, and facing policy

- M2 supports only the existing
  `EViewportPickPrecision::ActorComponentSurface` precision and returns no
  skeletal sub-element metadata.
- The provider queries `FSkeletalMeshRenderData::LODIndex == 0`, which is the
  only currently published skeletal render-data LOD. A missing or different
  LOD is unsupported and produces no candidate.
- Every indexed triangle in every valid LOD0 section participates. Intersection
  is double-sided, matching the M1 StaticMesh reference oracle; material
  rasterization and cull state do not silently alter CPU selection semantics.
- The pose's current finite `LocalBounds` is the only broad-phase bound. Bind
  bounds and reference-pose triangles are never used as a hit fallback.

### Pose and geometry validity

- A usable pose has a non-zero revision, matching non-empty skeleton
  compatibility identity, one finite matrix for every render-data palette
  entry, and finite valid local bounds. Pose and mesh compatibility identities
  must match exactly.
- CPU position, influence, index, palette, and section shapes must agree before
  any triangle result is accepted. Every used influence must resolve its
  skeleton bone index to exactly one palette slot and have a finite positive
  weight; the used weights must satisfy the asset contract.
- A component with missing, incomplete, incompatible, or non-finite data is
  skipped atomically. It cannot contribute a partial triangle hit, suppress a
  valid StaticMesh or SkeletalMesh candidate, or fall back to bind pose or a
  bounds-only Actor hit.
- Singular or non-finite component transforms are skipped consistently with
  StaticMesh picking. Hidden Actors, unregistered components, retired primitive
  IDs, and registration-generation mismatches remain rejected by the M1
  target-table and completion validation.

### Deformation and distance

- Each bind-space CPU position is transformed by every used palette matrix and
  accumulated by its canonical influence weight. Bone indices stored in
  `FSkeletalMeshVertexInfluences` are skeleton bone indices, so the provider
  builds one request-local bone-to-palette-slot lookup instead of treating them
  as dense palette indices.
- A bounds-surviving component skins each referenced LOD0 vertex at most once
  into an owned request-local array, then tests indexed triangles with the M1
  finite/degenerate epsilon behavior.
- The ray is transformed into component local space for bounds and triangle
  work. A local hit is transformed back to world space and compared by finite
  non-negative distance from the captured request ray origin. This preserves
  correct ordering under non-uniform scale and mirroring.
- StaticMesh, SkeletalMesh, and depth-tested visualization candidates continue
  to use `IsViewportPickHitPreferred`; provider enumeration order is never the
  final tie-break.

### Bounded reference-query failure

- M2 uses deterministic work counts, not elapsed wall-clock time. The budget
  separately limits total vertices skinned and total triangles tested across
  all valid skeletal components whose current pose bounds intersect the ray.
- Stage 0 records concrete request-wide constants after measuring small,
  representative, and upper-bound fixtures in the Agent Build Profile. Counts
  use overflow-checked integer arithmetic and are charged before a component's
  deformation/triangle loop starts.
- Invalid data is a component-local non-candidate. Budget exhaustion is a
  request-level `Failed` geometry completion because returning a farther static
  or skeletal hit while an untested current-pose candidate may be nearer would
  violate the semantic contract.
- A failed completion has no hit and Select mode does not mutate selection. No
  timeout, partial winner, Actor-bounds approximation, or per-frame error log
  is introduced. Test-visible counters/reasons distinguish bounds rejection,
  invalid pose/data, budget failure, vertices skinned, and triangles tested
  without expanding the public picking result.

## Current Foundations and Gaps

| Area | Existing foundation | M2 gap |
| --- | --- | --- |
| Semantic selection | One request/result, exact primitive/component identity, selection application above picking | Skeletal geometry does not produce a semantic candidate |
| Provider boundary | Private ordered geometry providers behind an immediate-or-pending backend | Only the StaticMesh LOD0 provider is registered; provider outcome cannot yet report budget failure/diagnostics |
| Skeletal CPU geometry | LOD0 positions, indices, sections, canonical influences, palette bones, and CPU-access render data | No current-pose vertex deformation or ray/triangle provider |
| Pose publication | Component atomically exposes an immutable palette with revision, compatibility identity, matrices, and animated local bounds | Picking does not snapshot or validate the pose |
| Bounds | Pose evaluation publishes current conservative local bounds; component provides render matrix | Picking does not reject skeletal candidates through current bounds |
| Ordering | M1 compares geometry by world distance and stable primitive identity | No mixed StaticMesh/SkeletalMesh or animated equal-hit coverage |
| Cost control | M1 StaticMesh brute-force path is a correctness oracle | Skeletal per-click skinning and triangles need deterministic request-wide limits and failure evidence |
| Tests | Skeletal asset, animation, scene-lifecycle, and GPU deformation fixtures; viewport semantic tests | No viewport fixture proves current-pose surface selection or failure policy |

## Implementation Stages

### Stage 0: Freeze the skeletal query contract and budget

Outcome: the deformation equation, validity rules, provider failure protocol,
fixtures, and numeric reference-query limits are recorded before production
query code is added.

Dependencies: completed M1 Viewport Picking Contract and current skeletal
render-data/pose-publication contracts.

- [x] Inventory the exact CPU geometry, influence, palette, compatibility,
  pose, bounds, and component-transform APIs used by the provider; record any
  lifetime or indexing mismatch that would require Runtime Engine work.
- [x] Freeze a private provider outcome that distinguishes not-applicable,
  valid miss/hit, invalid-component skip, and request-level failure while
  preserving the existing backend completion contract.
- [x] Write the reference deformation equation and bone-to-palette-slot mapping
  against non-contiguous palette fixtures; prove it matches existing pose and
  rendering matrix conventions.
- [x] Freeze LOD0, all-section, double-sided, current-pose-only, world-distance,
  epsilon, and stable-tie policies as stated above.
- [x] Add or identify deterministic viewport test builders for a one-bone
  triangle, a non-contiguous palette, mixed influences, animated translation,
  multiple components, and mixed StaticMesh/SkeletalMesh scenes.
- [x] Measure vertices and triangles for representative fixtures and record the
  request-wide maximum skinned-vertex and tested-triangle constants plus their
  over-budget status/counter expectations.
- [x] Confirm no public request/result field, Select-mode branch, Renderer API,
  or general runtime query interface is required by the frozen design.

#### Acceptance Gate

- One value-only equation maps bind positions and skeleton bone indices through
  the current mesh-palette-aligned pose, including non-contiguous palettes and
  multiple influences.
- Exact pose/data validity, LOD, facing, bounds, distance, invalid-component,
  and over-budget behavior has one selected outcome each.
- Numeric request-wide work limits and deterministic fixtures are recorded;
  implementation cannot start with an unbounded loop or wall-clock cutoff.
- The design fits behind the existing service and preserves M1 public APIs,
  selection ownership, comparator, tickets, and invalidation behavior.

### Stage 1: Implement the current-pose SkeletalMesh reference provider

Outcome: the built-in reference backend can return an exact semantic hit for a
valid current-pose `DSkeletalMeshComponent` without exposing mesh-family logic
to viewport clients or modes.

Dependencies: Stage 0 acceptance gate.

- [x] Add the private provider outcome/query context needed for deterministic
  work accounting and request-level failure propagation; keep StaticMesh
  behavior and ordering unchanged.
- [x] Register a built-in SkeletalMesh provider alongside the StaticMesh
  provider without relying on provider order for winner selection.
- [x] Acquire and validate one mesh render-data/pose snapshot, build the
  request-local bone-to-palette lookup, and reject invalid shapes atomically.
- [x] Transform the captured ray to component local space and reject it against
  the current pose bounds before charging deformation/triangle work.
- [x] Skin each referenced LOD0 vertex once using finite weighted palette
  transforms, then run exact double-sided indexed triangle intersection and
  convert the closest local hit to world distance.
- [x] Enforce the frozen request-wide caps with overflow-safe counts and return
  `Failed` without a partial winner when a bounds-surviving workload exceeds
  them.
- [x] Add test-visible diagnostics for applicable targets, pose/data skips,
  bounds rejects, budget failures, skinned vertices, and tested triangles
  without per-click logging or public semantic-result expansion.
- [x] Add focused provider/service tests for reference pose, non-contiguous
  palette lookup, multiple influences, back faces, degenerates, malformed
  indices/influences, missing/incompatible/incomplete/non-finite pose, singular
  transforms, and exact Actor/component/primitive identity.

#### Acceptance Gate

- A valid skeletal LOD0 triangle produces the exact component and non-zero
  primitive identity through the unchanged semantic completion path.
- Current pose matrices, rather than bind-pose positions or render-thread
  state, determine the hit; a missing or invalid pose cannot produce a partial
  or approximate hit.
- Bounds rejection performs no vertex skinning or triangle testing, and
  at/over-budget fixtures yield the frozen deterministic status and counters.
- StaticMesh picking tests remain unchanged and green; no SkeletalMesh include,
  cast, deformation, or failure branch appears in Select mode or
  `FLevelEditorViewportClient`.

### Stage 2: Prove animated competition and editor selection behavior

Outcome: current-pose SkeletalMesh surfaces participate correctly in mixed
viewport scenes and ordinary selection across pose, transform, visibility, and
lifetime changes.

Dependencies: Stage 1 acceptance gate and M1 selection integration coverage.

- [x] Add a pose-revision sequence where animation moves geometry into the ray,
  out of the ray, and back again across separate requests.
- [x] Add a bind-pose-only intersection whose current pose has moved away and
  prove the click is blank or resolves another valid candidate rather than the
  bind surface.
- [x] Prove nearer/farther StaticMesh and SkeletalMesh candidates compete by
  the same world-space distance under both enumeration orders.
- [x] Prove equal-distance mixed-family candidates use the existing stable
  primitive tie-break and do not depend on provider or Level order.
- [x] Cover translated, rotated, non-uniformly scaled, mirrored, and singular
  skeletal components, including finite world-distance ordering.
- [x] Cover multiple skeletal components on one Actor and across Actors,
  hidden Actors, unregister/re-register, destruction, and Level/client reset
  with exact component identity and no stale selection.
- [x] Drive the result through the existing Select-mode immediate-completion
  helper and prove replace, Ctrl toggle, blank clearing, visualization
  arbitration, and gizmo preemption need no skeletal-specific branch.
- [x] Prove invalid skeletal components remain non-candidates while valid
  static/skeletal candidates still resolve, and prove budget failure leaves the
  current selection unchanged instead of applying a partial winner.

#### Acceptance Gate

- Animation can move a selectable surface into and out of the captured ray;
  bind-pose-only hits are rejected.
- Static and skeletal hits share one finite world-distance and stable-tie rule
  across transforms, multiple components, and enumeration orders.
- Existing selection, visualization, gizmo, Ctrl, blank-click, lifetime, and
  per-viewport behavior remains backend-independent.
- Every roadmap M2 exit case has focused deterministic native coverage.

### Stage 3: Qualify M2 and publish the lasting contract

Outcome: current-pose skeletal selection is documented, regression-qualified,
and becomes the reference truth required by M3 spatial acceleration.

Dependencies: Stages 1-2 acceptance gates.

- [x] Update Viewport Editing Architecture with current-pose skeletal provider
  ownership, LOD/facing policy, pose validity, deformation/distance semantics,
  and bounded failure behavior.
- [x] Update this plan's Current Status, Last reviewed date, checklists, and
  evidence for every completed gate; record any selected deviation before
  qualification.
- [x] Update the Viewport Picking Roadmap to mark M2 complete, link this plan,
  and identify M3 `ViewportPickingSpatialAcceleration` as the next ready
  required milestone.
- [x] Run documentation validation and the smallest affected native test target
  or targets under repository guidance; preserve M1 viewport and skeletal
  animation/asset coverage relevant to any shared helper changed by M2.
- [x] Run the required successful full `all` build because skeletal selection
  is a user-visible editor change.
- [x] Launch the editor from the same Agent Build Profile and complete a runtime
  startup smoke; use focused native interaction coverage as the deterministic
  evidence for animated surface selection when no stable repository demo Level
  contains a skeletal fixture.
- [x] Review the verified executable and plan/roadmap/architecture/code/test
  agreement before marking this plan Completed.

#### Acceptance Gate

- Focused native tests, documentation validation, full `all` build, and editor
  runtime startup pass from the required profiles.
- Implementation, tests, Viewport Editing Architecture, this plan, and the
  parent roadmap agree on current-pose ownership, LOD, facing, validity,
  distance, budget, and failure semantics.
- The verified editor executable supports ordinary current-pose skeletal
  Actor/component selection without public-contract or Select-mode changes.
- M3 can compare acceleration results against retained StaticMesh and
  SkeletalMesh reference behavior with deterministic fixtures and work
  diagnostics.

## Validation Matrix

| Contract | Focused coverage | Required outcome |
| --- | --- | --- |
| Semantic identity | Viewport service/provider tests | Exact Actor, `DSkeletalMeshComponent`, non-zero primitive ID, registration generation, and stable key |
| Pose snapshot | Pose-revision interaction tests | One immutable current palette per component request; later animation affects only later requests |
| Deformation | One-bone, non-contiguous-palette, and mixed-influence fixtures | CPU positions match the declared weighted palette transform and remain finite |
| Current surface | Bind/current displacement fixtures | Current pose can add/remove a hit; bind-pose-only surface never wins |
| Bounds | Animated local-bounds tests and diagnostics | Ray miss performs zero skin/triangle work; valid moved surface remains inside the tested current bound |
| Triangle precision | Front/back, closest, degenerate, and malformed geometry tests | LOD0 all-section double-sided exact hit; invalid triangles cannot suppress valid candidates |
| Transforms | Translation, rotation, non-uniform scale, mirroring, singular matrix | Finite world distance is correct; singular/non-finite candidates are skipped |
| Mixed ordering | Static/skeletal near, far, equal, and reversed enumeration | Same M1 distance epsilon and stable tie-break independent of provider/Level order |
| Invalid pose/data | Missing, incompatible, incomplete, non-finite, invalid influence/index | Component contributes no partial/bind/bounds hit; another valid candidate can win |
| Work budget | Below, exact-limit, over-limit, and multi-component fixtures | Deterministic counts; over-budget completion is `Failed`, carries no hit, and does not mutate selection |
| Selection behavior | Select-mode integration | Replace, Ctrl toggle, blank clear, visualization priority, and gizmo preemption require no skeletal branch |
| Lifetime | Hide, unregister/re-register, destroy, Level/client reset | Retired or stale skeletal identity cannot resolve or mutate selection |
| Regression | Smallest affected native targets | Existing StaticMesh, visualization, viewport lifetime, skeletal asset, and pose behavior remains green |
| User-visible qualification | Full build and runtime guidance | Successful `all` build and verified editor executable from the same Agent Build Profile |

## Definition of Done

- All Stage 0-3 checklist items and acceptance gates are complete with recorded
  evidence.
- Ordinary viewport clicks return exact current-pose skeletal Actor/component
  surface hits through the unchanged M1 semantic request, ticket, completion,
  comparator, and selection-application path.
- LOD0, double-sided, current-pose-only, bone-to-palette mapping, validity,
  world-distance, and failure behavior match the lasting documented contract.
- Missing or invalid pose/data never falls back to bind pose or bounds-only
  selection, and budget exhaustion never returns a partial winner.
- StaticMesh and SkeletalMesh candidates order deterministically across
  animation, transforms, multiple components, and enumeration order.
- No SkeletalMesh cast or backend branch exists in Select mode or
  `FLevelEditorViewportClient`; no Renderer/RHI state or raw reflected object is
  retained by a request.
- Focused native validation, documentation validation, required full build, and
  editor runtime startup pass under repository guidance.
- Lasting M2 behavior is published in Viewport Editing Architecture; the parent
  roadmap marks M2 complete and M3 ready; this plan is evidence-complete.

## Deferred Follow-ups

- Scene-level broad phase, immutable StaticMesh LOD acceleration, skeletal
  candidate reduction, reference/accelerated parity, and performance counters:
  M3 `ViewportPickingSpatialAcceleration`.
- Renderer-visible LOD, material culling, masked/translucent policy, and
  current-pose ID rendering: conditional M5 `GPUViewportPicking` after accepted
  activation evidence and asynchronous readback support.
- Bone, socket, section, material, vertex, triangle, and animation-editor
  selection remain separate consumer-driven plans.
- Raising or replacing the M2 reference-query budget requires recorded workload
  evidence and must preserve deterministic failure or prove parity with M3
  acceleration.

## Related Documentation

- [Viewport Picking Roadmap](../Roadmaps/ViewportPicking.md)
- [Viewport Picking Contract Plan](ViewportPickingContract.md)
- [Viewport Editing Architecture](../Editor/Architecture/ViewportEditing.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Skeletal Animation Playback](../Runtime/Animation/SkeletalAnimationPlayback.md)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Public/LevelEditorViewportPicking.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingService.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingService.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportEditing.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/SkeletalMeshComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/SkeletalMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Animation/SkeletalAnimation.h`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalMesh.h`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalMeshResources.h`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalMeshResources.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportPickingContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportInteractionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAnimationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp`
