# Asset Thumbnails

Summary: Define thumbnail requests, providers, caches, rendering, invalidation, and Content Browser consumption.

Modules: DurinEd, LevelEditor, RenderCore, Renderer, TextureEditor

Content Browser thumbnails are optional editor derivatives. They never replace
authored packages or source files, and deleting the thumbnail cache cannot lose
project content.

## Ownership And Request Boundary

- `DurinEd` owns the provider-neutral request, view, provider registry,
  rendered-thumbnail cache, persistent object store, preview scene pool, and
  resource budgets. Every shared contract lives in the flat
  `Durin::Editor` namespace; concrete feature-editor providers remain in their
  owning modules under `Durin::Editor::<Feature>`.
- `Editor::FAssetThumbnailProviderRegistry` owns the one long-lived registration
  boundary. A rendered cache receives that registry and resolves providers when capturing each
  request, so cache construction order does not snapshot or fork provider state.
- `Editor::FRenderedAssetThumbnailCache` is the sole generation/cache owner.
  Request coalescing, priority admission, state transitions, persistence, preview
  sessions, upload, and eviction are private implementation mechanics and are not
  module extension surfaces.
- Rendered providers may register a module-owned generation extension through a
  move-only scoped handle. A persistent hit remains entirely in the shared core;
  a cold miss receives one provider-owned session for load, readiness, preview
  setup, revision validation, and diagnostics.
- Providers register by exact asset class on the game thread. A request
  captures an immutable provider input, provider generation, and request serial
  before asynchronous work begins. Asset and resource revisions are captured
  as the job loads and reaches resource-dependent transitions.
- `MaterialEditor` registers two exact classes that share the material provider.
  `TextureEditor` registers authored Texture2D source selection and the TextureCube
  provider with its wide view-environment presentation. Texture2D and
  supported source files retain their source-image decode path behind the same
  Content Browser request/view lifecycle.
- Material and texture ordinary C++ implementation types live in
  `Durin::Editor::Material` and `Durin::Editor::Texture`.
- `StaticMeshEditor` owns the StaticMesh rendered extension and immutable LOD 0
  framing inputs. Its unified module integration installs the exact-class asset
  route and thumbnail provider together. The provider never stores a mesh
  pointer in queued work; loading, resource readiness, and revision capture
  begin only after a persistent miss.
- LevelEditor owns Content Browser item presentation, its facade, and the
  exact-class canonical `DTerrainHeightmap` provider. That provider generates
  fixed 256x256 grayscale pixels from the immutable payload, includes revision
  and generator schema in identity, and uses no source image or Renderer state.
  LevelEditor otherwise asks the live registry for a source-image request or
  rendered authored-asset
  fingerprint. Unsupported
  classes issue no thumbnail job and retain their normal asset icon.

Material and MaterialInstance provider code, sessions, dependency closure, and
diagnostics live in `MaterialEditor`. Texture2D authored-source selection plus
TextureCube provider code, sessions, orientation, wide-environment contract,
and diagnostics live in `TextureEditor`. StaticMesh provider code, generation
sessions, visual contracts, and diagnostics live in `StaticMeshEditor`. The
default registry constructs no concrete provider. Moving ownership did not change
any rendered provider name, key field, schema, fixture, shader, or output policy.

The public headers follow responsibility rather than one broad thumbnail facade:
`AssetThumbnailTypes.h`, `AssetThumbnailKey.h`, and
`AssetThumbnailProvider.h` own core contracts;
`AssetThumbnailObjectStore.h` owns persistence and budget selection; rendered
extension, preview-scene, and cache contracts retain public headers. Queue and
generation state-machine types are private to the cache implementation.
The former `AssetThumbnail.h` and `AssetThumbnailCache.h` paths no longer
exist.

