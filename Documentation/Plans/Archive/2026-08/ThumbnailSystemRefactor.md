# Thumbnail System Refactor Plan

Summary: Refactor DurinEd thumbnails into a UE-style manager, renderer, asset-thumbnail, and pool architecture without changing derived-output behavior.

Last reviewed: 2026-08-29

Status: Archived
Completed: 2026-08-29

## Current Status

The refactor is implemented. Thumbnail remains a subsystem of `DurinEd`; the
manager owns exact-class renderer generations and the shared pool, asset cards
hold `FAssetThumbnail` references, and ordinary image files retain the private
Content Browser file-preview path. Material, MaterialInstance, Texture2D,
TextureCube, StaticMesh, SkeletalMesh, and Terrain Heightmap resolve through
feature-owned renderers.

The final public model follows the Unreal Editor concepts
`DThumbnailManager`, `DThumbnailRenderer`, `FThumbnailRenderingInfo`,
`FAssetThumbnail`, `FAssetThumbnailPool`, and `FObjectThumbnail`. The final
surface does not retain `Provider`, `Service`, `Executor`, or a
Content-Browser-specific asset-thumbnail cache facade.

### Frozen Compatibility Decisions

- Pool production remains fixed at 256 by 256. Requested dimensions are
  presentation-only, do not enter persistent identity, and do not expand CPU,
  GPU, or disk budget key spaces.
- Persistent field order, renderer strings, generator schemas, PNG encoding,
  object/index format, and `DerivedDataCache/Thumbnails` root remain compatible.
- Renderers capture on the game thread, optionally park a cold session while
  resources become ready, and either prepare a preview scene or publish direct
  canonical pixels. Reset is idempotent and generation-qualified.
- Texture2D asset thumbnails derive from canonical pixels embedded in the
  package. External reimport hints are never used as asset identity. Ordinary
  physical image files remain in `FSourceImageThumbnailCache`.

## Goal

Make asset thumbnails a cohesive `DurinEd` subsystem with UE-compatible names
and responsibilities: a manager maps exact asset classes to renderers, asset
thumbnail objects bind UI consumers to a shared pool, the pool owns bounded
asynchronous production and texture reuse, and feature modules own their
concrete renderers. Preserve the current nonblocking behavior, deterministic
identity, persistent reuse, cancellation, module-unload safety, diagnostics,
and visual results throughout the migration.

## Scope

- Introduce the final `DThumbnailManager`, `DThumbnailRenderer`,
  `DDefaultSizedThumbnailRenderer`, `FThumbnailRenderingInfo`,
  `FAssetThumbnail`, `FAssetThumbnailPool`, and `FObjectThumbnail` contracts in
  `DurinEd`.
- Move exact-class renderer lookup, registration generation, dirtiness, and
  the shared-pool entry point behind `DThumbnailManager`.
- Consolidate rendered asset and authored source-image asset work behind one
  `FAssetThumbnailPool`, while retaining path-specific concurrency limits and
  state transitions internally.
- Migrate Material, MaterialInstance, Texture2D, TextureCube, StaticMesh, and
  Terrain Heightmap generation to concrete `DThumbnailRenderer` subclasses in
  their owning editor modules.
- Replace Content Browser asset-thumbnail request/view bookkeeping with
  `FAssetThumbnail` ownership, prioritization, refresh, and presentation.
- Preserve exact-class modular registration, safe renderer retirement,
  cancellation, task scopes, game/render/worker-thread ownership, preview
  scene reset, RHI upload retirement, persistent object safety, and shutdown
  ordering.
- Migrate tests and lasting thumbnail/Content Browser/module documentation to
  the final vocabulary and ownership.

## Non-Goals

- Creating a separate `Thumbnail`, `ThumbnailCore`, or runtime module.
- Reproducing Unreal Engine implementation details, synchronous `Draw`
  assumptions, Slate types, package-embedded thumbnails, or UObject package
  serialization merely to match names.
- Giving each Content Browser card a world, viewport, preview actor, or
  independently owned GPU texture.
