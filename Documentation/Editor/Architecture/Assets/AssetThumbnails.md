# Asset Thumbnails

Summary: Define the shared Thumbnail Manager, renderer, asset-thumbnail, pool, persistence, and presentation contracts.

Modules: DurinEd, ContentBrowser, MainFrame, MaterialEditor, TextureEditor, StaticMeshEditor, LevelEditor

Last reviewed: 2026-10-16

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
  Texture2D/TextureCube, and StaticMesh logic
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
Texture2D captures the published built GPU allocation through the shared
TextureEditor preview renderer, including compression and mip sampling. It never
reads source art or falls back to it. Cold sessions wait for pending publication,
retain an immutable allocation snapshot, and reject changed package revisions,
platform data, usage, or allocations before publication. Keys identify the
Built/Auto/RGBA presentation and its shader/schema versions; package fingerprints
cover saved build settings. Warm hits do not load textures or source pixels.
Image captures use the same scheduler, cancellation, readback, and persistence
budget as scene captures.

TextureEditor owns `FTexturePreviewOptions` and `RenderTexturePreview`. Data
selection (Built or explicit Source), interpretation (Auto, Raw, Normal), and
channel inspection (RGBA/R/G/B/A) are independent. Auto reconstructs normal RG
with the material decoder and maps signed XYZ to display RGB; individual channels
always inspect stored values. Color sampling restores sRGB display encoding
without transforming alpha. Output fits the aspect ratio with transparent margins.
The texture editor and material graph use the same renderer as thumbnails;
material nodes retain and share bounded 256-pixel previews of built allocations.

`DEditorEngine` owns `FPreviewMeshResources` for the editor session. Before
creating the shell, it acquires `/Engine/Models/Sphere.Sphere` and
`/Engine/Models/Box.Box`, finishes their mesh compilation or CPU payload loading,
and initializes their render resources. A single startup render-command flush
completes resource initialization when RHI is available; non-rendering hosts
prepare CPU data only. Initialization is idempotent. Partial failures retain
available assets and emit a diagnostic without preventing editor startup.

Thumbnail sessions retain async-load handles and material preview documents acquire
`FAssetRetentionService` handles against these same resident objects. Closing
documents, resetting captures, and garbage collection do not release the
editor's handles or require rebuilding their buffers. The editor releases its
ownership after the shell and editor subsystems retire, before renderer shutdown.
Isolated tools without an editor-session owner retain their acquisition-based
lifetime. This fixed pair is shared outside individual thumbnail pool budgets.

Every completion revalidates asset identity, request serial, renderer generation,
key, and captured asset/resource revisions. Save, move, delete, reimport,
dependency change, resource rebuild, explicit dirty refresh, renderer
replacement, and corruption either produce a new key or reject stale work.

## Renderer Lifetime And State

Renderers capture immutable generation input on the game thread. A cold miss may
then use one renderer-owned session with these hooks:

1. request asynchronous loading of the exact asset;
2. poll bounded resource readiness without retaining the capture slot;
3. prepare the leased preview scene or provide canonical pixels directly;
4. validate revisions before render, readback, encoding, and publication;
5. reset preview state idempotently.

Material/MaterialInstance, StaticMesh, Texture2D and TextureCube sessions submit
`RequestAsyncLoad` from `Load()` and return `WaitingForResources` immediately,
including for resident assets. `PollResources()` observes request completion,
captures asset revisions once, then checks compilation and render readiness.
Material sessions validate the material before requesting the shared sphere.
Scene sessions request the Renderer-owned lit-thumbnail Shader preparation
profile after initial asset work. Material sessions start once the material
program is accepted, overlapping remaining resource preparation without flooding
the first asset-build queue. Texture2D sessions prepare `TextureEditor.Preview`
at high priority alongside loading.
`PollResources()` waits for these CPU outputs without holding the capture slot.
The shared RenderCore preparation cache coalesces exact registered programs;
ShaderBuild performs source capture, DDC access, and compilation on workers.
Capture consumes completed outputs to initialize typed maps and RHI resources,
and cannot fall back to synchronous source compilation. TextureCube and StaticMesh
sessions use the same scene profile. Shader reload and manual retry invalidate
preparation; forced reload also bypasses cached compiler outputs.
Pending PSOs defer capture to a later frame without waiting on the rendering
thread or reporting a resource failure. A reload detected before capture refreshes
the CPU preparation first. Pending capture retains its leased scene and immutable
inputs, and cancellation invalidates any queued retry.

