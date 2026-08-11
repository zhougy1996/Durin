# Viewport Picking Contract Plan

Summary: Replace Actor-only viewport picking with one per-viewport semantic request, result, arbitration, and lifetime contract while preserving current StaticMesh selection behavior.

Last reviewed: 2026-08-11

Status: Archived
Completed: 2026-08-11

## Current Status

All stages and acceptance gates are complete. The public value contract,
per-viewport service, StaticMesh CPU oracle, single visualization arbitration
path, Select-mode completion application, deferred fake backend,
registration-cycle validation, cancellation, stale-target rejection, and
multi-viewport isolation are in place. No legacy Actor-only picking call site
remains.

Qualification used the `Win64-Debug-DurinEditor` Agent Build Profile. The
focused `ViewportTests` and `SplineTests` targets passed 64 and 18 tests
respectively, the full `all` build succeeded,
and the built editor completed a hidden-window Sandbox startup/Level lifecycle
smoke through its normal exit path. Interaction-focused native coverage
qualifies StaticMesh nearest selection, visualization identity/arbitration,
Ctrl snapshot behavior, blank completion, gizmo precedence, independent
viewports, Level reset, target retirement, and prepared snapshot reuse.
Changed-document validation and whitespace checks also pass. Viewport Editing
Architecture publishes the lasting contract and the parent roadmap records M1
completion with M2 ready next.

## Goal

One Scene Viewport submits a semantic picking request and receives a validated
semantic completion whose public shape is unchanged by later CPU acceleration
or GPU rasterization. The M1 implementation preserves current StaticMesh and
Select-mode behavior while making exact primitive/component identity,
deterministic hit arbitration, request lifetime, and optional deferred
completion explicit.

At completion:

- `FLevelEditorViewportClient` builds/reuses the view and delegates one request
  instead of owning mesh intersection and duplicate visualization queries;
- Select mode consumes one `FViewportPickResult` without knowing which backend
  produced it;
- geometry results retain `FPrimitiveSceneId`, Actor, and exact component even
  though ordinary geometry clicks continue to select the Actor in M1;
- visualizer hits retain their current component/sub-element selection
  behavior;
- every request is per viewport, ticketed, generation-checked, cancellable, and
  able to complete immediately or later; and
- a deterministic CPU StaticMesh reference backend and focused tests provide
  the oracle for the next skeletal and acceleration plans.

## Scope

- Public LevelEditor value types for request policy, request ticket, completion
  status, semantic hit kind, and semantic hit result.
- A per-`FLevelEditorViewportClient` picking service with request sequencing,
  viewport/Level generation, immediate completion, polling, supersession, and
  cancellation.
- A private built-in geometry-provider boundary and the first StaticMesh CPU
  reference provider.
- Request-local mapping from `FPrimitiveSceneId` to weak component/Actor
  identity, plus validation before a completion reaches selection.
- Migration of existing ray/box/triangle helpers and Level traversal out of
  `FLevelEditorViewportClient`.
- One cross-family resolver for the nearest scene-geometry candidate and the
  already prepared component-visualization candidate.
- Select-mode integration that preserves Actor replacement/Ctrl toggling,
  component/sub-element visualization selection, blank-click clearing, and
  gizmo input ownership.
- Deterministic test seams for an immediate CPU backend and a controllable
  deferred fake backend.
- Focused unit, integration, lifecycle, multi-viewport, and editor interaction
  validation.
- Publication of the lasting M1 contract in Viewport Editing Architecture.

## Non-Goals

- `DSkeletalMeshComponent` intersection, CPU skinning, pose bounds, or any
  skeletal selection behavior; those belong to M2
  `SkeletalViewportPicking`.
- Scene AABB trees, mesh BVHs, query batching, task parallelism, or performance
  optimizations; those belong to M3 `ViewportPickingSpatialAcceleration`.