- Adding new asset families, animated/realtime thumbnails, user-authored
  custom thumbnails, arbitrary output sizes, or a thumbnail editor.
- Redesigning existing Material, TextureCube, StaticMesh, or Terrain visual
  composition and framing.
- Treating ordinary unregistered PNG/JPEG files as assets. Their file-preview
  presentation remains a Content Browser responsibility; shared image-codec
  primitives may be reused without merging file identity into
  `FAssetThumbnail`.
- Changing authored packages, source files, runtime loading, cooking, or the
  canonical asset-data lifecycle.

## Design Decisions and Invariants

### Ownership and vocabulary

- Thumbnail remains under `Engine/Source/Editor/DurinEd`; `DurinEd` is the only
  owner of shared asset-thumbnail contracts, scheduling, caches, persistence,
  preview-scene pooling, upload, budgets, and default manager lifetime.
- Final public symbols use Thumbnail Manager, Thumbnail Renderer, Asset
  Thumbnail, Asset Thumbnail Pool, Object Thumbnail, and Thumbnail Rendering
  Info terminology. Transitional adapters may exist privately within an
  incomplete stage, but the completed plan exposes no parallel Renderer,
  Service, Executor, or Content Browser asset-cache facade.
- `DThumbnailRenderer` is an abstract reflected `DObject` using the repository's
  `D` prefix. Concrete renderer classes remain in their asset-family modules:
  `DMaterialThumbnailRenderer`, `DTextureThumbnailRenderer`,
  `DTextureCubeThumbnailRenderer`, `DStaticMeshThumbnailRenderer`, and
  `DTerrainHeightmapThumbnailRenderer`.
- `DDefaultSizedThumbnailRenderer` owns only common fixed-size policy. It does
  not centralize feature-specific loading, dependencies, readiness, preview
  content, framing, or diagnostics.

### Manager and renderer lifetime

- `DThumbnailManager` owns one exact-class rendering-info table and the shared
  pool entry point. Duplicate exact-class registration fails atomically;
  replacement requires retirement of the previous renderer registration.
- Feature modules register and unregister their renderer classes through a
  move-only renderer-registration handle bound to the module callback gate.
  Matching UE vocabulary does not weaken Durin's unload contract.
- The manager may instantiate renderer objects lazily, but a pool job never
  keeps an unqualified raw renderer pointer across game, worker, or rendering
  thread boundaries. Captured work retains a manager-owned generation lease;
  unregistering a renderer closes admission, cancels its leases, resets its
  preview state on the game thread, and drains feature-owned callbacks before
  returning.
- Renderer lookup remains exact-class for this plan. Inheritance fallback,
  configuration-driven renderer discovery, and arbitrary project renderer
  plugins are deferred until class-hierarchy and unload semantics have an
  explicit contract.

### Asset thumbnail and pool

- `FAssetThumbnail` is the UI-facing reference for one asset identity, requested
  width/height, and pool. It owns no authored asset, preview world, render
  target, RHI allocation, worker task, or persistent object.
- Construction from unloaded `FAssetData` is the normal browser path. A
  resident `DObject` convenience path may be added only if it reduces a real
  caller and resolves to the same canonical identity.
- `FAssetThumbnailPool` owns request coalescing, reference pinning,
  visible-versus-prefetch prioritization, state transitions, per-frame work,
  CPU/GPU LRU retention, renderer invocation, preview-scene leasing, readback,
  upload, persistent reuse, cancellation, refresh, and failure publication.
- The manager owns one shared default pool, while tests and isolated tools may
  construct injected local pools. A process singleton is not the only usable
  construction path.
- Different production paths share admission, identity, publication,
  cancellation, pinning, and result state, but retain separate decode, render,
  resource-wait, and upload limits. Unifying the pool must not turn source
  decode into game-thread work or let a parked rendered asset monopolize the
  render slot.
- `FObjectThumbnail` represents core-owned CPU thumbnail output and its encoded
  form. It is optional editor-derived data and is not serialized into authored
  packages by this plan.

