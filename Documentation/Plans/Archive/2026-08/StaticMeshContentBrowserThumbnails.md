# Static Mesh Content Browser Thumbnails Plan

Summary: Add deterministic rendered StaticMesh thumbnails to Content Browser through the existing provider, scheduler, cache, and preview-scene pipeline.

Last reviewed: 2026-08-08

Status: Archived
Completed: 2026-08-08

## Current Status

Stage 4 and the plan are complete. StaticMesh thumbnails now have validated
cold and warm persistence paths, corruption recovery, mixed rendered-kind
budgets, stale-upload rejection, Content Browser mutation/close coverage, and
deterministic Vulkan presentation assertions. A post-completion editor check
also corrected the Content Browser model's rendered-thumbnail type routing so
`DStaticMesh` cards submit the implemented requests instead of retaining their
fallback icons.
The visual follow-up replaces the fixed gray square with transparent output,
uses a slightly elevated asymmetric three-quarter camera, and tightens bounds
framing so primitives occupy the card more clearly. Generator and preview
fixture versions advance so existing opaque cached PNGs regenerate once.

Stage 4 handoff:

- Baseline commit: `75cff775` (`feat(thumbnails): render static mesh previews`).
- Working set: the shared rendered cache and pipeline public/implementation
  files, StaticMesh and shared scheduler/cache tests, Vulkan material/render
  integration tests, rendered test fixtures, owning thumbnail and StaticMesh
  rendering documentation, and this plan.
- Key symbols: `FRenderedAssetThumbnailCacheStats`,
  `FRenderedAssetThumbnailPipeline::InvalidatePersistentObject()`, the
  decode-failure cold retry in `FRenderedAssetThumbnailCache::FImpl::StartNext()`,
  and upload-serial invalidation in `CancelPendingRequests()`.
- Decisions: a size-valid but undecodable persistent PNG is removed and retried
  once as cold work. Cancellation advances entry serials so queued GPU upload
  completions cannot publish after refresh, mutation, close, or shutdown.
  Stable cache statistics expose lifecycle and budget observations without
  exposing preview-scene or backend objects. Reloadable test fixtures replace
  cached raw StaticMesh pointers after package unload.
- Open questions: none.
- Validation: all 60 `ThumbnailTests` and all 77 `MaterialTests` pass. Coverage
  includes an unloaded-asset warm hit with no scene creation/mutation, corrupt
  PNG recovery, mixed Material/TextureCube/StaticMesh scheduling, Content
  Browser identity mutation and close, tolerant rendered image metrics, and a
  Vulkan cold-store-upload-ready then warm-decode-upload-ready cycle. Full
  `all` build and an eight-tick hidden DurinEditor lifecycle smoke pass on
  `Win64-Debug-DurinEditor-Tests`; plan and changed-document validation pass.
  The follow-up Content Browser routing regression passes with all 71
  `EditorAssetWorkflowTests` cases (70 enabled, one disabled).
  The visual follow-up passes all 60 `ThumbnailTests` and all 77
  `MaterialTests`, including Vulkan assertions for transparent borders,
  non-empty mesh coverage, material response, cold rendering, and warm upload.

The Content Browser already presents source-image, Material, MaterialInstance,
and TextureCube thumbnails through one request/view lifecycle. DurinEd owns an
exact-class provider registry, bounded scheduler, persistent PNG object store,
one shared rendered-preview scene, render/readback pipeline, and GPU upload
cache. StaticMesh assets currently have no registered provider, so their grid
cards fall back to the normal StaticMesh icon.

The implementation can extend the rendered-thumbnail path without adding a
Content Browser viewport or a second cache domain. The remaining gaps are a
public StaticMesh readiness/revision contract, StaticMesh provider capture,
bounds-based framing, preview-scene mesh assignment, and focused validation.

## Goal

