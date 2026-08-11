# Editor Thumbnail Architecture Namespace Refactor Plan

Summary: Move shared thumbnail infrastructure into `Durin::Editor` and split its public contracts by responsibility without changing identity, scheduling, persistence, rendering, or unload behavior.

Last reviewed: 2026-08-12

Status: Completed
Completed: 2026-08-12

## Current Status

All stages are complete. Shared thumbnail values, identity, provider
lifetime, scheduling, object storage, rendered extensions, preview scenes,
pipelines, services, and caches now live in the flat `Durin::Editor`
namespace behind responsibility-named headers.

The selected topology separates values, persistent identity, provider lifetime,
scheduling, object storage, rendered extensions, shared preview scenes,
rendered transitions, service registration, and cache/UI publication. Old
direct header paths will be removed without forwarding headers. Concrete
MaterialEditor, TextureEditor, StaticMeshEditor, and SkeletalMeshEditor provider
types remain owned by their feature modules and only qualify the shared
`Editor` contracts.

All focused validation targets pass: `ThumbnailTests`,
`EditorAssetWorkflowTests`, `EditorRenderingTests`,
`MaterialThumbnailTests`, `TextureThumbnailTests`,
`StaticMeshThumbnailTests`, `SkeletalMeshEditorTests`, `MaterialTests`,
and `SceneImportVulkanTests`. The complete native suite passed at default
target granularity, the full editor `all` build passed, and the hidden Sandbox
startup/exit smoke passed from the same `Win64-Debug-DurinEditor` profile.
Searches and the final diff retain no old direct header, root-owned shared API,
compatibility facade, or forwarding alias.

## Goal

- Complete the `Durin::Editor` namespace boundary for shared thumbnail APIs.
- Replace the monolithic `AssetThumbnail.h` surface with responsibility-focused headers.
- Correct misleading file ownership such as `AssetThumbnailCache` containing only object-store and budget utilities.
- Separate rendered preview-scene, pipeline, service, and cache contracts so consumers include only what they use.
- Preserve every behavioral, persistent, threading, and module-lifetime contract while migrating all consumers atomically.

## Scope

- Every public and private DurinEd file under `Thumbnail/`.
- DurinEd's umbrella header and direct public include surface.
- MainFrame thumbnail-service composition and reverse-order shutdown.
- LevelEditor source-image and rendered Content Browser caches, model, item view, and presentation consumers.
- MaterialEditor, TextureEditor, StaticMeshEditor, and SkeletalMeshEditor provider, module-registration, and scoped-handle consumers.
- Native tests covering key construction, dependency closure, scheduling,
  cancellation, persistence, preview pooling, rendering, provider unload,
  Content Browser integration, and GPU publication.
- Non-archived editor architecture documentation that names shared thumbnail contracts or header ownership.

## Non-Goals

- Changing thumbnail visual output, provider names, generator schema versions,
  fixture identities or versions, shader contract versions, output settings, or default budgets.
- Changing cache-key fields, field ordering, little-endian encoding, hash
  algorithm, dependency sorting, persistent format version, object extension,
  cache root, index/object layout, PNG encoding, or eviction policy.
- Changing request coalescing, priority, state transitions, retry behavior,
  cancellation serials, admission limits, per-frame limits, or publication validation.
- Changing game-thread, render-thread, worker-thread, readback, encode, upload,
  or teardown ownership.
- Changing provider registration generation, scoped-handle reset, module unload,
  shared-service shutdown, or MainFrame reverse-composition ordering.
- Moving concrete feature-editor provider classes or feature module classes into new namespaces.
- Moving runtime asset, RHI, renderer, world, texture, or reflected object types.
- Adding compatibility aliases, deprecated spellings, facade headers at removed
  direct paths, or forwarding headers.

## Design Decisions and Invariants

### Namespace and naming

- All shared DurinEd thumbnail declarations and definitions live directly in
  `Durin::Editor`; no `Editor::Thumbnail` subnamespace is introduced.
- Existing `FAssetThumbnail*`, `FRenderedAssetThumbnail*`, and corresponding
  enum/interface names remain unchanged because the qualifiers describe the
  thumbnail responsibility rather than repeat the `Editor` namespace.
- Concrete feature-module classes retain their current names and owners. Their
  base classes, method parameters, return types, and registration handles use
  explicit `Editor::` qualification.
- Runtime-owned `Asset::FAssetData`, `FAssetPath`, `FRHITexture`, `DWorld`, and
  renderer/RHI contracts stay in their current namespaces.

### Selected public-header topology

