# Asset Thumbnails

Summary: Define the shared Thumbnail Manager, renderer, asset-thumbnail, pool, persistence, and presentation contracts.

Modules: DurinEd, ContentBrowser, MainFrame, MaterialEditor, TextureEditor, StaticMeshEditor, SkeletalMeshEditor, LevelEditor

Last reviewed: 2026-09-03

Asset thumbnails are optional editor-derived data. They never replace authored
packages or source files, and deleting the thumbnail cache cannot lose project
content.

## Public Model And Ownership

- `DurinEd` owns `DThumbnailManager`, `DThumbnailRenderer`,
  `DDefaultSizedThumbnailRenderer`, `FThumbnailRenderingInfo`,
  `FAssetThumbnail`, and `FAssetThumbnailPool`.
- `DThumbnailManager` maps one exact asset class to one renderer generation.
  It rejects duplicate registration, owns the shared default pool, and performs
  explicit shutdown.
- Feature editor modules own their concrete renderer objects and move-only
  `FThumbnailRendererRegistrationHandle` values. Material/MaterialInstance,
  Texture2D/TextureCube, StaticMesh, SkeletalMesh, and Terrain Heightmap logic
  remains in the corresponding feature module.
- `FAssetThumbnail` is the UI-facing reference for one canonical asset identity,
  requested presentation size, and pool. It owns no asset, task, preview world,
  render target, or RHI resource.
- `FAssetThumbnailPool` owns coalescing, priority, state, generation, preview
  leasing, readback, encoding, persistence, upload, pinning, LRU retention,
  cancellation, diagnostics, and statistics.
- Persistent object-store, CPU RGBA8 transfer, and queue/index types remain
  private to `DurinEd`.

The process manager lazily creates one shared pool. Tests and isolated editor
tools may inject a local manager into a local pool. Manager and renderer public
contracts do not expose a parallel registry, cache facade, or generation
extension hierarchy.

## Identity And Output Size

Production output is fixed at 256 by 256 pixels. Requested `FAssetThumbnail`
dimensions are presentation-only: Content Browser scales the shared output and
requested width/height never enter the cache key, CPU/GPU budget accounting, or
persistent object identity. This avoids an unbounded output-size key space.

Persistent keys use a canonical little-endian encoding of:
canonical virtual path, exact class, package fingerprint, stable renderer name,
generator schema, fixed output settings, preview fixture identity/version,
shader contract, and sorted dependency fingerprints. The project-local
`DerivedDataCache/Thumbnails` store uses PNG objects and a persistent index.
A C++ type rename alone does not change cache identity.

Material and MaterialInstance sessions acquire the retained
`/Engine/Models/Sphere` fixture and attach their material to a component in the
leased preview scene. They perform no transient mesh import or private viewport
allocation. Material keys include the sorted, cycle-guarded parent-material and
texture dependency closure.
StaticMesh keys include LOD 0 framing and default-material closure. TextureCube
keys preserve wide environment orientation and visual-contract versions.
Texture2D derives fixed output from canonical pixels stored in the asset rather
than following an external reimport hint. Terrain Heightmap derives fixed
grayscale pixels from its immutable payload and revision.

Every completion revalidates asset identity, request serial, renderer generation,
key, and captured asset/resource revisions. Save, move, delete, reimport,
dependency change, resource rebuild, explicit dirty refresh, renderer
replacement, and corruption either produce a new key or reject stale work.

## Renderer Lifetime And State

Renderers capture immutable generation input on the game thread. A cold miss may
then use one renderer-owned session with these hooks:

1. load the exact asset and capture its revision;
2. poll bounded resource readiness without retaining the capture slot;
3. prepare the leased preview scene or provide canonical pixels directly;
4. validate revisions before render, readback, encoding, and publication;
5. reset preview state idempotently.

A renderer registration is qualified by a monotonically increasing generation
and the feature module's callback gate. Reset closes admission, cancels all
captured leases, resets and destroys sessions on the game thread, releases
immutable feature-owned input, and drains callbacks before returning. Queued,
waiting, rendering, readback, encoding, and already-enqueued upload completions
all reject the retired generation.