Every valid mounted `DStaticMesh` shown in Content Browser grid view receives a
deterministic 256-by-256 rendered thumbnail using its LOD 0 geometry and default
material assignments. Warm thumbnails load from the existing project-local
thumbnail cache without loading or rendering the StaticMesh. Pending and failed
requests remain nonblocking and retain the existing StaticMesh icon.

## Scope

- Register `DStaticMesh` with the existing rendered asset thumbnail service.
- Capture package and transitive Asset Registry dependencies before scheduling.
- Expose the minimum public StaticMesh resource readiness and monotonic revision
  information required for stale-work rejection.
- Render LOD 0 with the mesh's default material slots in the shared preview
  scene and existing PBR StaticMesh renderer.
- Derive a deterministic centered transform and camera framing from validated
  local bounds, including bounded near/far planes and card padding.
- Reuse the existing scheduling, cancellation, render/readback, PNG persistence,
  UI upload, and LRU budget behavior.
- Add provider, lifecycle, invalidation, failure, framing, and rendered-fixture
  coverage; update the owning thumbnail architecture documentation.

## Non-Goals

- Interactive orbit, user-authored thumbnail cameras, per-asset thumbnail
  overrides, animation, or live card viewports.
- Choosing a different LOD, material override, world lighting, sky box, or
  background per asset.
- Generating thumbnails during import, cook, package save, or headless runtime.
- Changing StaticMesh import normalization, renderer shading, asset package
  format, or the 256-by-256 PNG cache format.
- Persisting thumbnails inside `.dasset` packages or source-control content.
- Generalizing every asset type behind a new editor service in this plan.

## Design Decisions and Invariants

- DurinEd remains the sole owner of thumbnail provider registration, scheduling,
  preview scenes, persistent objects, uploads, and budgets. LevelEditor continues
  to submit only fingerprints and present `FAssetThumbnailView`.
- `DStaticMesh` registers by its exact qualified class name. Unsupported classes
  continue to issue no job and show their existing icon.
- A StaticMesh key contains the exact package fingerprint, a distinct provider
  name and generator schema, output settings, a versioned StaticMesh preview
  fixture identity, a StaticMesh shader/visual-contract version, and the sorted
  transitive registry dependency closure. Default-material edits and texture
  dependency edits therefore produce a different key.
- Cold generation loads the asset on the game thread, calls `InitResources()`
  through the existing lifecycle, and waits without blocking until LOD 0 is
  ready. Failed, released, empty, or invalid render data produces one stable,
  asset-qualified diagnostic and no successful disk object.
- The runtime StaticMesh API exposes semantic thumbnail needs, not the private
  state enum: a readiness query, a monotonic render-data/resource revision, and
  read-only validated LOD 0 bounds. Publication, imported-state exchange,
  reimport, resource recreation, and release must advance or invalidate that
  observable contract so an old completion cannot publish over new geometry.
- The preview scene uses one dedicated `DStaticMeshComponent` path for mesh
  thumbnails. Before every capture it clears the Material and TextureCube paths,
  assigns the requested mesh, and uses the asset's default material slots.
  `Reset()` detaches all asset references before another job or shutdown.
- Framing is deterministic and contains the complete validated local `FBox`.
  The transform recenters the bounds; the camera keeps the established
  three-quarter direction and 42-degree vertical field of view, computes
  distance from all projected box corners for both axes, applies a fixed 10%
  image-space margin, and derives finite near/far planes with a minimum positive
  near distance. Invalid or degenerate bounds fail rather than emitting a
  misleading empty card.
- The first version renders LOD 0 with opaque output, the shared neutral
  background, key/fill lighting, and editor assistance disabled. Any change to
  geometry choice, framing, lighting, material fallback, shaders, color space,
  or post-processing increments the matching schema/contract version.
- Visible requests retain priority over prefetch. Only one shared preview scene
  and at most one rendered capture per frame remain live; adding StaticMesh does
  not raise existing default budgets.
- Warm-cache hits decode and upload the PNG without loading the StaticMesh,
  initializing its resources, or creating/mutating the preview scene.

### StaticMesh Readiness and Revision Transitions