### Identity, persistence, and presentation

- Existing cache identity fields, renderer/generator schema values, package and
  transitive dependency fingerprints, output settings, visual-contract
  versions, DDC domain, PNG encoding, and corruption recovery remain compatible
  unless Stage 0 records a specific unavoidable migration.
- Renaming a C++ type alone must not invalidate persistent objects. Renderer
  names that currently participate in keys retain their existing stable string
  identity until an intentional visual or data-contract version changes.
- Every asynchronous completion revalidates asset identity, request serial,
  renderer generation, key, and captured asset/resource revisions before
  publication. A stale completion cannot replace a newer request.
- Pending and failed assets continue to show their asset icon; valid ready
  thumbnails retain transparency policy and diagnostics. Failure is stable and
  requires changed identity, explicit refresh, or explicit retry.
- The initial supported pool size is the existing fixed 256-by-256 production
  output. `FAssetThumbnail` records requested dimensions for UE-compatible UI
  semantics, but Stage 0 must select whether non-256 requests scale the shared
  output at presentation or participate in cache identity. It must not silently
  introduce an unbounded size key space.

### Threads and shutdown

- Asset loading, renderer preparation, preview-scene mutation, and feature
  renderer callbacks occur on the game thread. Rendering, environment
  resolution, capture, and readback obey their existing rendering-thread
  contracts. Decode, encode, and atomic object publication may use owned worker
  tasks.
- MainFrame stops Content Browser admission, releases asset-thumbnail
  referencers, unregisters feature renderers in reverse composition order,
  drains the shared pool, destroys the manager, and only then unloads concrete
  feature modules and rendering dependencies.
- All success, failure, cancellation, renderer retirement, pool replacement,
  and shutdown paths converge on idempotent preview reset and owning-thread RHI
  release.

### Selected final public layout

```text
DurinEd/Public/
|-- AssetThumbnail.h
`-- ThumbnailRendering/
    |-- ThumbnailManager.h
    |-- ThumbnailRenderer.h
    |-- DefaultSizedThumbnailRenderer.h
    |-- ThumbnailRenderingInfo.h
    `-- ThumbnailPreviewScene.h
```

Queue, cache-entry, render-job, source-decode, object-store index, upload-ticket,
and renderer-generation lease types remain private implementation details.

## Current Foundations and Gaps

| Area | Current foundation | Plan gap |
| --- | --- | --- |
| Type routing | Exact-class registry with generation-qualified scoped registration and unload drain | Public vocabulary is Renderer-oriented and the default registry is separate from an editor manager |
| Rendered assets | One bounded asynchronous cache owns scheduling, persistence, preview scenes, readback, upload, eviction, and diagnostics | UI consumes request/view calls rather than `FAssetThumbnail` references and a shared pool |
| Authored source images | Texture2D selects a source through its asset-family integration; Content Browser decodes, persists, and uploads it | Asset source-image production is owned by a Content Browser-private cache and follows a second lifecycle |
| Ordinary files | Content Browser can preview supported physical image files | File identity and asset identity are combined by one browser facade even though only one is an asset thumbnail |
| Renderers | Material, TextureCube, StaticMesh, Texture2D, and Terrain behaviors already exist in owning modules | They implement multiple Renderer/extension/session shapes rather than one renderer abstraction |
| Identity and invalidation | Package, dependency, schema, fixture, shader, asset revision, and resource revision checks reject stale work | Stable key identity must survive public C++ renaming and pool consolidation |
| Persistence | Project-local `DerivedDataCache/Thumbnails` objects are atomic, bounded, recoverable, and optional | Source and rendered implementations have separate private storage paths and index mechanics |
| Presentation | Content Browser maps public states to icon/loading/ready/failure display | Each panel owns a browser cache rather than lightweight thumbnail references into a shared pool |
| Tests | Contract, source-image, Content Browser, material, texture, mesh, Vulkan, invalidation, corruption, and shutdown coverage exists | Fixtures and assertions name the old architecture and do not prove manager/pool reference semantics |