- `AssetThumbnailTypes.h` owns provider-neutral states, priority, package
  fingerprint, output settings, budgets, request, source-image, and view values,
  plus the frozen rendered visual value contract currently embedded in the
  monolithic header.
- `AssetThumbnailKey.h` owns dependency nodes, key inputs,
  `BuildAssetThumbnailDependencyClosure()`, and `BuildAssetThumbnailCacheKey()`.
- `AssetThumbnailProvider.h` owns cancellation, immutable generation input,
  generation lease/request, provider registration and interface, provider
  handles, scoped registration, and the exact-class provider registry.
- `AssetThumbnailScheduler.h` owns scheduled jobs and the bounded scheduler.
- `AssetThumbnailObjectStore.h/.cpp` replaces `AssetThumbnailCache.h/.cpp` and
  owns object-store settings/stats/load result, the object store, budget entries,
  and `SelectAssetThumbnailBudgetEvictions()`.
- `RenderedAssetThumbnailExtension.h` retains rendered session states, updates,
  preview-view values, and provider extension interfaces, but moves them into
  `Durin::Editor` and includes only the provider/type contracts it requires.
- `RenderedAssetThumbnailPreviewScene.h` becomes the public owner of capture
  state and `FRenderedAssetThumbnailPreviewScenePool`; the existing preview-scene
  implementation file remains responsibility-matched.
- `RenderedAssetThumbnailPipeline.h/.cpp` retains only rendered jobs, start
  results, pipeline stats, and transition/persistence orchestration.
- `RenderedAssetThumbnailService.h/.cpp` becomes the owner of the live registry
  service and `GetDefaultRenderedAssetThumbnailService()`.
- `RenderedAssetThumbnailCache.h/.cpp` retains only cache stats and the
  rendered cache that owns scheduling, generation, preview leasing, upload, and
  UI texture lifetime.
- `AssetThumbnail.h/.cpp` and `AssetThumbnailCache.h/.cpp` are removed after
  their declarations and definitions move. No facade remains at those paths.
- DurinEd's umbrella header includes the new provider-neutral public headers;
  repository implementation files use the narrowest direct header rather than
  depending on the umbrella.

### Frozen behavior and data contracts

- `FAssetThumbnailState`, priority ordering, request serials, generation
  numbers, identity validation, revision validation, and all transition guards
  remain unchanged.
- Persistent cache keys remain byte-for-byte identical for identical inputs.
  Dependency closure remains sorted, cycle guarded, and invalid on missing or
  conflicting registry data.
- Object-store containment checks, atomic publication, corruption recovery,
  size limits, LRU eviction, stats, and recoverable-miss behavior remain unchanged.
- Provider registration remains exact-class and game-thread-owned. Scoped reset
  closes admission, invalidates generations, cancels leases, resets preview
  state, and destroys provider-owned objects before returning.
- Rendered cold jobs retain the queued, loading, resource-wait, rendering,
  readback, encoding, upload, and publication sequence. Warm hits still avoid
  asset load, preview-scene creation, render, and readback.
- The preview pool retains one resettable editor preview scene by default,
  provider-neutral camera/light/output ownership, render-thread capture and
  readback, and game-thread reset.
- MainFrame remains the shutdown coordinator. Feature modules unregister
  providers before workspaces; caches and the shared service drain before
  concrete module unload.
- Existing strings, diagnostics, provider names, callback order, object names,
  counters, and failure classifications remain byte-for-byte or semantically
  unchanged as applicable.

## Current Foundations and Gaps

- Shared editor transactions, workspaces, interaction, asset support, and
  preview scenes already live in `Durin::Editor`; thumbnails are the largest
  remaining shared DurinEd API family in the root namespace.
- DurinEd currently exposes five thumbnail headers. `AssetThumbnail.h` is over
  four hundred lines and combines values, identity, provider lifetime,
  scheduling, and helper functions.
- `AssetThumbnailCache.h` does not own an operational thumbnail cache; it owns
  the persistent object store and generic budget eviction, making include intent
  and ownership unclear.
- `RenderedAssetThumbnailPipeline.h` also exposes the preview-scene pool, while
  `RenderedAssetThumbnailCache.h` exposes both the live provider service and the
  rendered cache.
- The shared headers are consumed by DurinEd, MainFrame, LevelEditor, four
  feature-editor modules, their concrete providers, and multiple native-test targets.
- Existing tests already freeze identity, scheduling, persistence, module
  unload, warm/cold paths, rendered fixtures, and Content Browser presentation;
  the public migration crosses enough modules and targets to require complete-suite qualification.

