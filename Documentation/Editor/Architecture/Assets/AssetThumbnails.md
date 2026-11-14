# Asset Thumbnails

Summary: Define the shared Thumbnail Manager, renderer, asset-thumbnail, pool, persistence, and presentation contracts.

Modules: DurinEd, ContentBrowser, MainFrame, MaterialEditor, TextureEditor, StaticMeshEditor, LevelEditor

Last reviewed: 2026-11-14

Asset thumbnails are optional editor-derived data. They never replace authored
packages or source files, and deleting the thumbnail cache cannot lose project
content.

## Public Model And Ownership

- `DurinEd` owns `DThumbnailManager`, `DThumbnailRenderer`,
  `FThumbnailRenderingInfo`,
  `FAssetThumbnail`, and `FAssetThumbnailPool`.
- `DThumbnailManager` maps one exact asset class to one renderer object.
  It rejects duplicate registration, owns the shared default pool, and performs
  explicit shutdown. It does not construct draw jobs.
- Feature editor modules register reflected concrete renderer classes and own
  move-only `FThumbnailRendererRegistrationHandle` values. Material/MaterialInstance,
  Texture2D/TextureCube, and StaticMesh logic
  remains in the corresponding feature module.
- `FAssetThumbnail` is the UI-facing reference for one canonical asset identity
  and pool. It owns no asset, task, preview world,
  render target, or RHI resource.
- `FAssetThumbnailPool` owns coalescing, priority, state, generation, preview
  scene borrowing, readback, upload, pinning, LRU retention,
  cancellation, diagnostics, and statistics.
- The private `FAssetThumbnailCache` owns asynchronous pixel lookup, decoding,
  encoding and persistence. It has no scheduler, renderer or capture dependency;
  the pool owns pending read futures and all rendering transitions.
- The public persistent object store is shared with ordinary-file thumbnail
  caches. Its `FThumbnailObjectStoreSettings` and budget helpers depend on no
  asset pool or preview scene. Queue/index implementations remain private to
  `DurinEd`.

`ThumbnailManager.h` declares registration, handles and shared-pool ownership.
`ThumbnailRenderer.h` declares the loaded-object renderer and its
capability/draw entrypoints. `ThumbnailDrawContext.h` contains the
concrete draw setup and resource-lifetime context. The private request queue
contains only pending `FAssetData` values and priority. The pool computes cache
keys at admission and retains only asset data, output settings, key
for admitted work. There are no renderer-generation
or request-serial counters, thumbnail cancellation tokens, or shared execution
phase protocol. Concrete renderers derive directly from
`DThumbnailRenderer`; the fixed output contract needs no intermediate base class.
All concrete renderers are `DCLASS` objects with initializer constructors.
`RegisterCustomRenderer(AssetClass, RendererClass)` validates a concrete renderer
class and constructs a transient instance through `NewObject`. It returns a
move-only registration handle, empty on failure; registration does not retain or
return diagnostic strings. The native manager
service retains registered instances and pool-owned jobs retain active instances
with GC strong references; native
smart pointers never delete a renderer. One renderer class may serve multiple
asset classes through independent registrations. The manager constructs `FThumbnailRenderingInfo` from the registered asset
class and renderer instance; asset/renderer class names, reflected classes and
the borrowed instance are available through `GetRenderingInfo`. Renderers do
not describe their own registration or return generator metadata.
Asset class names remain the unloaded-catalog lookup boundary. Production has no
instance-registration overload; synthetic catalog tests inject managed objects
through a private test access seam.

`FAssetThumbnail` and pool access use Asset Registry `FAssetData` directly.
Callers do not provide package size, modification time or format version.
Private key capture obtains the current root and transitive package closure
from the Asset Registry and validates asset identity before cache lookup.
A warm hit does not load an asset or invoke the renderer. Cache policy belongs
to the pool and private key implementation, with no renderer description phase.
The object drawing boundary is `CanVisualizeAsset(DObject*)` and
`Draw(DObject*, FThumbnailDrawContext&)`. There is no public session factory,
polymorphic generation input or caller-managed session lifetime.
`AssetThumbnailPool.h` exposes resource management without including the concrete
preview scene. The private queue coalesces pending demand and selects the next
asset without loading or drawing it. Cache admission and all admitted work
belong to the pool.

The process manager lazily creates one shared pool. Tests and isolated editor
tools may inject a local manager into a local pool. Manager and renderer public
contracts do not expose a parallel registry, cache facade, or generation
extension hierarchy.