## Implementation Stages

### Stage 0: Freeze the UE-style contract and compatibility baseline

- [x] Inventory every public and private Thumbnail symbol, production call site,
  module registration, shutdown edge, task scope, test fixture, cache key field,
  persistent object/index schema, setting, diagnostic, and documentation term
  affected by the rename.
- [x] Freeze the final signatures and ownership for `DThumbnailManager`,
  `DThumbnailRenderer`, `DDefaultSizedThumbnailRenderer`,
  `FThumbnailRenderingInfo`, renderer-registration handles,
  `FAssetThumbnail`, `FAssetThumbnailPool`, and `FObjectThumbnail`.
- [x] Select the 256-output versus requested-size rule and record its exact key,
  scaling, memory-budget, and presentation consequences.
- [x] Select the renderer preparation/draw/reset state model needed to preserve
  nonblocking load, parked resource waits, rendered capture, direct generated
  pixels, and authored source-image paths without publishing generic
  Renderer/Executor terminology.
- [x] Freeze stable persistent key strings and byte formats so C++ renaming can
  be distinguished from intentional generator-schema changes.
- [x] Capture the existing default budgets, queue ordering, state transitions,
  cache-hit behavior, visual hashes or pixel oracles, diagnostics, unload
  behavior, and shutdown ordering in focused baseline tests.
- [x] Classify existing source-image behavior into Texture2D asset-thumbnail
  production versus ordinary physical-file preview, and identify the exact code
  that remains in Content Browser after the split.

#### Acceptance Gate

- Every old public symbol and production call site maps to one final symbol or
  an explicitly deleted private mechanism.
- The final contracts contain no unresolved ownership, requested-size,
  renderer-lifetime, file-versus-asset, cache-compatibility, or thread decision.
- Baseline fixtures fail on changed visual output, key identity, queue ordering,
  stale publication, renderer retirement, persistence reuse, or shutdown order.

### Stage 1: Introduce Thumbnail Manager and Renderer contracts

- [x] Add reflected `DThumbnailRenderer` and
  `DDefaultSizedThumbnailRenderer`, with renderer-owned identity capture,
  preparation, readiness, draw or pixel production, revision validation,
  diagnostics, and idempotent reset hooks selected in Stage 0.
- [x] Add `FThumbnailRenderingInfo` and `DThumbnailManager` with exact-class
  lookup, duplicate rejection, renderer generation, stable unsupported result,
  dirtied event, shared-pool access, and explicit shutdown.
- [x] Add move-only, module-gated custom-renderer registration and retirement
  that preserves the existing admission-close, cancellation, session reset,
  callback drain, and object-destruction guarantees.
- [x] Compose the default manager from MainFrame without changing concrete
  renderer registration order or Content Browser behavior.
- [x] Add focused manager tests for exact lookup, duplicate registration,
  unregister/reregister generations, lazy instance lifetime, unsupported
  classes, dirty notification, local manager injection, and shutdown.
- [x] Keep any bridge to the old registry private and stage-local; do not expose
  a second public rendering extension path.

#### Acceptance Gate

- The manager can register, resolve, retire, and replace a renderer by exact
  asset class without leaking a feature-module pointer into retained work.
- Renderer retirement cancels and drains every captured generation before the
  module gate retires, including queued, waiting, rendering, readback,
  encoding, and uploading states.
- Existing thumbnails still produce identical keys, states, diagnostics, and
  visual output through the temporary bridge.

### Stage 2: Establish Object Thumbnail and Asset Thumbnail Pool ownership

- [x] Add `FObjectThumbnail` as the core-owned CPU pixel/encoded result with
  explicit dimensions, transparency, encoding version, compression/decode, and
  corruption validation.
- [x] Add `FAssetThumbnailPool` and move the current rendered request queue,
  coalescing, priority, cancellation, memory/GPU LRU, persistence, upload,
  statistics, budgets, and frame pumping behind it.
- [x] Preserve the existing project-local DDC root and stable key/object/index
  format, including bounded path validation, atomic publication, corrupt-object
  removal, and cold regeneration.
