# Terrain Scene Activation Performance Plan

Summary: Remove repeated Terrain payload validation and derived-data construction from synchronous level activation, then stage collision publication and loading behind explicit lifecycle and performance contracts.

Last reviewed: 2026-08-14

Status: Archived
Completed: 2026-08-14

## Current Status

The initial baseline called `LoadAsset` and then synchronously activated every
actor and component. Terrain registration performed render-proxy construction,
editor-picking bounds construction, and collision heightfield construction on
that path. The immutable heightmap payload was also semantically rebuilt by
`IsValid()` at several trusted runtime call sites, including render preparation.

The repository fixture named `TestTerrain_1025` currently resolves to a 513x513
RAW16 source. Even at that size, one activation repeats full sample/hierarchy
validation and patch LOD-error derivation several times before the first useful
frame. Collision hashing, sample copying, and heightfield-tree construction added
another synchronous full-heightmap pass.

Stage 1 is complete. Published immutable payloads now expose a non-allocating
layout check while exact `IsValid()` reconstruction remains at untrusted
boundaries. Terrain components retain one patch/LOD/bounds generation keyed by
payload revision, spacing, and height range; render-proxy and editor-picking
requests reuse it. The activation call path therefore performs no canonical
payload reconstruction and builds identical patch metadata only once.

Validation completed on the `Win64-Debug-DurinEditor` profile: focused
`TerrainHeightmapTests` and `TerrainRenderPrimitiveTests`, the complete
`@terrain` domain including Cook and Vulkan targets, the all-plan validator,
and a full `all` build. Stage 2 is complete and Stage 3 is now in progress.
Editor and Preview worlds keep Terrain collision dormant during registration;
explicit debug/tool requests build asynchronously. PlayInEditor and Game worlds
schedule immutable worker construction and cross a required readiness barrier
before BeginPlay. Publication revalidates object handles, registration
generation, asset and collision revisions, settings, and cancellation before
touching the physics scene.

The focused Win64 Debug characterization recorded 513x513 cold hash/match/copy/
tree/insert phases of 11,805/30/217/7,258/122 us and 1025x1025 phases of
49,140/8/746/28,404/81 us. Warm exact matches were 53 us and 238 us
respectively. These are diagnostic evidence, not latency gates. Stage 3 is also
complete: uncooked PostLoad exposes Loading while
coalesced worker work performs DDC/source recovery, and GameThread publication
re-enters the ordinary Terrain revision path. Stage 4 is complete. On the GTX
1060 Debug Vulkan profile, the 1025x1025 cold
first frame was 87.11 ms CPU, followed by 14.94 ms median / 15.60 ms p95 warm
CPU and 1.043 ms warm GPU p95. The cold resource phases were 1.25 ms height,
6.79 ms topology, 3.30 ms shader, and 14.46 ms pipeline. These measurements do
not justify a warm-up queue; retained generation/key caches already make an
unchanged reopen creation-free, while device invalidation reconstructs on
demand. Stage 5 completed the Debug/Release editor and game qualification
matrix, including Terrain and physics domains, explicit renderer and physics
qualification targets, full ordinary native-test aggregates, full builds, and
bounded process startup/gameplay/task-system/shutdown smokes.

## Goal

Make opening and reopening a finite Terrain level responsive and predictably
bounded, while retaining exact height samples, deterministic patch LOD metadata,
editor picking, collision query correctness, and asset corruption detection.

## Scope

- Uncooked Terrain heightmap load and DDC-hit/miss behavior.
- Terrain component registration during editor and runtime level activation.
- Canonical payload validation boundaries and trusted immutable consumers.
- Terrain patch, LOD-error, bounds, and collision derived-data ownership.
- Main-thread, CPU-worker, render-thread, and physics-scene publication ordering.
- Focused timing diagnostics and repeatable activation performance gates.

## Non-Goals

- Heightmap streaming, world partition, virtual texturing, or unbounded Terrain.
- Changing height samples, patch dimensions, LOD selection, stitching, materials,
  editor picking semantics, or collision query results.
- Increasing the 1025x1025 render/collision ceiling.
- Making the asset package or mutable reflected object graph generally async.
- Hiding incomplete collision behind an undocumented query failure.

## Design Decisions and Invariants

### Canonical validation belongs at trust boundaries

- Imported, decoded, deserialized, and externally supplied payloads receive full
  canonical validation before publication.
- A payload published by `DTerrainHeightmap` is immutable and canonical for its
  complete shared lifetime. Trusted render, component, and picking consumers use
  a non-allocating layout check and may not rebuild the canonical hierarchy.