## Implementation Stages

### Stage 0: Freeze topology, ownership, and validation surfaces

- [x] Inventory DurinEd thumbnail declarations, definitions, includes, modules, tests, and non-archived documentation.
- [x] Select the flat `Durin::Editor` namespace and retain responsibility-bearing symbol names.
- [x] Select the ten-header responsibility topology and source-file split.
- [x] Freeze key, persistence, scheduling, rendering, threading, and unload contracts.
- [x] Reject compatibility aliases, facade headers at removed paths, and forwarding headers.

#### Acceptance Gate

- Every existing shared declaration and definition has one selected destination.
- Every removed file has a direct replacement responsibility rather than a generic compatibility facade.
- Validation surfaces cover core contracts, Content Browser integration,
  concrete providers, module unload, rendered output, full DLL linkage, and startup/shutdown.

### Stage 1: Split values, identity, providers, and scheduling

- [x] Add `AssetThumbnailTypes.h`, `AssetThumbnailKey.h`,
  `AssetThumbnailProvider.h`, and `AssetThumbnailScheduler.h` under `Durin::Editor`.
- [x] Split `AssetThumbnail.cpp` into responsibility-matched key, provider, and scheduler implementations.
- [x] Preserve generation-lease invalidation, provider-registry state, scheduler ordering, and transition behavior exactly.
- [x] Update every direct `AssetThumbnail.h` consumer, including concrete
  provider headers and tests, to qualify the migrated core contracts and include
  the narrow direct headers.
- [x] Remove `AssetThumbnail.h/.cpp` after every declaration, definition, and consumer migrates.

#### Acceptance Gate

- `ThumbnailTests` passes key, dependency, provider, lease, cancellation, scheduler, and state-transition coverage.
- Identical contract-test inputs produce the same persistent keys and dependency order.
- Searches find no old direct include, migrated core root-namespace symbol, alias, or facade header.

### Stage 2: Rename object storage and migrate Content Browser consumers

- [x] Rename `AssetThumbnailCache.h/.cpp` to `AssetThumbnailObjectStore.h/.cpp`.
- [x] Move object-store and generic budget contracts into `Durin::Editor` without renaming their types or functions.
- [x] Update LevelEditor source-image disk/memory caches, rendered facade, model, item view, and presentation code.
- [x] Update DurinEd's umbrella header to expose the new provider-neutral header set.
- [x] Preserve persistent path containment, atomic store, corruption recovery,
  size validation, LRU selection, stats, and Content Browser fallback behavior.

#### Acceptance Gate

- `ThumbnailTests` passes source-image cache, object-store, budget, and rendered-facade coverage.
- `EditorAssetWorkflowTests` passes Content Browser request, model, and presentation coverage.
- Old `AssetThumbnailCache` filenames and broad umbrella-dependent implementation includes have no consumers.

### Stage 3: Separate rendered extension, preview, pipeline, service, and cache

- [x] Move rendered extension contracts into `Durin::Editor` with narrow provider/type includes.
- [x] Add `RenderedAssetThumbnailPreviewScene.h` and move capture state plus preview-pool declarations out of the pipeline header.
- [x] Keep `RenderedAssetThumbnailPreviewScene.cpp` as the implementation owner and qualify `Editor::FPreviewScene` directly.
- [x] Add `RenderedAssetThumbnailService.h/.cpp` and move registry-service/default-service definitions out of cache files.
- [x] Reduce `RenderedAssetThumbnailPipeline.h/.cpp` and
  `RenderedAssetThumbnailCache.h/.cpp` to their selected responsibilities.
- [x] Update DurinEd, concrete provider, and rendered contract-test consumers
  without changing warm/cold transitions or stats.

#### Acceptance Gate

- `ThumbnailTests` passes rendered extension, preview-pool, pipeline, persistence, and cache coverage.
- `MaterialTests` and `SceneImportVulkanTests` pass representative renderer-backed cold-generation and fixture coverage.
- Header dependency searches show preview-pool consumers no longer require the pipeline header and service consumers no longer require the cache header.

### Stage 4: Migrate service composition and module lifecycle

- [x] Update feature module public signatures, forward declarations, scoped
  registration handles, startup, rollback, and shutdown paths.
- [x] Update MainFrame service acquisition, composition, failure rollback, and reverse-order shutdown.
- [x] Update LevelEditor service/cache consumers and all affected native-test consumers.
- [x] Preserve exact-class routes, provider generations, unload cancellation,
  default service identity, and provider-before-workspace teardown.

#### Acceptance Gate