## Identity And Output Size

Production output is fixed at 256 by 256 pixels. Content Browser scales the shared
output to its card layout. `FAssetThumbnail` has no requested-size parameters;
presentation dimensions never enter the cache key, CPU/GPU budget accounting, or
persistent object identity.

Persistent keys use a canonical little-endian encoding of canonical asset path,
exact asset class, reflected renderer class name, fixed output settings and
sorted package metadata for the root and dependencies. Private cache version 4
covers both key encoding and preview implementation changes; bump it when fixture,
shader or rendering behavior changes. This deliberately invalidates the shared
thumbnail cache rather than exposing separate per-renderer version contracts.
Older cache objects are regenerated automatically. The project-local
`DerivedDataCache/Thumbnails` store uses PNG objects and a persistent index.
Renaming the reflected renderer class changes cache identity.

Material and MaterialInstance draws acquire the retained
`/Engine/Models/Sphere` fixture and attach their material to a component in the
renderer-owned preview scene. They perform no transient mesh import or private viewport
allocation. Material keys include the sorted, cycle-guarded parent-material and
texture dependency closure.
StaticMesh keys include LOD 0 framing and default-material closure. TextureCube
keys preserve wide environment orientation and visual-contract versions.
Texture2D captures the published built GPU allocation through the shared
TextureEditor preview renderer, including compression and mip sampling. It never
reads source art or falls back to it. Cold jobs wait for pending publication,
retain an immutable allocation snapshot, and reject changed package revisions,
platform data, usage, or allocations before publication. Keys identify the
Built/Auto/RGBA presentation through the renderer class and private cache version; package fingerprints
cover saved build settings. Warm hits do not load textures or source pixels.
Image captures use the same scheduler, cancellation, readback, and persistence
budget as scene captures.

DurinEd owns `FTexturePreview`, `FTexturePreviewOptions` and
`RenderTexturePreview` in `Preview/TexturePreview.h`, under namespace `Durin`. Data
selection (Built or explicit Source), interpretation (Auto, Raw, Normal), and
channel inspection (RGBA/R/G/B/A) are independent. Auto reconstructs normal RG
with the material decoder and maps signed XYZ to display RGB; individual channels
always inspect stored values. Color sampling restores sRGB display encoding
without transforming alpha. Output fits the aspect ratio with transparent margins.
The texture editor and material graph use the same renderer as thumbnails;
material nodes retain and share bounded 256-pixel previews of built allocations.
The shared shader program belongs to DurinEd. Its module shutdown releases shared
pipelines after dependent feature consumers retire and before the render thread
stops; unloading TextureEditor does not release material graph resources.
Source/Built selection, build settings and volume slicing remain in TextureEditor.
The resource-creation slot exclusively owns the shared pipeline payload; draw
calls borrow its resolved resources without a second persistent reference set.
Interactive preview outputs use render-target and shader-resource flags only.
Thumbnail captures explicitly opt into CPU readback. Uploads remain synchronous.

Fixed preview mesh paths are shared constants in `PreviewMeshPaths`. Material
thumbnail renderers lazily request the sphere through a retained async-load
handle. The handle belongs to the renderer, so retiring one draw does not cancel
shared fixture loading. Renderer destruction cancels that handle. Cache hits do
not create a scene or start fixture loading. Material preview documents retain
their own acquisition handles; there is no synchronous editor-session mesh owner.

Draw jobs validate captured asset/resource revisions before their pixels can
be accepted for display or persistence. GPU upload results belong to the entry that initiated
them and cannot be routed to a replacement entry by asset path. Save, move, delete, reimport,
dependency change, resource rebuild, explicit dirty refresh, renderer
replacement, and corruption either produce a new key or reject stale work.

## Renderer Lifetime And State

A cold cache miss creates one private pool work record holding the renderer GC
reference and a nonblocking root `RequestAsyncLoad` handle. Loading starts at
cold admission. Once loaded, the pool validates the catalog class, calls
`CanVisualizeAsset`, then invokes `Draw` while a draw slot is available. There
is no separate render-job protocol or mandatory first-update waiting phase.