- Renderer ID passes, GPU pick tokens, RHI textures, staging buffers, readback,
  fences, or Vulkan changes.
- Changing `FLevelEditorContext` as selection authority or redesigning edit
  modes, Details targeting, transactions, transform-gizmo targets, or the
  component visualizer registry.
- Making an ordinary mesh-surface click select the component in Details. M1
  retains current Actor-selection behavior while preserving the component in
  the hit result for later consumers.
- Bone, vertex, triangle, material-section, instance, socket, UV, or arbitrary
  topology selection.
- Physics collision, gameplay raycasting, navigation queries, or a universal
  runtime spatial-query service.
- A public plugin/provider registration API. M1 and M2 use private built-in
  provider composition; public registration requires a concrete external
  contributor and reviewed lifetime semantics.
- Persisting requests, results, or `FPrimitiveSceneId` into Level or workspace
  files.

## Design Decisions and Invariants

### Ownership and module boundary

- `FLevelEditorContext` remains the sole selection authority. Picking returns
  value facts and never selects an Actor, changes a component, toggles Ctrl
  state, starts a transaction, or activates a mode.
- Each `FLevelEditorViewportClient` owns exactly one
  `FViewportPickingService`. This matches the existing one-client-per-panel
  view snapshot, camera, hover, and gizmo lifetime and prevents global pending
  request state.
- Public semantic value types live in a focused LevelEditor public header so
  edit modes and native tests do not depend on a private backend class.
  Service, provider, intersection, and test-injection details remain in the
  LevelEditor viewport implementation boundary.
- The default service composes a private ordered list of built-in geometry
  providers. StaticMesh is the only M1 provider; M2 adds SkeletalMesh without a
  cast or branch in `FLevelEditorViewportClient` or Select mode.

### Request API is ticketed and optionally deferred

- Submission always returns a non-zero request ticket and a status. The CPU
  reference backend normally returns `Completed` with an optional semantic hit
  in the same call; a future backend may return `Pending` without changing the
  Select-mode call shape.
- The service exposes polling/draining by ticket, explicit cancellation, and
  invalidation. There is no callback into an edit mode because mode instances
  may exit before an asynchronous result completes.
- Select mode retains the ticket and the selection intent captured at click
  time, including Ctrl state. It applies an immediate completion through the
  same path used for a polled completion and cancels its ticket on forced exit
  or replacement.
- One newer Select click supersedes the older outstanding Select click for the
  same viewport. Hover and other future purposes use separate purpose slots;
  M1 does not route hover through the new service unless required to eliminate
  duplicate visualization collection.
- Pending support is proven with an injected fake backend. Production M1 does
  not introduce background work, tasks, renderer commands, or frame latency.

### Request and result format

- A request captures a monotonically increasing request ID, viewport-client
  generation, weak Level identity, immutable `FSceneView`, viewport pixel
  position, requested hit layers, required precision, and purpose.
- Input position is in output-target pixel coordinates. The request rejects a
  position outside the `FSceneView` viewport rectangle and uses
  `SceneViewProjection::BuildViewportRay`; no backend independently reconstructs
  camera matrices or guesses widget/DPI coordinates.
- M1 defines at least `SceneGeometry` and `EditorVisualization` layers,
  `ActorComponentSurface` precision, and `ClickSelection` purpose. Enums are
  closed value types, not a backend registration protocol.
- A hit result contains hit kind, `FPrimitiveSceneId` when the source is a
  primitive, weak Actor/component identity, optional typed sub-element,
  non-negative finite world distance, source priority/depth policy, and a
  deterministic game-thread stable tie key.
- World position, normal, triangle, section, instance, and bone fields are not
  added speculatively. Later plans extend the result only when a selected
  consumer requires them.
- Empty space is a successful `Completed` request with no hit. Invalid input,
  unsupported precision, retired ownership, cancellation, and stale generation
  are distinguishable from an empty hit without exceptions.

### Identity and lifetime