- [x] Add pool referencer counts and pinning so visible `FAssetThumbnail`
  objects retain their entries while unused textures remain recyclable.
- [x] Preserve distinct bounded lanes for authored-source decode, renderer
  preparation/resource wait, capture/readback, encode, and upload inside one
  pool admission and publication model.
- [x] Add `FAssetThumbnail` construction, reassignment, destruction, state,
  texture access, refresh, priority, and rendered/failed notification behavior.
- [x] Cover local pools, shared pool ownership, duplicate thumbnails,
  referencer release, LRU eviction, cancellation, cache warm reuse, corruption,
  and pool shutdown.

#### Acceptance Gate

- Multiple `FAssetThumbnail` instances for the same canonical identity and
  output share one pool entry and one admitted production job.
- Releasing the last referencer makes the entry evictable without synchronously
  destroying an in-use RHI resource or cancelling unrelated references.
- Warm and cold results, budgets, diagnostics, persistent bytes, corruption
  recovery, and stale-completion rejection match the Stage 0 baseline.

### Stage 3: Migrate concrete asset renderers

- [x] Replace Material and MaterialInstance registrations with
  `DMaterialThumbnailRenderer`, preserving sorted transitive dependencies,
  sphere fixture, inherited parameters, texture readiness, transparent output,
  draw validation, revisions, and diagnostics.
- [x] Replace StaticMesh registration with `DStaticMeshThumbnailRenderer`,
  preserving LOD 0 bounds framing, default material closure, resource waits,
  transparent output, preview attachment/reset, and zero-draw rejection.
- [x] Replace TextureCube registration with
  `DTextureCubeThumbnailRenderer`, preserving wide environment orientation,
  stable RHI texture retention, opaque output, no-world-content behavior,
  environment reset, and invalid-environment failure.
- [x] Replace Texture2D source selection with
  `DTextureThumbnailRenderer` production through the pool, preserving authored
  source fingerprinting, decode limits, transparency, disk reuse, and failure
  behavior.
- [x] Replace Terrain Heightmap generation with
  `DTerrainHeightmapThumbnailRenderer`, preserving fixed grayscale pixels,
  generator schema, revision identity, and independence from Renderer state.
- [x] Register every concrete renderer from its owning module using the manager
  and module gate; remove the old registration/extension/session implementation
  immediately after the corresponding renderer passes parity.
- [x] Run focused per-family CPU and Vulkan rendering parity coverage after each
  migration rather than deferring all visual diagnosis to the final stage.

#### Acceptance Gate

- Every currently supported asset class resolves through
  `DThumbnailManager` to exactly one owning renderer; unsupported classes admit
  no job.
- Each family preserves its cache key, dependency invalidation, readiness,
  visual output, transparency, diagnostic, cancellation, and unload behavior.
- No production concrete asset module includes or names an old Renderer,
  rendered extension, or generation-session contract.

### Stage 4: Migrate Content Browser to Asset Thumbnail references

- [x] Replace per-panel rendered asset request/view maps with owned
  `FAssetThumbnail` references backed by the manager's shared pool.
- [x] Prioritize visible cards and prefetch bounded nearby cards through pool
  APIs; release references deterministically on refresh, navigation, filtering,
  item replacement, panel close, and shutdown.
- [x] Preserve icon, loading, ready, transparency-grid, failed, tooltip, retry,
  and explicit refresh presentation without exposing pool internals to the
  item view.
- [x] Split ordinary physical-file image preview from Texture2D asset-thumbnail
  production. Retain a Content Browser-private file-thumbnail path only for
  non-asset items and reuse lower-level decoding primitives where dependency
  direction permits.
- [x] Remove `FContentBrowserFileThumbnailCache` as an asset-thumbnail facade and
  delete obsolete identity-to-rendered-path bookkeeping.
- [x] Cover multiple panels, duplicate visible cards, rapid navigation,
  refresh, rename, move, delete, reimport, filter churn, panel close, and editor
  shutdown with shared-pool reference and stale-publication assertions.