Each draw attempt starts waiting. Returning without marking completion defers the
attempt; `MarkReady` completes setup and `Fail` records a terminal reason.
`TrySetScene` returns false when another job reserves the scene, leaving the
draw waiting; an unavailable scene fails the context. There is no parallel
`expected` result to reconcile with context state. Draw state and its failure
reason live only in the context; parked records retain scheduling times without
copying draw results. Capture binding and validation execution are private pool
operations. Renderer snapshot validation is a boolean predicate; the core
reports one stale-resource reason.
Renderers combine readiness and preview setup in one draw operation.
`Draw` never waits for
asset compilation, shaders or GPU completion. Renderers retain reusable shader
preparation handles; their polling refreshes after shader reload. The context
owns configuration and validation callbacks that capture submission snapshots
by value. Root edit revisions are checked centrally by the context.
Terminal drawing failures receive a default reason at the context boundary when
needed; the pool forwards that reason without repeated fallback wrappers.
The context exposes neither generic renderer state storage nor asset loading.
When it releases a scene reservation, the scene clears mesh and material bindings
before becoming available to the next draw. Snapshot callbacks are released with
the draw, independently of reusable renderer preparation resources.

Only a ready draw reserves its renderer scene and modifies its actor. Parked
jobs continue root loading while another capture is active, and drawing is
resumed when the slot is free. At most one prepared draw is selected per pass,
so two jobs using one renderer cannot overwrite each other's scene. A ready
job keeps its exact snapshots through submission and readback. Validation runs
at capture submission and pixel acceptance rather than on every ready poll.

Material draws validate the material before requesting the shared sphere.
Scene draws request the Renderer-owned lit-thumbnail Shader preparation
profile after initial asset work. Material draws start once the material
program is accepted, overlapping remaining resource preparation without flooding
the first asset-build queue. Texture2D draws prepare `DurinEd.TexturePreview`
at high priority alongside loading.
Draw attempts defer for these CPU outputs without holding the capture slot.
The shared RenderCore preparation cache coalesces exact registered programs;
ShaderBuild performs source capture, DDC access, and compilation on workers.
Capture consumes completed outputs to initialize typed maps and RHI resources,
and cannot fall back to synchronous source compilation. TextureCube and StaticMesh
draws use the same scene profile. Shader reload and manual retry invalidate
preparation; forced reload also bypasses cached compiler outputs.
Pending PSOs defer capture to a later frame without waiting on the rendering
thread or reporting a resource failure. A reload detected before capture refreshes
the CPU preparation first. Pending capture retains its renderer scene and immutable
inputs, and cancellation invalidates any queued retry.

The sphere and StaticMesh use `RequestRenderDataAndResources()` for missing CPU
render data; thumbnail polling never calls the blocking mesh-load API.
StaticMesh draws also wait for every assigned default material's current
compiled program before declaring readiness or binding the renderer scene.
Pending, running, deferred and scheduled material requests remain parked;
terminal material failures report the underlying failure reason. Scene preparation
captures weak material identities and owned effective material snapshots. These
contain the accepted program, static properties and flattened parameter values,
including counted texture references and sampler state. Validation compares
current effective content and compilation readiness, rather than parent-chain
revision hashes or diagnostic counters. Inherited changes invalidate preparation;
changes hidden by an override preserve it. Geometry residency and package-edit
identities keep their separate existing checks.

The job retains successful root/dependency load handles through capture
validation. Destroying it cancels unfinished requests, clears renderer snapshots
and actor bindings, releases the scene reservation, then releases asset and
renderer references. Retry creates a new job. Removing the last `FAssetThumbnail`
referencer cancels UI-only queued work and destroys its preparing, capturing or
parked job immediately while retaining cached pixels. Another UI referencer or
explicit background interest keeps shared work alive.