- M1 captures a request-local target table on the game/editor thread while it
  enumerates the current Level. Each primitive entry contains
  `FPrimitiveSceneId`, weak component, weak owning Actor, and the request's
  Level/client generation. This is the authoritative resolver for that ticket.
- The target table is retained by the service, not copied into a backend result
  and not exposed to another thread. A future GPU backend transports only
  detached numeric tokens/IDs; the service resolves them through the retained
  game-thread table.
- The M1 table is rebuilt for each submitted scene-geometry request. M3 may
  replace discovery with an incrementally maintained spatial index without
  changing tickets, results, or Select mode.
- Before applying a completion, the service verifies the client generation,
  weak Level, Actor, component, ownership relationship, primitive ID, current
  registration, and hidden state relevant to the request. A destroyed,
  unregistered, reparented, hidden, or different-Level target produces
  `Invalidated`, not a hit on reused memory.
- Camera movement after submission does not reinterpret or automatically
  invalidate the request: the hit corresponds to the immutable clicked view.
  World/Level replacement, viewport reinitialization/close, a newer click, or
  mode exit does invalidate it.
- `FPrimitiveSceneId` remains process-local runtime identity and is never
  serialized or truncated.

### StaticMesh reference policy

- M1 preserves the current supported geometry set: visible Actors with
  registered `DStaticMeshComponent` entries whose mesh has valid CPU render
  data, valid local bounds, non-empty LOD0 positions/indices, and an invertible
  finite render matrix.
- The query remains LOD0, double-sided, bounds-then-triangle, and exact surface
  intersection. A bounds-only hit is not a geometry result; degenerate or
  invalid triangles are skipped.
- Distance comparison occurs in world space so non-uniform component scale
  cannot reorder candidates incorrectly.
- Missing data, singular transforms, invalid indices, non-finite values, and
  unsupported component families are skipped without failing other
  candidates.
- The StaticMesh provider owns all StaticMesh includes, casts, and CPU geometry
  access. Viewport client and Select mode include only semantic picking APIs.

### Hit collection and arbitration

- Transform-gizmo update remains before picking. If the gizmo is hovered or
  dragging, Select mode does not submit a request.
- The prepared visualization collector is hit-tested once at submission. The
  service never calls the collector again for the same request, and Select mode
  no longer asks separately for an Actor and a visualization.
- Invalid candidates are discarded first. A depth-independent visualization
  beats scene geometry. Otherwise the smallest finite world distance wins.
- Within the existing intersection epsilon, scene geometry beats a depth-tested
  visualization to preserve today's strict-less-than overlay replacement
  behavior. Remaining ties use higher semantic priority and then a stable key,
  never Level enumeration, provider container, or hash iteration order.
- The existing visualization collector remains responsible for choosing one
  visualization candidate from its lines/icons/boxes by shape tolerance,
  distance, and priority. M1 unifies that candidate with geometry; it does not
  redesign individual screen-shape hit math.
- An ordinary geometry winner causes the existing Actor select/toggle action
  and clears descendant selection through `FLevelEditorContext`. A
  visualization winner continues to select its exact component or sub-element.

### Failure and compatibility policy

- New APIs return explicit status and optional value results; invalid requests
  and stale completions do not log per-frame errors or assert in normal editor
  lifecycle paths.
- Temporary `PickActor`, `PickActorWithView`, or
  `PickVisualizationWithView` compatibility wrappers are allowed only while
  migrating focused tests and call sites inside a stage. They are removed from
  the production viewport client before M1 completion unless a named external
  call site is found and documented.
- Existing visualization hover rendering may retain its current focused API in
  M1, but it must reuse the prepared collector and may not recreate or
  double-dispatch visualization primitives as a side effect of click picking.
- The implementation must not retain raw Level, Actor, component, collector,
  or view references in a pending ticket.

## Current Foundations and Gaps