#### Acceptance Gate

- Content Browser asset cards know only `FAssetThumbnail` and presentation
  state; they do not select renderer, source-image, generated-pixel, or rendered
  paths.
- Multiple panels reuse pool output while releasing one panel cannot cancel or
  evict an entry still referenced by another.
- Ordinary image files retain their current preview behavior without entering
  manager renderer lookup or asset DDC identity.

### Stage 5: Remove the legacy architecture and harden lifecycle behavior

- [x] Delete the old renderer registry, renderer interfaces, rendered cache,
  generation extension/session types, Content Browser asset-cache facade, and
  obsolete public headers after all production and test call sites migrate.
- [x] Rename private queue, cache, object-store, preview-scene, diagnostic, and
  statistics types to the final Thumbnail vocabulary without changing stable
  persisted strings.
- [x] Audit `DurinEd`, ContentBrowser, MainFrame, MaterialEditor,
  TextureEditor, StaticMeshEditor, and LevelEditor module descriptors and public
  includes for cycles, unnecessary exposure, and feature-module dependencies.
- [x] Exercise cancellation and renderer retirement from every public pool
  state, including already-enqueued render-thread uploads and parked resource
  waits.
- [x] Exercise repeated manager/pool construction and destruction, feature
  unregister/reregister, editor shutdown, RHI resource retirement, and task
  drain under the repository lifecycle test conventions.
- [x] Add deterministic pool statistics for queued, pinned, CPU-resident,
  GPU-resident, disk-hit, cold-render, decode, cancellation, stale-rejection,
  failure, and eviction counts needed to qualify the migration.

#### Acceptance Gate

- Targeted search finds no production public or private Thumbnail type using
  Provider, Service, Executor, `FRenderedAssetThumbnailCache`, or
  `FContentBrowserFileThumbnailCache` terminology.
- Public module dependency direction is acyclic, concrete renderers remain
  feature-owned, and `DurinEd` contains no concrete asset-family cast,
  readiness branch, framing rule, or diagnostic.
- Sanitized/debug lifecycle coverage observes no callback after module
  retirement, stale texture publication, leaked task, retained preview content,
  double reset, or wrong-thread RHI release.

### Stage 6: Qualify, document, and hand off the refactor

- [x] Run the smallest registered Thumbnail, Content Browser, per-family,
  module-lifecycle, RenderCore/RHI, and Vulkan rendering targets selected under
  repository testing guidance, then the required bounded aggregate and build
  tier for this cross-editor shared subsystem.
- [x] Compare queue ordering, cache hit/miss, frame work limits, CPU/GPU/disk
  retention, persistent reuse, cancellation, and representative rendered output
  against Stage 0 without silently raising an existing budget.
- [x] Update the lasting Asset Thumbnails, Content Browser, and Code Modules
  contracts to the final manager/renderer/asset-thumbnail/pool ownership and
  remove obsolete terminology.
- [x] Record exact build, test, Vulkan, visual, corruption, unload, shutdown,
  budget, and documentation validation evidence in this plan.
- [x] Complete every passed checklist, set final lifecycle metadata only after
  all acceptance gates pass, and prepare the repository-required plan/stage
  commit provenance.

#### Acceptance Gate

- Focused, bounded aggregate, build, Vulkan/rendering, lifecycle, persistence,
  corruption, documentation, and budget gates all pass with recorded evidence.
- Final source, tests, persisted-key contract, module ownership, and lasting
  documentation agree on one Thumbnail Manager/Renderer/AssetThumbnail/Pool
  architecture.
- The editor can browse supported and unsupported assets, navigate and refresh
  under load, unload feature renderers, and shut down without a visual,
  responsiveness, cache, task, or resource-lifetime regression.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Public architecture | Compile-time and targeted-search evidence for only Manager, Renderer, RenderingInfo, AssetThumbnail, AssetThumbnailPool, and ObjectThumbnail public concepts |