The shared preview pool owns its world, camera/view, environment value, light,
output target, capture, and readback. Scene renderers attach only session-owned
content and detach it in reset. TextureCube supplies a stable counted RHI
environment value and creates no world content. `DurinEd` contains no concrete
asset casts, readiness rules, framing rules, or feature diagnostics.

## Pool Scheduling And Budgets

Visible requests outrank prefetch, duplicate keys coalesce, and different
`FAssetThumbnail` instances for one identity share one entry and one admitted
job. Each reference increments the entry's pin count. Releasing the final
reference makes the entry evictable without synchronously releasing an in-use
RHI resource or affecting a reference held by another panel.

The default limits remain:

| Resource | Limit |
| --- | ---: |
| Queued jobs | 512 |
| Render captures per frame | 1 |
| Live preview scenes | 1 |
| Parked resource waits | 64 |
| Retained pool entries | 4096 |
| Resource poll interval | 4 frames |
| Resource wait timeout | 600 frames |
| CPU pixels | 64 MiB |
| GPU textures | 64 MiB |
| One encoded object | 16 MiB |
| Persistent objects | 256 MiB |

Jobs move through queued, loading, waiting-for-resources, rendering, readback,
encoding, uploading, and ready states. A waiting rendered job is parked and
releases the capture slot, allowing canonical-pixel or another ready rendered
job to progress. Timeout and failure are stable until identity changes or the
caller explicitly refreshes. Unreferenced inactive metadata entries use a
separate count-bounded LRU so failed or unsupported assets cannot grow the pool
without bound.

Pool statistics expose jobs, loads, waits, renders, readbacks, disk hits,
failures, retries, cancellations, evictions, uploads, live textures, queued
jobs, retained entries, pinned entries, and reference count. Statistics are
observations and do not alter scheduling or cache identity.

## Content Browser And Ordinary Files

Content Browser asset cards hold `FAssetThumbnail` references and submit only
identity, visible/prefetch priority, refresh, and presentation requests. They do
not choose a renderer or generation path. Navigation, filtering, replacement,
refresh, panel close, and shutdown deterministically release references.

Ordinary PNG/JPEG/BMP/TGA files are not assets. Their physical-path decode,
disk reuse, upload, and view remain in the Content Browser-private
`FSourceImageThumbnailCache`. Physical file identity never enters the manager,
asset pool, or asset DDC key. Texture2D assets use canonical package pixels in
`DTextureThumbnailRenderer`; an external source hint is never probed implicitly.

Pending and failed assets retain the asset icon. Ready output preserves its
transparency policy and diagnostic. Unsupported exact classes create no pool
job and remain icon-only.

## Persistence, Corruption, And Shutdown

Warm requests load compatible PNG objects and upload them without loading the
authored asset or creating a preview scene. Decode admission is bounded by the
fixed requested output before allocation, and decoded dimensions must match
that output exactly. Missing, incompatible, oversized, truncated, corrupt, or
decode-invalid objects are safe misses. Removal first proves the resolved path
remains beneath the cache root; regeneration then uses mounted authored
content. An invalid warm object is invalidated and requeued once, never retried
every frame.

MainFrame shutdown order is:

1. stop Content Browser request admission;
2. release panel `FAssetThumbnail` references;
3. unregister feature renderers in reverse composition order;
4. clear and destroy the manager-owned pool;
5. destroy the manager and unload feature/rendering modules.

All completion, cancellation, retirement, refresh, pool destruction, and
shutdown paths converge on idempotent preview reset and owning-thread RHI
release.

## Related Documentation

- [Content Browser](ContentBrowser.md)
- [Asset Packages](../../Runtime/Assets/AssetPackages.md)
- [Core Task System](../../Runtime/Core/TaskSystem.md)
- [Modular Features And Module Retirement](../../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