The sphere and StaticMesh use `RequestRenderDataAndResources()` for missing CPU
render data; thumbnail polling never calls the blocking mesh-load API.
StaticMesh sessions also wait for every assigned default material's current
compiled program before declaring readiness or leasing the capture scene.
Pending, running, deferred and scheduled material requests remain parked;
terminal material failures report a qualified diagnostic. Scene preparation
captures weak material identities and render-state revisions, and later
validation rejects changed or no-longer-current materials before publication.

Sessions retain successful load handles through capture validation. Reset and
destruction cancel unfinished requests and release those handles. Removing the
last `FAssetThumbnail` referencer cancels UI-only queued work and releases its
preparing, capturing or parked session immediately, while retaining cached
pixels. Another UI referencer or explicit background interest keeps shared
work alive. Scene-only cleanup during preparation preserves the
load handles until the session is reset.

EngineLoop owns the async-load pump. Standalone thumbnail hosts and tests must
also call `ProcessAsyncLoading`; polling a session or pool alone does not advance
package loading. The [async asset loading boundary](../../../Runtime/Assets/AssetPackages.md#runtime-lifetime)
still performs object construction, bulk validation and PostLoad on GameThread,
so this integration removes synchronous thumbnail load calls without promising
that those remaining stages cannot produce a long frame.

A renderer registration is qualified by a monotonically increasing generation.
The host stops thumbnail consumers and drains work before unloading renderer code. Reset closes admission, cancels all
captured leases, resets and destroys sessions on the game thread, releases
immutable feature-owned input. Owners must finish any executing callbacks before
reset; registration removal is not a concurrent-callback barrier. Queued,
waiting, rendering, readback, encoding, and already-enqueued upload completions
all reject the retired generation.

The shared preview pool owns its world, camera/view, environment value, light,
output target, capture, and readback. It can retain generic named Actor fixtures
for its lifetime. Material sessions reuse one fixture and its StaticMeshComponent,
clearing mesh and material bindings in reset. Other session-owned scene content
is detached in reset. Isolated hosts without reusable fixtures retain the
create/destroy fallback. TextureCube supplies an immutable allocation snapshot with a fixed counted RHI
environment value and creates no world content. The shared capture implementation
contains no concrete asset casts, readiness rules, framing rules, or feature diagnostics.

Texture-dependent sessions also retain the actual successful allocation snapshots.
Cube uses its fixed reference for capture; Material retains all built-in texture
role dependencies and observes texture input/completion/close events. Any observed
change invalidates delayed acceptance, including a replacement that leaves the
stable binding pointer unchanged. Pending updates prevent readiness. Texture
request revisions are not part of the generic session revision fields; package,
material and mesh versions retain their own existing contracts. The engine frame
pump advances texture ownership independently of these read-only session checks.

## Pool Scheduling And Budgets

Visible requests outrank prefetch, duplicate keys coalesce, and different
`FAssetThumbnail` instances for one identity share one entry and one admitted
job. Each reference increments the entry's pin count. Releasing the final
reference makes the entry evictable without synchronously releasing an in-use
RHI resource or affecting a reference held by another panel. Explicit `Prefetch`
interest keeps background generation alive after the last UI reference is
released; global cancellation and renderer retirement still cancel that work.

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
| Admission steps per frame | 8 |
| Game-thread scheduling allowance | 2 ms |
| Pending cache queries | 8 (at most 7 ordinary) |
| CPU pixels | 64 MiB |
| GPU textures | 64 MiB |
| One encoded object | 16 MiB |
| Persistent objects | 256 MiB |

Jobs move through queued, loading, waiting-for-resources, rendering, readback,
encoding, uploading, and ready states. Preparation and capture are independent.
A waiting or ready rendered job is parked without owning the scene. One capture retains its session through
readback and validation while other assets continue cache lookup and loading.
Preparation capacity applies backpressure instead of failing queued assets.
The pool consumes multiple cache results and admits more work within the
per-frame step and elapsed-time limits. The time allowance is cooperative: an
individual asset callback, load finalization, or render submission is not
preempted. Current-frame visibility reprioritizes queued and ready work.
Timeout and failure are stable until identity changes or the caller explicitly
refreshes. Unreferenced inactive metadata entries use a
separate count-bounded LRU so failed or unsupported assets cannot grow the pool
without bound.

Rendered captures enqueue an RHI texture readback and remain `ReadbackPending`
until later polling publishes tightly packed pixels. `PollCapture` returns
`std::expected<std::optional<FByteBuffer>, std::string>`: an empty optional while
idle or pending, owned pixels once on completion, or a terminal error. Consuming
the pixels returns the capture to idle. Neither submission nor
polling waits for GPU completion. Vulkan retains the mapped range with its GPU
submission ticket, invalidates host memory only after completion, and retires
the range on the RHI owner. Cancellation discards pixel publication but preserves
GPU retirement prerequisites. Unsupported backends and exhausted readback
capacity fail the request rather than falling back to synchronous readback.
The Vulkan context admits at most eight pending requests and 64 MiB of pending
readback ranges; thumbnail scheduling still uses one active capture.

The interactive pool uses a serial `BlockingIO` task lane for cache index
initialization, object reads, PNG decoding, atomic writes, index updates,
and disk eviction. PNG encoding runs independently on a worker, so encoding a
previous result cannot hold the cache I/O lane. Pending queries take priority
over queued writes; an already-running I/O operation is not preempted. Frames
only poll task completion. Up to eight cache queries are retained, reserving
one slot for the generated-pixel lane. Up to eight queued/encoding saves are
retained in addition to an active write. Decoded-read reservations and retained
save buffers share the CPU pixel budget. When save admission is exhausted, the
optional cache save is skipped; display and capture remain available. Encoded
input is additionally bounded by the maximum object size. The private generation
coordinator retains a synchronous mode for offline opaque-object generation;
the interactive pool always selects background mode.

Interactive stores batch index mutations, publishing after sixteen mutations,
when the save lane becomes idle, or during teardown. Direct stores retain
per-write index publication. Corrupt-object removal publishes immediately.
A crash before a batch flush can lose recent cache entries, requiring safe
regeneration; authored content is unaffected. Disk-budget accounting maintains
an incremental byte total, including replacements and removals.

New pixels are validated against the current request and resource revisions on
the game thread and submitted for UI upload without waiting for PNG encoding or
cache publication. Cache failures do not change a display-ready request to failed.
Workers retain only cache state, immutable inputs, and cancellation tokens;
renderer sessions and scheduler transitions stay on the game thread. Cache write
success, failure, and skipped admission are reported separately in generation
statistics.

Pool statistics expose jobs, loads, waits, renders, readbacks, disk hits,
failures, retries, cancellations, evictions, uploads, live textures, queued
jobs, retained entries, pinned entries, and reference count. Statistics are
observations and do not alter scheduling or cache identity.

The process manager enables low-priority pre-generation for added or modified
supported assets from the Asset Registry change journal. Package metadata is
resolved during bounded admission, and work uses the ordinary cache and renderer
pipeline. Initial catalogs, full refreshes and journal rollover never trigger
whole-project generation. Explicitly injected pools do not subscribe by default.

## Content Browser And Ordinary Files

MainFrame advances the shared asset pool once around workspace drawing, even
when Content Browser is closed. Individual consumers do not advance that pool.
AssetPicker requests only visible candidate rows and the hovered selection;
rows show a small preview with the asset name and package path, while tooltips
show a larger preview, exact identity, and class. These requests use the same
fixed-size cached output as Content Browser and stay pinned by visible priority
for the host frame. Pending, failed, and unsupported picker entries show a file
icon without changing selection or synchronously loading candidates.

Content Browser asset cards hold `FAssetThumbnail` references and submit only
identity, visible/prefetch priority, refresh, and presentation requests. They do
not choose a renderer or generation path. Navigation, filtering, replacement,
refresh, panel close, and shutdown deterministically release references.

Ordinary PNG/JPEG/BMP/TGA files are not assets. Their physical-path decode,
disk reuse, upload, and view remain in the Content Browser-private
`FSourceImageThumbnailCache`. Physical file identity never enters the manager,
asset pool, or asset DDC key. Texture2D assets use built GPU allocations in
`DTextureThumbnailRenderer`; source art and external source hints are never used.

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
content. An invalid warm object is invalidated on the worker and returned as cold
work for the same request, never retried every frame. Warm completions revalidate
request identity and renderer generation before upload.

MainFrame shutdown order is:

1. stop Content Browser request admission;
2. release panel `FAssetThumbnail` references;
3. unregister feature renderers in reverse composition order;
4. clear and destroy the manager-owned pool;
5. destroy the manager and unload feature/rendering modules.

All completion, cancellation, retirement, refresh, pool destruction, and
shutdown paths converge on idempotent preview reset and owning-thread RHI
release. Pool destruction drains encoding and cache I/O, including index publication,
before module unload;
normal cancellation and refresh never wait for cache tasks. Canceled writes are
discarded before encoding/publication when cancellation is observed, and canceled
read results cannot publish into a replacement request.

## Related Documentation

- [Content Browser](ContentBrowser.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Core Task System](../../../Runtime/Core/TaskSystem.md)
- [Modular Features And Module Retirement](../../../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