| Manager registration | Exact-class success, duplicate rejection, unsupported result, unregister/reregister generation, module-gated retirement, lazy instance lifetime, and local injection |
| Pool scheduling | Visible before prefetch, duplicate coalescing, bounded lanes, parked waits releasing capture capacity, upload/frame limits, and no per-frame retry loop |
| Reference ownership | Duplicate `FAssetThumbnail` instances share output; pin/unpin, last-release eviction, multi-panel survival, reassignment, and panel destruction are deterministic |
| Identity and invalidation | Package, dependency, schema, fixture, shader, asset revision, resource revision, save, move, delete, reimport, and renderer replacement reject stale results |
| Persistence | Old stable keys remain warm hits; missing, incompatible, oversized, truncated, corrupt, and decode-invalid objects safely miss, remove, and regenerate |
| Renderer families | Material/instance, Texture2D, TextureCube, StaticMesh, and Terrain retain selected key fields, visual contract, transparency, readiness, revisions, and diagnostics |
| Preview isolation | Scene content, camera/view, environment texture, output policy, lighting, cancellation generation, and diagnostic state reset between heterogeneous jobs |
| File boundary | Asset Texture2D uses renderer/pool identity; ordinary supported image files retain private file-preview behavior and never register as assets |
| Threading | Game-thread object work, rendering-thread capture/readback/upload retirement, worker decode/encode/publication, cancellation, and callback-gate ownership are asserted |
| Shutdown | Browser admission stop, reference release, reverse renderer unregister, pool drain, manager destruction, task completion, and module/RHI teardown occur in order |
| Performance and budgets | Queue, CPU pixels, GPU textures, disk objects, preview scenes, parked waits, per-frame render/upload, and representative frame-time observations do not exceed existing limits |
| Compatibility | Unsupported icons, pending/failed presentation, explicit retry, DDC root, PNG output, settings, stable diagnostic intent, and non-thumbnail asset behavior remain unchanged |
| Documentation | Changed/all document validation and all-plan lifecycle validation pass after lasting contracts and this plan are updated |

## Validation Evidence

Completed on Windows MSVC x64 with the `Win64-Debug-DurinEditor` preset on
2026-08-29:

- `./DevTool.bat test "@thumbnail"` passed the bounded aggregate containing
  `MaterialThumbnailTests`, `MaterialVulkanTests`,
  `StaticMeshThumbnailTests`, `TextureThumbnailTests`, and `ThumbnailTests`.
  Direct focused runs passed 60 Thumbnail tests, 7 Material thumbnail tests,
  10 Texture thumbnail tests, and 9 Static Mesh thumbnail tests. This covers
  queue order and budgets, CPU/GPU/disk retention, persistent warm hits,
  corrupt-object recovery, cancellation, stale rejection, renderer retirement,
  generated-pixel and Vulkan-backed visual production.
- `./DevTool.bat test TerrainHeightmapTests` passed 11 of 11 tests, preserving
  Terrain Heightmap generation and framing behavior.
- `./DevTool.bat test ContentBrowserWorkflowTests` passed 61 tests with one
  platform-independent fixture scenario skipped. Reference release, refresh,
  navigation, identity replacement, and unsupported-item presentation remain
  covered by Content Browser and thumbnail-focused tests.
- `./DevTool.bat test SkeletalSceneLifecycleTests` passed its Vulkan-backed
  renderer/module lifecycle test; `./DevTool.bat test RHICommandListTests`
  passed 66 of 66 RenderCore/RHI command and retirement tests.
- `./DevTool.bat build` passed the required `all` target after the final source
  and test migrations.
- Targeted searches found no live provider/service/executor, rendered-cache,
  Content Browser asset-cache facade, or old generation-extension symbols.
  Persisted renderer strings, key field order, DDC root, PNG output, and object
  formats remain unchanged by the C++ vocabulary migration.
- `./DevTool.bat doc validate --scope changed` validated 4 changed documents;
  `./DevTool.bat doc validate --scope all` validated 135 documents; and
  `./DevTool.bat doc plan validate --scope all` validated the repository plan
  lifecycle before completion. The same checks are rerun against this completed
  state before handoff.