| Area | Foundation | M1 gap |
| --- | --- | --- |
| Input ownership | Scene Viewport normalizes navigation, click, Ctrl, focus, and mode input; gizmo consumes before Select picking | No request purpose, ticket, supersession, or pending completion state |
| View snapshot | `FLevelEditorViewportClient` prepares one `FSceneView` and one visualization collector for input/render reuse | Geometry and visualization APIs can be called separately and do not retain one semantic request identity |
| Projection | Shared scene-view projection builds viewport rays and projects visualizers | Public Actor-pick wrappers rebuild view state and result format does not record request/view generation |
| Static geometry | Local bounds/triangle helpers and LOD0 CPU data produce exact transformed closest hits | Helpers, type casts, Level scan, and winner state are embedded in the viewport client |
| Visualizations | Collector preserves weak Actor/component/element identity, depth independence, distance, and priority | Visualization is a separate result type and Select mode has special reconciliation logic |
| Selection | `FLevelEditorContext` enforces Actor/component/sub-element ownership and clearing | Select mode consumes Actor and visualization APIs instead of one semantic hit |
| Primitive identity | Registered primitive components have stable `FPrimitiveSceneId` | Geometry picking drops it and there is no request-local weak resolver/generation check |
| Tests | Projection, closest triangle, bounds rejection, visualizer identity/priority, shared snapshots, and selection flows have focused coverage | No semantic result, deterministic equal-hit, deferred completion, supersession, invalidation, or independent-viewport tests |

## Implementation Stages

### Stage 0: Freeze the M1 contract and migration map

Outcome: every M1 ownership, API, ordering, coordinate, lifetime, and
compatibility choice is expressed as one implementable header sketch and all
call sites are assigned to a migration stage.

Dependencies: parent Viewport Picking Roadmap M1 entry gate and the current
Viewport Editing Architecture contract.

- [x] Inventory every production and native-test call to `PickActor`,
  `PickActorWithView`, `PickVisualizationWithView`, picking-ray helpers, and
  prepared visualization snapshots; record any call site not covered by this
  plan.
- [x] Write the proposed public value-type signatures for request, ticket,
  status, completion, hit kind, layers, precision, purpose, and hit result
  before implementing storage or providers.
- [x] Write the private service/backend/provider signatures, including
  immediate completion, polling, cancellation, generation invalidation,
  request-local target lookup, and test injection.
- [x] Confirm the service owner is one `FLevelEditorViewportClient`, while the
  selection intent/ticket is owned by the requesting Select mode instance.
- [x] Confirm viewport-position normalization against the exact `FSceneView`
  rectangle used by prepared input/render paths, including outside-rect
  rejection.
- [x] Freeze the cross-family winner comparator, epsilon, stable tie key, and
  current geometry-versus-visualization behavior with table-driven examples.
- [x] Freeze request invalidation events and prove how current
  `InitializeForLevel`, mode exit, viewport destruction, and newer-click paths
  advance/cancel the relevant generation or ticket.
- [x] Select the temporary compatibility-wrapper lifetime and enumerate the
  exact tests/call sites that must migrate before wrappers are removed.

#### Acceptance Gate

- The header/service sketches can express immediate CPU and delayed fake
  completion without a callback into a mode or selection mutation in the
  service.
- One request coordinate convention, one hit comparator, one request-local
  identity table, and one invalidation matrix are selected; no conflicting
  alternatives remain in later stages.
- Every current picking call site is assigned to Stage 1 or Stage 2, and no
  skeletal, BVH, Renderer, RHI, or Vulkan work is needed to implement the
  selected contract.

### Stage 1: Implement semantic types and the StaticMesh reference backend

Outcome: the new service can submit, immediately complete, and validate a
scene-geometry request through a StaticMesh provider while producing exact
primitive/component identity.

Dependencies: Stage 0 acceptance gate.