- The existing semantic `IsValid()` contract remains exact for untrusted callers,
  serialization, build products, thumbnails, and tests.
- Corrupt DDC or cooked bytes are rejected before they can become a Ready asset.

### Render-derived metadata has one component-owned generation

- Patch descriptors, LOD errors, and local bounds are derived from payload
  identity/revision plus spacing and height-range parameters.
- A component retains one immutable derived generation and reuses it for render
  proxy creation and editor picking bounds.
- Any key change publishes a fully rebuilt candidate atomically on the owning
  thread. Failure retains no partially initialized generation.
- Render proxies may copy bounded descriptors initially; cross-thread shared
  ownership is considered only after measurement shows the copy matters.

### Collision publication must become explicit before it becomes asynchronous

- Stage 1 retains synchronous collision construction to minimize behavioral risk.
- The editor authoring world retains its PhysicsScene but does not eagerly create
  Terrain bodies when a level or component is registered. Render, direct
  heightmap picking, selection, details, and transform editing do not require a
  Terrain physics body.
- Editor Terrain collision begins in `Dormant`. A tool that explicitly requires
  World collision, collision visualization, or physics debugging requests an
  asynchronous build and observes `Building`, `Ready`, or `Failed`; it may not
  trigger a hidden synchronous build on the UI thread.
- Play/Simulate schedules Terrain collision builds as soon as the target level is
  selected and must cross a readiness barrier before `BeginPlay` or resumed play.
  A failed required build rejects the transition with an actionable diagnostic;
  gameplay never begins with silently missing required Terrain collision.
- Heightmap, spacing, height-range, or collision-setting edits immediately remove
  the published Terrain body, cancel its request, and return editor state to
  `Dormant`. Stale collision is never retained against changed visible geometry.
- A later worker build captures immutable payload and scalar settings, never a
  mutable component or world pointer.
- Completion publishes only when component registration generation, heightmap
  revision, collision revision, and settings still match the request.
- Cancellation, level replacement, component destruction, and engine shutdown
  discard stale completion without touching the retired physics scene.
- Primitive registration and physics-state construction must be decoupled through
  an explicit creation policy rather than a Terrain-only exception inside
  `DPrimitiveComponent::OnRegister()`. Terrain selects editor `OnDemand` and
  gameplay `DeferredRequired`; cheap primitive shapes may retain `Eager`.

### Performance is an acceptance contract

- Instrument DDC load/rebuild, canonical validation, patch derivation, collision
  build, component registration, render upload, and total activation separately.
- Cold DDC, warm DDC, first open, reopen, collision enabled/disabled, Debug, and
  Release are reported independently.
- Timing gates use named fixtures and repeated samples; no wall-clock assertion is
  added to ordinary unit tests.

## Current Foundations and Gaps

| Area | Foundation | Gap |
| --- | --- | --- |
| Height payload | Deterministic samples and min/max hierarchy; strict decode | `IsValid()` rebuilds and allocates the entire canonical payload |
| Render metadata | Deterministic 64x64 patches and bounded LOD arrays | Render and picking independently recompute identical data |
| Collision | Immutable cached heightfield geometry and revision facts | First registration builds it synchronously; global cache is weak-only |
| Asset loading | DDC-first authoring policy with exact source fallback | Both hit deserialization and miss rebuild are synchronous and uninstrumented |
| Rendering | Height upload occurs on the render thread and is revision-keyed | First-use upload/resource creation is not separated from activation evidence |
| Diagnostics | Collision retained/peak facts and renderer counters exist | No activation phase timings or cache-hit facts |

## Implementation Stages

### Stage 1: Remove repeated trusted-path reconstruction

- [x] Add a non-allocating heightmap payload layout check while preserving exact
  semantic `IsValid()` behavior at untrusted boundaries.
- [x] Route Terrain component and render-thread trusted consumers through the
  layout check after canonical publication.
- [x] Cache patch descriptors, LOD errors, and local bounds by payload generation
  and component scalar settings.
- [x] Reuse the cached generation for scene-proxy construction and editor picking
  bounds, with deterministic invalidation after heightmap or property changes.
- [x] Add focused tests for layout rejection, canonical validation retention,
  cache-equivalent render/picking results, and parameter invalidation.

#### Acceptance Gate

- Existing Terrain heightmap, primitive, picking, renderer, and collision tests
  retain identical observable results.
- Trusted runtime paths perform no canonical payload reconstruction.
- One unchanged component builds patch/LOD metadata at most once per generation,
  independent of render-proxy and picking-bound request order.
- Focused build and tests pass under the repository workflows.