## Definition of Done

- `DurinEd` owns one documented Thumbnail subsystem whose final public API is
  expressed through Manager, Renderer, RenderingInfo, AssetThumbnail,
  AssetThumbnailPool, and ObjectThumbnail concepts.
- Feature modules register exact-class `DThumbnailRenderer` implementations
  without exposing feature behavior or unloadable pointers to shared pool code.
- Content Browser asset cards hold `FAssetThumbnail` references and do not own
  an asset-thumbnail cache or choose a production path.
- One bounded pool coordinates authored source decode, generated pixels,
  rendered preview, persistence, texture reuse, pinning, cancellation,
  invalidation, refresh, diagnostics, and shutdown.
- Existing supported families preserve visual output, cache identity,
  dependency invalidation, readiness, transparency, diagnostics, and recovery;
  unsupported classes preserve icon-only behavior.
- Ordinary files remain outside asset-renderer lookup and retain their current
  supported image preview.
- No old Provider/Service/Executor or split asset-cache architecture remains in
  production code, tests, or lasting documentation.
- Focused and bounded aggregate tests, required build, representative Vulkan
  rendering, persistent compatibility, corruption, lifecycle, performance,
  and documentation gates pass with evidence recorded in this plan.
- Changes are staged and committed with repository-required plan provenance
  after successful validation.

## Deferred Follow-ups

- Class-hierarchy renderer fallback and configuration-driven renderer mapping.
- Project/plugin-defined renderer discovery beyond explicit module
  registration.
- Realtime or animated thumbnails and per-frame realtime rendering budgets.
- User-authored custom thumbnails, viewport capture, and thumbnail editing.
- Multiple persistent output sizes, DPI-aware regeneration, and non-square
  thumbnail identities beyond the Stage 0 selected presentation rule.
- Package-embedded thumbnails or cook/runtime thumbnail access.
- Cross-process or remote DDC sharing and background project-wide prewarming.
- A dedicated Thumbnail inspection/rebuild UI and end-user quality settings.

## Related Documentation

- [Asset Thumbnails](../../../Editor/Architecture/AssetThumbnails.md)
- [Content Browser](../../../Editor/Architecture/ContentBrowser.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Core Task System](../../../Runtime/Core/TaskSystem.md)
- [Modular Features and Module Retirement](../../../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [Testing](../../../Agents/Testing.md)
- [Build and Run](../../../Agents/BuildAndRun.md)

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Thumbnail/`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/`
- `Engine/Source/Editor/ContentBrowser/Private/Assets/ContentBrowserThumbnailReferences.h`
- `Engine/Source/Editor/ContentBrowser/Private/Assets/ContentBrowserThumbnailReferences.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Assets/SourceImageThumbnailCache.h`
- `Engine/Source/Editor/ContentBrowser/Private/Assets/SourceImageThumbnailCache.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/MaterialEditor/Public/Thumbnail/MaterialThumbnailRenderer.h`
- `Engine/Source/Editor/MaterialEditor/Private/Thumbnail/MaterialThumbnailRenderer.cpp`
- `Engine/Source/Editor/TextureEditor/Public/Thumbnail/TextureThumbnailRenderer.h`
- `Engine/Source/Editor/TextureEditor/Public/Thumbnail/TextureCubeThumbnailRenderer.h`
- `Engine/Source/Editor/TextureEditor/Private/Thumbnail/TextureThumbnailRenderer.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Thumbnail/TextureCubeThumbnailRenderer.cpp`
- `Engine/Source/Editor/StaticMeshEditor/Public/Thumbnail/StaticMeshThumbnailRenderer.h`
- `Engine/Source/Editor/StaticMeshEditor/Private/Thumbnail/StaticMeshThumbnailRenderer.cpp`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Tests/Native/EngineTests/Private/AssetThumbnailContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SourceImageThumbnailTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/MaterialThumbnailRendererTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshThumbnailRendererTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserItemViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialRenderingTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialVulkanTests.cpp`