- [x] Add the focused public semantic picking header and private
  service/provider implementation using the selected complete-or-pending
  contract.
- [x] Move ray-box and ray-triangle helpers out of
  `FLevelEditorViewportClient.cpp` into the reference geometry implementation
  without changing their finite/epsilon behavior.
- [x] Implement request-local Level enumeration and target-table capture for
  registered visible primitive components with stable `FPrimitiveSceneId`.
- [x] Implement the private StaticMesh provider with the frozen LOD0,
  double-sided, bounds-then-triangle, world-distance, invalid-data, and stable
  tie policies.
- [x] Implement ticket sequencing, immediate completion storage, polling,
  cancellation, client/Level generation, and semantic target validation.
- [x] Add focused value/comparator tests for empty completion, invalid request,
  outside-view request, cancellation, finite distance, exact component
  identity, primitive ID preservation, and deterministic ties.
- [x] Migrate the existing closest-triangle and bounds-only tests to assert the
  semantic result while preserving their original transformed-near/far
  behavior.

#### Acceptance Gate

- The CPU reference backend produces the same StaticMesh Actor winner as the
  legacy path and additionally returns the exact component and non-zero
  `FPrimitiveSceneId`.
- Invalid/singular/missing geometry cannot suppress another valid candidate,
  and equal-distance results do not depend on Level or provider enumeration
  order.
- Service unit tests demonstrate ticket creation, immediate completion,
  polling semantics, cancellation, and stale target rejection without any
  selection mutation.
- No StaticMesh include, cast, bounds loop, or triangle loop remains in the
  viewport client's picking implementation.

### Stage 2: Unify visualization arbitration and Select-mode application

Outcome: one click request resolves prepared visualizations and scene geometry
once, then applies the semantic completion through existing shared selection
rules.

Dependencies: Stage 1 acceptance gate and existing visualization snapshot
reuse.

- [x] Adapt the prepared collector's single visualization hit into a semantic
  candidate without losing weak Actor/component/element identity, distance,
  priority, or depth-independent policy.
- [x] Submit the geometry and prepared-visualization inputs through one service
  call and implement the frozen cross-family comparator.
- [x] Replace Select mode's separate Actor/visualization queries with one
  request ticket and one completion-application helper.
- [x] Preserve ordinary geometry Actor select/toggle, visualization
  component/sub-element selection, Ctrl snapshot, blank-click clearing, and
  gizmo consumption exactly.
- [x] Ensure a prepared visualization collector is hit-tested once per click
  and is neither regenerated nor retained by a pending request.
- [x] Remove migrated production compatibility wrappers and update remaining
  focused tests to semantic requests/results.
- [x] Add integration tests for depth-independent overlays, depth-tested
  overlay versus nearer/farther geometry, epsilon ties, component/sub-element
  identity, Ctrl selection, blank selection, and gizmo preemption.

#### Acceptance Gate

- Select mode contains no mesh-family cast, ray/triangle logic, duplicate
  visualization reconciliation, or backend-specific branch.
- One request yields one deterministic winner and current Actor/component/
  sub-element user behavior passes across geometry, camera/light icons, spline
  elements, blank space, and gizmo overlap.
- Visualization generation-count tests prove click picking reuses the prepared
  snapshot and performs one collector hit test.
- No obsolete Actor-only picking API remains unless a documented external call
  site forced a bounded compatibility seam during Stage 0.

### Stage 3: Prove deferred lifetime and per-viewport isolation

Outcome: the production CPU path remains immediate, while the same contract is
proven safe for future delayed completion through a deterministic fake backend.

Dependencies: Stage 2 acceptance gate.

- [x] Add a controllable fake backend that can hold, complete, fail, or reorder
  requests without tasks, sleeping, renderer commands, or GPU resources.
- [x] Prove a delayed completion applies the Ctrl/replace intent captured at
  submission rather than current input state.