EngineLoop owns the async-load pump. Standalone thumbnail hosts and tests must
also call `ProcessAsyncLoading`; polling a job or pool alone does not advance
package loading. The [async asset loading boundary](../../../Runtime/Assets/Packages/AssetPackages.md#runtime-lifetime)
still performs object construction, bulk validation and PostLoad on GameThread,
so this integration removes synchronous thumbnail load calls without promising
that those remaining stages cannot produce a long frame.

Registration handles retain a weak renderer identity and asset class. Reset only
removes that exact registered object, so an old handle cannot unregister a later
replacement. Withdrawal removes admission first, then notifies attached pools
to remove pending demand and retire draw jobs and upload futures.
There is no weak cancellation-ticket registry or per-request renderer identity.
Refresh, last-reference release, withdrawal, eviction and global cancellation
use one per-asset retirement operation. It cancels queue work, resets capture
output before destroying its draw state, removes parked/preparing work and
discards the upload future immediately. Texture retention remains a separate
caller policy: reference release keeps cached pixels, while refresh discards them.
The manager owns no draw state, input or session. Cache work contains immutable
inputs; workers cannot retain renderer callbacks or destroy preview state.
Pools retire old demand before replacement and admit new work against the
renderer currently registered for the asset class.
Owners must finish executing game-thread
callbacks before withdrawal; registration removal is not a concurrent-callback
barrier. The renderer object itself is destroyed by a later GC pass. Reflection
providers retain mapped code under the module lifecycle contract while their
classes and managed objects can still execute destructors.

Each renderer lazily owns one native `FThumbnailPreviewScene`, released in
`BeginDestroy`. Its `FPreviewScene` owns the preview world and render scene and
keeps the world reachable through the existing preview-world lifetime contract.
The thumbnail scene retains studio lighting and one lazily created actor.
Material and StaticMesh draw states reuse that actor and its StaticMeshComponent,
then clear mesh and material bindings on reset. They neither own nor destroy
the world or actor. Different renderer registrations have independent scenes.
There is no named fixture map or create/destroy fallback.

The asset pool owns `FThumbnailCapture`: output target, view, environment,
render submission and asynchronous readback. A draw context reserves its scene
before modifying the actor and supplies setup to the capture. Capture reset
cancels pending work and drops borrowed scene/environment/image bindings before
the owning job destroys its context. Context destruction clears feature state
and then releases the scene reservation. Standalone captures can reserve a scene
directly; both paths use the same exclusive reservation. Scene destruction
drains queued render commands.

View, environment and image-renderer setters configure pending capture data and
return `void`; changing capture setup in flight is a programming error.
`BeginCapture` validates configuration at submission. Scene binding remains
fallible. Completed pixels enter `CompleteCapture`, which validates pixels and
accepts the rendering-to-ready transition before scheduling independent cache
encoding. The pool validates draw snapshots once at this acceptance boundary.
Public views expose only `None`, `Pending`, `Ready` and `Failed`. Unsupported
assets use `None` and retain the ordinary asset icon. All accepted work without a
usable texture is `Pending`; `Ready` requires a usable GPU texture. Withdrawing
demand preserves ready textures, while explicit refresh discards them.
No request serial is exposed or propagated internally.
Each pool entry stores its public state and failure diagnostic. The pool builds
views directly from that entry; the queue owns no execution state or diagnostics.
GPU readback has its own private pending state and persistence runs independently.
Each resource entry owns its pending upload future. The render command owns the
promise, pixels and RHI resources and only fulfills that future; it never writes
back through a pool pointer, asset-path completion queue or request identifier.
Refresh, cancellation, clear and renderer withdrawal discard the consumer.
Later completion therefore only retires old resources. The pool polls futures
it still owns and registers their textures with the UI on the game thread.
TextureCube binds its renderer scene and supplies an immutable allocation
snapshot as a counted RHI environment value. Texture2D supplies an image renderer
without creating or binding any preview world. The shared capture implementation
contains no concrete asset casts, readiness rules, framing rules, or feature diagnostics.

Texture-dependent draws also retain the actual successful allocation snapshots.
Cube uses its fixed reference for capture; Material retains all built-in texture
role dependencies and observes texture input/completion/close events. Any observed
change invalidates delayed acceptance, including a replacement that leaves the
stable binding pointer unchanged. Pending updates prevent readiness. Texture
texture request revisions are not part of generic task identity; package,
material and mesh versions retain their own existing contracts. The engine frame
pump advances texture ownership independently of these draw snapshot checks.

## Pool Scheduling And Budgets

The editor calls `Tick` once after UI requests are submitted. A tick resets its
work budgets, drains completions, admits bounded work and evicts unused resources.
It then clears visibility and demotes queued priorities for the next batch of
requests; UI code does not pair pool begin/end calls. `AccessTexture` submits or
reprioritizes a request and returns its current public view. `Find` and reference
`GetView` are passive queries; lightweight references also offer `AccessTexture`.

The pool retains display resources in `FThumbnailInfo` entries and pending cache
reads separately as work plus pixel futures. A completed read selects the warm
upload path when it contains pixels; a miss starts a draw job. There is no
intermediate generation orchestrator or cross-layer load/render/capture protocol.
The pool can admit more reads while earlier I/O is pending. Canceling an asset
removes its pending future without waiting for background I/O.

Visible requests outrank prefetch, duplicate keys coalesce, and different
`FAssetThumbnail` instances for one identity share one entry and one admitted
job. Repeated equal asset data coalesces; changed asset data replaces and retires
the old work. Registry changes are consumed before fresh demand is accepted.
Queued work captures current package metadata at admission, so unrelated changes
do not discard demand whose dependencies have not yet been queried. Explicit
refresh retires the current work before the same asset can be admitted again. Each reference increments the entry's pin count. Releasing the final
reference makes the entry evictable without synchronously releasing an in-use
RHI resource or affecting a reference held by another panel. Explicit `Prefetch`
interest keeps background generation alive after the last UI reference is
released; global cancellation and renderer retirement still cancel that work.

Preview scenes are retained per used 3D renderer registration rather than limited
by a pool-wide scene count. Cache hits, parked loading and 2D image captures
create no scene. The default pool limits remain:

| Resource | Limit |
| --- | ---: |
| Queued jobs | 512 |
| Render captures per frame | 1 |
| Active captures per pool | 1 |
| Parked resource waits | 64 |
| Retained pool entries | 4096 |
| Resource poll interval | 4 frames |
| Resource wait timeout | 600 frames |
| Admission steps per frame | 8 |
| Game-thread scheduling allowance | 2 ms |
| Pending cache queries | 8 |
| CPU pixels | 64 MiB |
| GPU textures | 64 MiB |
| One encoded object | 16 MiB |
| Persistent objects | 256 MiB |

Queue entries exist only until admission or removal. Cache futures, preparing
and parked draws, capture work and upload futures represent actual pending work
in the pool; they do not mirror a queue state machine. GPU readback remains
internal to capture; encoding and saving are independent background tasks. Preparation and capture are independent.
A waiting or ready rendered job is parked without binding a scene. One capture retains its draw job through
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
only poll task completion. Up to eight cache queries are retained.
Up to eight queued/encoding saves are
retained in addition to an active write. Decoded-read reservations and retained
save buffers share the CPU pixel budget. The pool conservatively includes
unconsumed pixel futures when admitting reads or optional saves. When save
admission is exhausted, the optional cache save is skipped; display and capture remain available. Encoded
input is additionally bounded by the maximum object size. All generation uses
this asynchronous pipeline; tests drain its tasks explicitly when observing
persistent output. There is no separate synchronous or generated-pixel path.

Interactive stores batch index mutations, publishing after sixteen mutations,
when the save lane becomes idle, or during teardown. Direct stores retain
per-write index publication. Corrupt-object removal publishes immediately.
A crash before a batch flush can lose recent cache entries, requiring safe
regeneration; authored content is unaffected. Disk-budget accounting maintains
an incremental byte total, including replacements and removals.

New pixels are validated against the current request and resource revisions on
the game thread and submitted for UI upload without waiting for PNG encoding or
cache publication. Cache failures do not change a display-ready request to failed.
Workers retain only cache state and immutable inputs; renderer draws and pool
state updates stay on the game thread. Cache write
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
whole-project generation. Explicitly injected pools do not pre-generate by default.
Every pool observes catalog changes for invalidation, reusing dependency
package paths captured from admitted work. It refreshes affected roots and
dependents, including retained textures with no current UI references. Missing
journal coverage triggers conservative full invalidation. Browser panels retain
no separate dependency closure.

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
Each view frame releases references outside the visible and bounded prefetch
range, so scrolling does not pin every previously visited card against eviction.

Ordinary PNG/JPEG/BMP/TGA files are not assets. Their physical-path decode,
disk reuse, upload, and view remain in the Content Browser-private
`FSourceImageThumbnailCache`. Physical file identity never enters the manager,
asset pool, or asset DDC key. Texture2D assets use built GPU allocations in
`DTextureThumbnailRenderer`; source art and external source hints are never used.

Pending and failed assets retain the asset icon. Ready output preserves its
transparency information and diagnostic. Transparency is determined from accepted
RGBA8 pixels on both cold and warm upload paths. Unsupported exact classes create no pool
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
normal cancellation and refresh never wait for cache tasks. Cancellation
withdraws demand and discards result consumers; submitted GPU work still retires
safely, and cache reads may finish without publishing into a replacement request.
Valid pixels accepted before retirement may finish encoding and persistence.
UI visibility does not determine cache validity; snapshot validation and the
immutable cache key prevent invalid data from being accepted or reused.

## Related Documentation

- [Content Browser](ContentBrowser.md)
- [Asset Packages](../../../Runtime/Assets/Packages/AssetPackages.md)
- [Core Task System](../../../Runtime/Core/TaskSystem.md)
- [Modular Features And Module Retirement](../../../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