`FStaticMeshRenderResourceStatus::IsReady()` is publishable only when readiness
is `Ready` and revision is nonzero. A revision belongs to one `DStaticMesh`
object identity; imported-state exchange never swaps or reuses revisions.

| Boundary | Public readiness after boundary | Revision rule |
| --- | --- | --- |
| Construction, before CPU render data | `Unavailable` | Seed one nonzero object-local revision |
| Successful CPU render-data publication, cooked load, or first import | `Unavailable` until initialization is accepted | Advance even when readiness remains `Unavailable` |
| Accepted `InitResources()` | `Queued` | Advance before work becomes externally observable |
| Render-thread initialization succeeds | `Ready` | Advance with the coherent ready publication |
| Render-thread initialization fails | `Failed` | Advance with the coherent terminal failure publication |
| Candidate preparation fails before replacing current data | Existing readiness | Do not advance because current data and resources remain authoritative |
| Successful render-data replacement or render-resource recreation | `Ready` when the replacement is already initialized, otherwise `Unavailable` | Advance at invalidation/publication; old completions cannot match |
| Imported-state exchange, commit, or reverse | Readiness of the data now owned by each object | Advance each affected object's own revision; never swap revision values |
| Accepted release request | `Unavailable` | Advance before release is queued; the prior `Ready` revision is immediately unpublishable |
| Release completion | `Unavailable` | Advance to represent the completed resource invalidation |
| Destruction begins or CPU render data is finally discarded | `Unavailable` | Advance at each boundary that can still be observed; no prior revision becomes publishable again |
| Repeated no-op initialization/release or rejected state transition | Existing readiness | Do not advance |

## Current Foundations and Gaps

| Area | Existing foundation | Required change |
| --- | --- | --- |
| Content Browser | One source/rendered request facade and fallback presentation | No card/UI behavior change; prove StaticMesh fingerprints route to rendered output |
| Provider system | Exact-class registry, immutable inputs, provider generations, serial cancellation | Add a StaticMesh provider and generation input |
| Cache identity | Package, dependency, fixture, output, and shader contract fields | Define StaticMesh provider/fixture versions and include material dependency closure |
| StaticMesh runtime | LOD arrays, local bounds, default slots, async resource initialization | Expose stable readiness/revision semantics needed by the provider |
| Preview scene | Shared Material sphere and TextureCube preview component | Add mutually exclusive requested-mesh assignment and bounds-derived view |
| Pipeline | Loading, resource wait, rendering, readback, PNG persistence, GPU upload | Route StaticMesh jobs and retain/release the active mesh safely |
| Validation | Provider-neutral, Material, TextureCube, cache, and rendered fixture tests | Add StaticMesh contract, framing, invalidation, failure, and image coverage |

## Implementation Stages

### Stage 0: Freeze StaticMesh Thumbnail Contracts

- [x] Add named constants for the StaticMesh provider, generator schema,
  preview-fixture identity/version, shader contract, margin, LOD index, and
  output-opacity policy.
- [x] Define the public StaticMesh readiness/revision result and exact transitions
  for initial load, resource initialization, failure, reimport/imported-state
  exchange, render-state recreation, release, and destruction.
- [x] Define a pure bounds-to-view calculation that consumes local bounds,
  output aspect ratio, field of view, camera direction, and margin, and returns
  centered transform, camera position/target, and clip planes.
- [x] Add contract tests proving projected bounds fit both image axes and invalid,
  non-finite, or zero-volume bounds are rejected.

#### Acceptance Gate

- The readiness/revision table has no transition that permits changed or
  released render data to retain a publishable prior revision.
- The framing helper is deterministic, independent of card draw size, and
  contains representative cube, tall, wide, deep, offset, and very small bounds
  with the specified margin.

### Stage 1: Expose StaticMesh Render Identity and Readiness

Dependencies: Stage 0.

- [x] Add the minimal semantic query API to `DStaticMesh`; keep the private
  lifecycle enum and mutable render data encapsulated.