The shared preview pool owns its world, provider-neutral camera/view, optional
RenderCore view environment, lighting, output target, render command, and
readback. Scene-backed cold sessions such as Material and StaticMesh attach
their own Actor/Components to the leased world and remove them in
`ResetPreview`. TextureCube installs no world content: it supplies a counted
stable RHI texture reference as one submission-local environment value. The
core cache therefore has no concrete asset includes, casts, active pointers,
readiness branches, framing branches, or diagnostics. Material and StaticMesh
retain transparent-black capture while TextureCube retains opaque output.

## Extension Registration And Unload

The registry owns one live exact-class registration set. Request clients created before
or after a provider registration resolve that registry when they capture work;
they do not snapshot provider objects. Duplicate exact-class registration fails
without replacing the current provider. Replacement requires resetting the old
handle and registering again, which assigns a later provider generation.

Scoped registration accepts unique ownership. Reset first removes the exact
class from admission and invalidates its generation. It then cancels every
captured core lease, calls the provider session's idempotent preview reset on the
game thread, and destroys the session, immutable generation input, and extension
before returning. A queued or in-flight job retains only a cancelled DurinEd
lease. Loading, resource wait, render, readback, encode, upload, and publication
all reject that lease or its stale generation.

GPU upload tickets copy only core-owned cancellation, provider generation,
request serial, and asset identity. The game-thread drain re-resolves the live
registry registration before exposing the texture to the UI backend, preventing
an upload queued by an unloaded or replaced extension from publishing.

The extension owns exact-class capture, deterministic key fields, immutable
input, asset load and type checks, readiness and revision polling, scene-backed
content or value-only environment selection, validation, and asset-qualified
diagnostics. Its session may configure only the preview scene leased to that
cold job and must detach its own scene content during reset. `DurinEd` retains
scheduling, coalescing, cache lookup,
scene leasing, capture, readback, encode/decode, persistence, UI upload, budgets,
and publication. Provider objects never cross module unload.

## Identity And Invalidation

Rendered keys include the normalized virtual asset path, exact class, package
fingerprint, provider and generator schema, fixed output settings, preview
fixture identity, and shader/visual-contract version. Material keys also
include a sorted, cycle-guarded Asset Registry closure of parent-material and
texture package fingerprints. TextureCube keys include the authored cube
package fingerprint; its ready build/resource revision is revalidated across
the in-flight capture rather than persisted as a separate key field.
StaticMesh keys include the mesh package plus the sorted transitive fingerprints
of default materials and their texture dependencies, as well as the fixed
bounds-framing, transparent-output, preview-fixture, and shader contracts.

Every asynchronous completion revalidates the key, provider generation,
request serial, asset identity, and asset/resource revisions. Save, move,
delete, import, dependency edits, resource rebuilds, generator changes, and
cache corruption therefore become cancellation, a safe miss, or a new key;
stale work cannot replace a newer result.

## Scheduling And Lifetime

Only requested cards enter the scheduler. Visible requests outrank prefetch
requests, duplicate keys coalesce, and a newer serial cancels replaced work.
The default limits are:

| Resource | Default limit |
| --- | ---: |
| Queued jobs | 512 |
| Concurrent source decodes | 4 |
| UI uploads per frame | 2 |
| Rendered captures per frame | 1 |
| Live rendered preview scenes | 1 |
| Parked rendered resource waits | 64 |
| Parked resource poll interval | 4 frames |
| Parked resource wait timeout | 600 frames |
| CPU pixels | 64 MiB |
| GPU thumbnail textures | 64 MiB |
| One encoded object | 16 MiB |
| Persistent thumbnail objects | 256 MiB |

Rendered jobs move through queued, loading, resource-wait, rendering, readback,
encoding, and ready states. A resource-waiting job is parked without retaining
the active render slot or preview scene, so another rendered request can load
or capture while the parked session remains generation-qualified. Parked jobs
are polled at a bounded interval, visible ready jobs resume first, and a bounded
wait becomes one stable failure rather than monopolizing the queue.
Preview-scene mutation and asset loading occur on the game thread; rendering and
readback occur on the rendering thread; encoding and atomic publication may run
on workers. Completed CPU/GPU entries and disk objects use least-recently-used
eviction, while active or pinned entries are not selected. Closing Content
Browser or unloading a provider cancels active and parked work and rejects
stale completion before releasing scenes and GPU resources on their owning
threads.