- `MaterialThumbnailTests`, `TextureThumbnailTests`,
  `StaticMeshThumbnailTests`, and `SkeletalMeshEditorTests` pass.
- `EditorRenderingTests` passes shared service and feature-editor integration coverage.
- Module-unload and re-registration tests prove no provider object crosses scoped-handle reset or module shutdown.

### Stage 5: Documentation, stale-surface removal, and final validation

- [x] Update `AssetThumbnails.md` and other non-archived architecture documentation with the new namespace and header ownership.
- [x] Search source, tests, and non-archived documentation for old filenames,
  broad includes, root-namespace shared contracts, aliases, and forwarding headers.
- [x] Run every focused validation target in the matrix.
- [x] Run the complete native-test suite at default target granularity because the public migration crosses test and DLL boundaries.
- [x] Complete a full editor `all` build after all header/source splits and consumer migrations.
- [x] Run a hidden Sandbox startup/exit smoke using the same Agent Build Profile.
- [x] Validate all plans and record final evidence, including any non-standard concurrency qualification required by unrelated tests.

#### Acceptance Gate

- Every validation-matrix entry passes.
- The full editor build and startup/shutdown smoke pass from the same profile.
- No stale old include, root-namespace API, compatibility surface, or undocumented topology remains.
- Repository searches and the final diff show no change to keys, cache formats,
  default values, provider names, transition ordering, thread ownership, or unload behavior.

## Validation Matrix

| Surface | Validation |
| --- | --- |
| Values, keys, dependency closure, provider registry, leases, scheduler, object store, preview pool, pipeline, cache | `ThumbnailTests` |
| Content Browser model, request selection, and presentation | `EditorAssetWorkflowTests` |
| Shared service and editor rendering integration | `EditorRenderingTests` |
| Material provider, module registration, cold generation, and unload | `MaterialThumbnailTests` |
| Texture2D source-image and TextureCube rendered providers | `TextureThumbnailTests` |
| StaticMesh provider, dependency identity, rendered cache, and unload | `StaticMeshThumbnailTests` |
| Skeletal provider registration and editor-module lifecycle | `SkeletalMeshEditorTests` |
| Representative material preview and rendered-thumbnail integration | `MaterialTests` |
| Vulkan-backed shared rendered fixture and publication | `SceneImportVulkanTests` |
| Cross-target public API and header migration | Complete native-test suite at default target granularity |
| DurinEd, MainFrame, LevelEditor, and feature-editor DLL linkage | Full editor `all` build |
| MainFrame composition and reverse-order startup/shutdown | Hidden Sandbox startup/exit smoke |
| Plan lifecycle and documentation links | All-plan validator |

## Definition of Done

- Every shared thumbnail contract lives in `Durin::Editor` and every concrete provider qualifies that owner directly.
- The selected responsibility headers and implementation files replace the old monolithic and misleading files.
- Removed direct paths have no facade, alias, forwarding header, or stale consumer.
- Cache keys, persistent objects, schemas, budgets, provider identities, visual
  fixtures, scheduling, rendering, publication, and unload behavior remain unchanged.
- Focused tests, complete native tests, full editor build, startup/shutdown
  smoke, stale-surface searches, and plan validation pass with recorded evidence.

## Deferred Follow-ups

- Review whether concrete feature-editor provider and module types should move
  into module-specific namespaces after the shared thumbnail boundary stabilizes.
- Evaluate private implementation extraction inside the rendered cache only if
  responsibility metrics remain poor after the public split; do not mix that
  behavioral refactor into this namespace migration.
- Review compile-time include metrics after migration and add forward declarations
  only where they preserve clear ownership and complete-type requirements.

## Related Documentation

- [Editor Preview Scene Namespace Refactor](EditorPreviewSceneNamespaceRefactor.md)
- [Editor Asset Support Namespace Refactor](EditorAssetSupportNamespaceRefactor.md)
- [Editor Common Namespace Refactor](EditorCommonNamespaceRefactor.md)
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Skeletal Asset Editor](../Editor/Architecture/SkeletalAssetEditor.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Thumbnail`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail`
- `Engine/Source/Editor/DurinEd/Public/DurinEd.h`
- `Engine/Source/Editor/MainFrame`
- `Engine/Source/Editor/LevelEditor`
- `Engine/Source/Editor/MaterialEditor`
- `Engine/Source/Editor/TextureEditor`
- `Engine/Source/Editor/StaticMeshEditor`
- `Engine/Source/Editor/SkeletalMeshEditor`
- `Engine/Tests/Native/EngineTests`