- [x] Advance the monotonic revision at every render-data/resource publication
  or invalidation boundary selected in Stage 0.
- [x] Return validated LOD 0 bounds only when CPU render data exists; report
  queued, ready, failed, or unavailable resource state without waiting.
- [x] Extend StaticMesh lifecycle tests across initial initialization, successful
  recreation, failed initialization, imported-state exchange, and release.

#### Acceptance Gate

- A caller can initiate initialization, poll without blocking, distinguish wait
  from terminal failure, obtain validated LOD 0 bounds, and revalidate the same
  nonzero revision immediately before publication.
- Existing StaticMesh render-resource ownership and destruction fences remain
  unchanged and lifecycle tests pass.

### Stage 2: Add the StaticMesh Provider and Cache Routing

Dependencies: Stage 1.

- [x] Add `FStaticMeshAssetThumbnailProvider` and an immutable input containing
  only the mounted asset path and frozen visual contract.
- [x] Validate the exact registry fingerprint and capture the sorted transitive
  dependency closure so default-material and referenced-texture package edits
  invalidate persistent output.
- [x] Register the provider beside Material, MaterialInstance, and TextureCube;
  generalize misleading Material-only cache/internal names where they now own
  all rendered asset thumbnail kinds.
- [x] Route StaticMesh active-job loading, exact-class validation, revision
  capture, resource initialization, wait, cancellation, and teardown through the
  existing pipeline without introducing a parallel scheduler or object store.
- [x] Add provider tests for exact class, deterministic key input, dependency
  ordering, missing/stale registry data, and generator/fixture version changes.

#### Acceptance Gate

- A StaticMesh card request enters the same priority queue and state machine as
  existing rendered assets, while an unsupported class remains `NotRequested`.
- Editing the mesh package or any captured material/texture dependency changes
  the key; an identical fingerprint and contract reproduces the same key.
- Cancellation or cache destruction releases the loaded asset reference and
  prevents stale upload/publication.

### Stage 3: Render and Frame StaticMesh Thumbnails

Dependencies: Stages 1-2.

- [x] Extend the shared preview-scene pool with a StaticMesh assignment that is
  mutually exclusive with Material-sphere and TextureCube assignments.
- [x] Apply the Stage 0 bounds-to-view result before capture and ensure the
  renderer draws LOD 0 with the asset's default slot materials and existing
  missing/error-material behavior.
- [x] Revalidate asset and resource revisions after readiness, before render,
  after readback, and before encoded publication through the existing scheduler
  transitions.
- [x] Reset component references and per-job view state after success, failure,
  cancellation, and shutdown; retain the one-scene/one-render-per-frame budget.
- [x] Preserve fallback presentation for pending and failed work and surface one
  stable asset-qualified diagnostic without retrying every frame.

#### Acceptance Gate

- Representative cube, tall, wide, and multi-section/material meshes are fully
  visible, centered, consistently oriented, and shaded with their default
  materials in the 256-by-256 output.
- A resource revision change at any asynchronous boundary rejects the stale
  capture, and a later current request can complete normally.
- Invalid bounds, missing render data, resource failure, and missing material
  dependencies retain the StaticMesh icon and never publish a successful object.

### Stage 4: Validate Persistence, Budgets, and Editor Workflow

Dependencies: Stage 3.

- [x] Add cold-path coverage for load, resource wait, render, readback, encode,
  atomic store, upload, and ready presentation.
- [x] Add warm-path coverage proving a disk hit performs decode/upload only and
  does not load or initialize the StaticMesh or mutate the preview scene.
- [x] Add deterministic rendered fixtures and image assertions for framing,
  orientation, background opacity, default-material selection, and non-empty
  geometry coverage; use tolerant pixel/image metrics rather than exact driver
  byte equality where Vulkan output can vary.
- [x] Stress visible versus prefetch ordering, duplicate coalescing, queue bounds,
  one-render-per-frame behavior, cancellation, GPU LRU eviction, and corrupted
  cache recovery with mixed Material, TextureCube, and StaticMesh requests.