- [x] Prove newer click supersession, explicit cancellation, Select-mode exit,
  forced mode exit, viewport-client reinitialization, Level/world replacement,
  component unregister/destruction/reparent, Actor hiding, and viewport
  destruction discard the old completion.
- [x] Prove camera movement alone does not reinterpret or invalidate a valid
  delayed result captured against the clicked view.
- [x] Prove two viewport clients have independent request sequences,
  generations, fake backends, prepared visualizations, and completion queues.
- [x] Add diagnostics sufficient to distinguish submitted, immediately
  completed, pending, empty, cancelled, invalidated, and failed requests in
  tests without per-frame logging.
- [x] Exercise repeated request/cancel/complete cycles under object collection
  and confirm no raw object or collector reference survives in pending state.

#### Acceptance Gate

- Every invalidation event has a focused deterministic test and cannot mutate
  `FLevelEditorContext` after the request becomes stale.
- Delayed and immediate completions use the same Select-mode application path
  and produce identical semantic selection for the same captured request.
- Multiple viewport instances cannot cancel, complete, or resolve one another's
  tickets.
- Pending service state contains only owned values, weak object identities, and
  backend handles; no raw Level/Actor/component/collector/view reference is
  retained.

### Stage 4: Qualify the editor contract and publish the lasting architecture

Outcome: the M1 contract is documented, regression-qualified, and ready to
serve as the unchanged entry boundary for skeletal picking.

Dependencies: Stages 1-3 acceptance gates.

- [x] Update Viewport Editing Architecture with semantic request/result,
  per-viewport service, exact identity, comparator, immediate/deferred
  lifecycle, and selection-application ownership.
- [x] Update this plan's Current Status, Last reviewed date, checklists, and
  evidence for every completed gate; record any selected deviation before
  qualification.
- [x] Run formatting/static checks and the smallest affected native
  `EngineTests` target following repository build/test guidance.
- [x] Run the required full `all` build because this is a user-visible editor
  interaction change.
- [x] Launch the editor from the same Agent Build Profile and smoke-test
  StaticMesh nearest selection, camera/light visualization selection, Ctrl
  toggling, blank clearing, gizmo manipulation, two viewports, and Level
  replacement without stale selection.
- [x] Review M2 `SkeletalViewportPicking` entry evidence and update the parent
  roadmap M1 status, link, and next milestone only after every M1 gate passes.

#### Acceptance Gate

- Focused native tests, full `all` build, and editor smoke pass from one Agent
  Build Profile with no stale completion or selection regression.
- Viewport Editing Architecture, implementation, tests, this plan, and the
  parent roadmap agree on ownership, API, ordering, failure, coordinate, and
  lifetime behavior.
- M2 can add a SkeletalMesh provider behind the existing service without
  changing Select mode, request/result types, comparator, or selection
  authority.
- All M1 Definition of Done items are evidenced and the plan is ready to be
  marked Completed.

## Validation Matrix

| Contract | Focused coverage | Required outcome |
| --- | --- | --- |
| Request values | Type/service unit coverage | Non-zero ticket, immutable view/position/policy, valid enum/flag handling, explicit empty/invalid/pending/completed/cancelled status |
| Projection coordinates | Viewport projection tests | Exact `FSceneView` rectangle, center/edge/outside behavior, finite ray, no widget/DPI reinterpretation in providers |
| StaticMesh reference | Viewport interaction tests | LOD0 exact surface, bounds rejection, nearest world distance, transformed/non-uniform/mirrored candidates, degenerate/invalid data, exact primitive/component identity |
| Deterministic ordering | Table-driven unit and interaction tests | Depth-independent overlay, depth-tested distance, epsilon geometry tie, semantic priority, stable final key independent of enumeration |
| Selection application | Edit-mode/context integration tests | Geometry Actor select/toggle, visualization component/element selection, blank clear, Ctrl snapshot, gizmo preemption, no backend mutation of context |
| Prepared visualization reuse | Viewport customization generation-count tests | One visualization snapshot and one hit test per click; no regeneration or retained collector reference |
| Lifetime | Deferred fake-backend tests | Newer request, cancel, mode exit, client reset/destruction, Level/world change, target retire/reparent/hide all reject stale completion |
| View stability | Deferred request tests | Camera motion does not alter captured hit; Level/view generation changes invalidate as selected |
| Multi-viewport | Two-client integration tests | Independent ticket sequence, generation, backend state, collector, completion, and selection intent |
| Object safety | Weak-reference and collection tests | No raw reflected-object reference outlives synchronous submit; invalid weak identities cannot resolve or mutate selection |
| Regression | Smallest affected native target | Existing viewport projection, navigation, visualization, spline, gizmo, Details selection, and StaticMesh picking coverage remains green |
| User-visible qualification | Repository full-build and runtime guidance | Successful full `all` build and verified editor smoke from the same Agent Build Profile |