### Stage 2: Decouple editor collision and implement asynchronous play publication

- [x] Measure collision hashing, matching, copying, tree construction, and physics
  scene insertion separately on the 513x513 and 1025x1025 fixtures.
- [x] Select and document the incomplete-collision policy for editor activation,
  runtime level transitions, BeginPlay, and direct synchronous queries.
- [x] Add explicit `Eager`, `OnDemand`, and `DeferredRequired` physics-state
  creation policies at the primitive/world boundary; remove unconditional
  Terrain construction from component registration.
- [x] Keep ordinary editor Terrain collision `Dormant`; route exact Terrain
  picking through the existing heightmap query and make collision-dependent
  tools request readiness explicitly.
- [x] Define an immutable build request and completion token containing component
  registration generation, asset/collision revisions, and scalar settings.
- [x] Build heightfield geometry through the CPU task system with bounded
  cancellation and no worker access to DObjects or physics scenes.
- [x] Publish completion on the owning thread only after token revalidation;
  discard stale, cancelled, failed, or shutdown completions deterministically.
- [x] Preserve the synchronous path where an explicit activation barrier or test
  requires collision readiness, without duplicating cache entries.
- [x] Add lifecycle tests for rapid level replacement, component destruction,
  heightmap reimport, property edits, cancellation, and shutdown.
- [x] Add world-mode tests proving editor registration performs no Terrain
  collision build, debug/tool requests transition `Dormant` through `Building`,
  and Play/Simulate cannot begin before required collision becomes Ready.

#### Acceptance Gate

- Editor level activation leaves Terrain collision `Dormant` and performs no
  heightfield hashing, copying, tree construction, or physics-scene insertion.
- Render, selection, transform editing, and exact Terrain picking are fully usable
  while collision is dormant.
- Play/Simulate begins only after every required Terrain body is Ready, and build
  failure rejects the transition rather than silently removing collision.
- No stale completion can publish into a replaced world or retired physics scene.
- Collision readiness and failure are observable and queries follow the frozen
  incomplete policy exactly.
- Existing deterministic collision results and revision facts remain equivalent.

### Stage 3: Stage uncooked heightmap load and derived-data recovery

- [x] Instrument DDC query/read/decode and source capture/decode/build/store.
- [x] Keep package/object graph creation on its owning thread while moving only
  immutable byte and payload work to CPU tasks.
- [x] Define activation behavior for referenced Terrain assets whose payload is
  loading, missing, corrupt, or rebuilding from source.
- [x] Coalesce concurrent requests for the same derived-data key and bound retained
  bytes, request count, cancellation, and failure diagnostics.
- [x] Publish one canonical payload generation and trigger render/collision
  derived generation through the ordinary revision path.
- [x] Add cold/warm DDC and corrupt/missing-source integration fixtures.

#### Acceptance Gate

- Warm DDC and source-recovery work are separately measurable and neither blocks
  UI progress beyond the frozen publication boundary.
- Concurrent references do not duplicate payload builds or exceed memory limits.
- Failed recovery leaves an actionable failed asset state without partial render
  or collision publication.

### Stage 4: First-frame render-resource characterization and bounded warm-up

- [x] Measure height texture upload, topology creation, shader/pipeline lookup,
  and first Terrain draw after activation.
- [x] Distinguish editor-visible open latency from render-thread first-frame hitch
  in diagnostics and qualification reports.
- [x] Reuse height and topology resources by immutable generation/key and remove
  only measured redundant creation.
- [x] If required, schedule a bounded warm-up that cannot stall unrelated views
  or retain resources after level replacement.
- [x] Add Vulkan first-use and reuse qualification cases with counter conservation.

#### Acceptance Gate

- Reopening an unchanged Terrain does not repeat avoidable topology or height
  resource preparation while a valid retained generation exists.
- First-frame work stays within the frozen render-thread and memory budgets.
- No warm-up changes draw results, resource lifetime, or device-loss behavior.

### Stage 5: Qualification and lasting contracts

- [x] Capture cold/warm, first-open/reopen, 513x513/1025x1025, collision on/off,
  Debug/Release measurements with adapter, build, resolution, and sample count.
- [x] Run focused Terrain and physics targets, bounded domains, editor open/reopen,
  cooked runtime transition, and shutdown smoke paths.
- [x] Publish lasting canonical-payload, Terrain derived-generation, collision
  readiness, and activation timing contracts in their owning runtime documents.
- [x] Remove temporary diagnostics not retained as bounded product telemetry.

#### Recorded Evidence