The pool snapshots the view and optional environment independently when a
capture is accepted. Reset advances the cancellation generation, clears pixel
and diagnostic state, restores the default view, and releases the environment
reference. Success, failed preparation, failed render, cancellation, provider
removal, pool replacement, and shutdown all converge on this idempotent reset,
so a later scene-backed job cannot inherit an earlier TextureCube environment.

Refresh, navigation, rename, move, reimport, delete, panel close, and editor
shutdown cancel the current request generation. Cancellation advances the UI
upload serial too, so an already-enqueued render-thread upload cannot register a
stale texture when it returns to the game thread. Existing ready textures remain
bounded by the GPU LRU until their identity changes, they are evicted, or the
cache is cleared.

MainFrame owns shutdown sequencing rather than any provider. It stops Content
Browser admission, unregisters StaticMeshEditor, TextureEditor, MaterialEditor,
and LevelEditor integrations in reverse composition order, then drains and
destroys the shared caches and service before concrete module unload. Each
module removes its thumbnail handles before its workspace handle, making queued,
loading, waiting, rendering, readback, encoding, and uploading work incapable of
calling provider code after removal. Removing one module does not shut down the
service or registrations owned by the others.

## Persistence And Recovery

Both source and rendered outputs use the project-local
`DerivedDataCache/Thumbnails` object/index domain documented in
[Asset Packages](../../Runtime/Assets/AssetPackages.md). A rendered cold request
loads its asset, captures once, reads back once, encodes PNG, and atomically
publishes the object. A warm request decodes that PNG and uploads it without
loading the authored asset, creating a preview scene, rendering, or reading
back.

Missing, incompatible, oversized, truncated, or corrupt indexes and objects
are ordinary misses. Invalid entries are removed only after their resolved
paths are proven to remain beneath the thumbnail cache root. The next request
regenerates from mounted authored content; persistence failure does not turn
valid in-memory pixels into a failed thumbnail. A size-valid object whose PNG
payload fails decoding is invalidated and requeued once through the cold path;
the corrupt object cannot become a per-frame retry loop.

## Presentation And Failure

Grid view scales the fixed 256-by-256 output to the current card size. Pending
states retain the asset icon with nonblocking progress. Invalid material data,
missing dependencies, failed texture builds, render failures, and readback
failures retain the icon and expose one stable asset-qualified diagnostic.
Failures are not persisted as successful output and do not retry every frame;
retry requires a changed key, refresh, or explicit retry.

A required TextureCube environment is resolved on the rendering thread. A null
target, incompatible dimension, or unavailable renderer resource fails the
render result before readback; a clear image or black fallback is never
persisted as a successful TextureCube thumbnail.

Material and MaterialInstance previews load and retain the shared
`/Engine/Models/Sphere` mesh before resource polling, wait for both that mesh
and referenced textures to become render-ready, and use the resolved runtime
material values. A scene-backed capture with visible geometry but no successful
geometry draw is rejected before readback publication, so a transparent clear
caused by unavailable shader, pipeline, or mesh resources cannot enter the
thumbnail object store. TextureCube previews use an opaque 100-degree wide
environment view and the orientation contract in
[Cube Textures](../../Runtime/Rendering/CubeTextures.md).
They create no Actor, Component, StaticMesh, primitive proxy, bounds, or
visibility record. Content Browser cards never own live viewports, worlds, or
per-card preview meshes.

StaticMesh previews use the asset's validated LOD 0 bounds, a deterministic
elevated three-quarter camera, a compact image margin, the asset's default
positional material slots, and transparent output that blends with the active
Content Browser theme. Invalid bounds, unavailable or failed render
resources, and revision changes during render, readback, encoding, or
publication preserve the StaticMesh icon and publish no persistent success.