## Definition of Done

- All required Stage 0-4 checklist items and acceptance gates are complete with
  recorded evidence.
- Select mode consumes one semantic completion and has no StaticMesh,
  SkeletalMesh, Renderer, RHI, or backend-specific logic.
- `FLevelEditorViewportClient` no longer owns StaticMesh intersection or an
  Actor-only picking result; it owns/delegates to one per-viewport service.
- StaticMesh reference picking preserves visible user behavior and adds exact
  primitive/component identity plus deterministic ties.
- Prepared visualization and geometry candidates resolve once through the
  documented comparator.
- Immediate and fake-deferred completions share one ticket, validation, and
  selection-application path; every lifecycle invalidation is tested.
- No production pending state retains raw Level, Actor, component, collector,
  or view references, and no render-thread ownership is introduced.
- Focused native validation, required full build, and editor smoke pass under
  repository guidance.
- Lasting behavior is published in Viewport Editing Architecture, the parent
  roadmap records M1 completion/readiness for M2, and this plan contains no
  unique implemented rule that belongs only in historical provenance.

## Deferred Follow-ups

- Current-pose SkeletalMesh CPU reference provider and animated correctness:
  M2 `SkeletalViewportPicking`.
- Incremental scene broad phase, per-LOD StaticMesh BVH, skeletal candidate
  reduction, diagnostics, and brute-force parity: M3
  `ViewportPickingSpatialAcceleration`.
- Non-blocking bounded-region RHI readback: conditional M4
  `AsynchronousTextureRegionReadback` after activation evidence.
- Renderer integer-ID pass and hybrid CPU/GPU arbitration: conditional M5
  `GPUViewportPicking` after activation evidence and M4.
- Persistent hover picking, material/section/instance/triangle identity,
  translucent surface policy, and external provider registration remain
  separately gated consumers rather than M1 extensions.
- A shared runtime spatial-query or collision contract requires a second
  concrete non-editor consumer and does not inherit viewport selection
  semantics automatically.

## Related Documentation

- [Viewport Picking Roadmap](../../../Roadmaps/Archive/2026-08/ViewportPicking.md)
- [Viewport Editing Architecture](../../../Editor/Architecture/ViewportEditing.md)
- [Scene Viewport Navigation](../../../Editor/Guides/SceneViewportNavigation.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Renderer Scene Representation](../../../Runtime/Rendering/SceneRepresentation.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Tests](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Editor/LevelEditor/Public/LevelEditorSelection.h`
- `Engine/Source/Editor/LevelEditor/Public/LevelEditorViewportEditing.h`
- `Engine/Source/Editor/LevelEditor/Public/LevelEditorCustomizations.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportEditing.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/LevelEditorCustomizations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorContext.h`
- `Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorContext.cpp`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/StaticMeshComponent.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/RenderCore/Public/SceneViewProjection.h`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportProjectionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportInteractionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportFoundationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportCustomizationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/DetailsSelectionTests.cpp`