- Host: Windows x64, NVIDIA GeForce GTX 1060 6GB, Vulkan API 1.3.280,
  threaded RHI; Debug enabled Vulkan validation and Release used the ordinary
  non-validation profile.
- 1025x1025 first-frame render qualification at 17x17 with 256 patches and one
  hardware draw: Debug cold CPU 87.11 ms, warm CPU p50/p95 14.94/15.60 ms,
  GPU p95 1.043 ms; Release cold CPU 49.43 ms, warm CPU p50/p95
  0.97/1.21 ms, GPU p95 0.944 ms. Reopen retained the immutable height and
  topology resources with zero repeated creation.
- Collision-enabled cold phase samples in Debug for 513x513 were
  hash/match/copy/tree/insert 12,195/23/201/7,540/132 us, with a 54 us warm
  match. The 1025x1025 values were 49,218/8/856/29,858/82 us, with a 224 us
  warm match. Release recorded 898/3/162/999/36 us and 33 us warm for 513x513,
  and 3,553/1/571/3,938/19 us and 214 us warm for 1025x1025. Collision-off
  editor registration remained `Dormant` with no heightfield construction or
  physics-scene insertion; Play/Game crossed the required async barrier.
- Both Debug and Release passed `@terrain`, `@physics`,
  `TerrainRenderQualificationTests`, `PhysicsQualificationTests`, full `all`
  builds, and the complete ordinary native-test aggregate. Debug and Release
  DurinEditor and DurinGame process smokes passed hidden startup, native
  gameplay, task-system drain, bounded ticks, and normal shutdown. The Game
  preset's configured Terrain domain contains the runtime primitive and Vulkan
  targets; authoring/cook targets remain correctly Editor-only.

#### Acceptance Gate

- Named activation fixtures meet the frozen phase and total-latency budgets at
  p50 and p95 without correctness regressions.
- Required CPU, Vulkan, editor, cooked runtime, lifecycle, and shutdown validation
  passes on the named profile.
- Lasting contracts are documented outside this plan and every checklist reflects
  recorded evidence.

## Validation Matrix

| Area | Required cases | Evidence |
| --- | --- | --- |
| Payload trust | canonical, malformed layout, corrupt hierarchy, DDC decode | Heightmap unit and derived-data tests |
| Patch cache | render then picking, picking then render, unchanged reuse, each key field changed | Terrain component/primitive tests |
| Collision lifecycle | editor dormant registration, explicit tool request, Play/Simulate barrier, build, cache reuse, disable, edit, reimport, replace, destroy, cancel, shutdown | Engine and Aether focused tests |
| Asset loading | warm/cold DDC, miss, corrupt object, missing/corrupt RAW16, concurrent references | Asset integration tests |
| Render first use | height upload/reuse, topology/pipeline creation/reuse, device failure | Vulkan Terrain tests and counters |
| End to end | editor open/reopen and cooked runtime level transition | Named smoke capture with phase timings |
| Performance | 513x513 and 1025x1025; Debug and Release; collision on/off | Tracy scopes and repeated timing report |

## Definition of Done

- Trusted immutable payload consumers never rebuild the canonical payload merely
  to establish readiness.
- Patch/LOD/bounds metadata is constructed once per component generation and
  reused by render and editor picking.
- Editor authoring activation never constructs Terrain collision; explicit tools
  request it asynchronously, while Play/Simulate waits at the documented
  readiness barrier before gameplay.
- Stale asynchronous work cannot publish after revision, registration, world, or
  shutdown invalidation.
- Cold/warm loading and first-frame rendering have separate diagnostics and meet
  the recorded budgets without changing Terrain results.
- Lasting ownership and readiness contracts, focused tests, builds, and measured
  qualification evidence are complete.

## Deferred Follow-ups

- Cross-component sharing of patch metadata when measured component-local copies
  become material.
- Persistent cooked collision payloads if async construction remains outside the
  runtime transition budget.
- Heightmap streaming, world partition, virtual textures, and GPU-driven Terrain.
- General asynchronous package/object graph loading outside Terrain immutable work.

## Related Documentation

- [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md)
- [Aether Physics Evolution Roadmap](../../../Roadmaps/AetherPhysicsEvolution.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)
- [Terrain Workflow](../../../Editor/Guides/TerrainWorkflow.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmap.h`
- `Engine/Source/Runtime/Engine/Private/Terrain/TerrainHeightmap.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/TerrainComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/TerrainComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/WorldCore.cpp`
- `Engine/Source/Runtime/AetherCore/Private/Collision/CollisionGeometry.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/TerrainHeightmapAuthoringPolicy.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingSceneIndex.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderPrimitiveTests.cpp`