- [x] Exercise Content Browser refresh, rename, move, reimport, delete, close,
  and editor shutdown while StaticMesh work is queued, waiting, rendering, or
  uploading.
- [x] Update `AssetThumbnails.md` and any changed StaticMesh rendering contract,
  then run focused native tests and the repository-required full `all` build for
  this user-visible editor change through the documented DurinDevTool workflow.

#### Acceptance Gate

- Cold, warm, invalidation, recovery, mixed-load, lifecycle, and rendered-fixture
  tests pass on the supported renderer path.
- Content Browser remains responsive and shows the correct thumbnail or fallback
  throughout mutation and shutdown workflows, with no leaked preview scene,
  loaded asset retention, stale UI texture, or whole-device idle wait.
- A successful full `all` build verifies the editor executable from the active
  Agent Build Profile.

## Validation Matrix

| Concern | Unit/contract | Integration | Rendering/end-to-end |
| --- | --- | --- | --- |
| Provider identity and key | Exact class, stable schema, package/dependency ordering | Registry mutation creates new request/key | Warm object reused only for identical key |
| Readiness and revisions | StaticMesh lifecycle transition tests | Reimport/recreate during queued and waiting work | Stale render/readback cannot publish |
| Framing | Projected-corner containment and invalid bounds | Preview scene receives centered transform and per-job view | Cube/tall/wide/deep fixtures stay inside margin |
| Materials | Dependency closure and default slot mapping | Multi-section mesh resolves default/error materials | Distinct fixture slots render visibly distinct output |
| Failure | Stable diagnostics for load/resource/bounds errors | No persisted success or per-frame retry | Card retains StaticMesh icon and editor remains responsive |
| Persistence | Key/store corruption and size bounds | Cold render then warm decode/upload | Restart reuses PNG without mesh load/render |
| Scheduling/lifetime | Priority, coalescing, serial, budgets | Mixed asset kinds share one scene and queue | Close/shutdown cancels and releases all resources |

## Definition of Done

- Every stage acceptance gate is satisfied and its checklist is evidence-backed.
- StaticMesh thumbnails use the provider-neutral rendered-thumbnail system with
  no LevelEditor-owned rendering or per-card scene/view lifetime.
- Thumbnail identity covers geometry, default-material dependencies, output,
  fixture, and visual/shader contract changes.
- Automatic framing is deterministic, bounded, and validated across aspect
  extremes and invalid geometry.
- Cold and warm paths, stale-work rejection, failure fallback, budgets, cache
  recovery, editor mutations, and shutdown are covered by focused tests.
- Long-lived behavior is recorded in owning architecture/runtime docs, plan
  validation passes, and the required full editor build succeeds.

## Deferred Follow-ups

- User-authored thumbnail cameras, saved orbit, framing overrides, and custom
  backgrounds or lighting rigs.
- LOD selection, wireframe, collision, Nanite-equivalent, skeletal mesh, and
  animated previews.
- Dependency-driven proactive regeneration for assets that are not requested by
  a visible or prefetched card.
- A broader rename or extraction of rendered-thumbnail cache code beyond names
  directly made inaccurate by StaticMesh support.

## Related Documentation

- [Asset Thumbnails](../../../Editor/Architecture/AssetThumbnails.md)
- [Content Browser](../../../Editor/Architecture/ContentBrowser.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Thumbnail/AssetThumbnail.h`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/RenderedAssetThumbnailPipeline.h`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/MaterialAssetThumbnail.cpp`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/RenderedAssetThumbnailPreviewScene.cpp`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/RenderedAssetThumbnailPipeline.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/ContentBrowserThumbnailCache.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Tests/Native/EngineTests/Private/AssetThumbnailContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/MaterialAssetThumbnailTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TextureCubeAssetThumbnailTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RenderedAssetThumbnailFixtureTests.cpp`
