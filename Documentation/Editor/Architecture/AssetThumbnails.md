# Asset Thumbnails

Content Browser thumbnails are optional editor derivatives. They never replace
authored packages or source files, and deleting the thumbnail cache cannot lose
project content.

## Ownership And Request Boundary

- `DurinEd` owns the provider-neutral request, view, provider registry,
  scheduler, rendered-generation pipeline, persistent object store, preview
  scene pool, and resource budgets.
- Providers register by exact asset class on the game thread. A request
  captures an immutable provider input, provider generation, and request serial
  before asynchronous work begins. Asset and resource revisions are captured
  as the job loads and reaches resource-dependent transitions.
- Material and MaterialInstance share the material provider. TextureCube uses
  the cube provider and its reflection-vector preview pass. Texture2D and
  supported source files retain their source-image decode path behind the same
  Content Browser request/view lifecycle.
- LevelEditor owns only Content Browser item presentation and the facade that
  routes a source-image request or an authored-asset fingerprint. Unsupported
  classes issue no thumbnail job and retain their normal asset icon.

## Identity And Invalidation

Rendered keys include the normalized virtual asset path, exact class, package
fingerprint, provider and generator schema, fixed output settings, preview
fixture identity, and shader/visual-contract version. Material keys also
include a sorted, cycle-guarded Asset Registry closure of parent-material and
texture package fingerprints. TextureCube keys include the authored cube
package fingerprint; its ready build/resource revision is revalidated across
the in-flight capture rather than persisted as a separate key field.

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
| CPU pixels | 64 MiB |
| GPU thumbnail textures | 64 MiB |
| One encoded object | 16 MiB |
| Persistent thumbnail objects | 256 MiB |

Rendered jobs move through queued, loading, resource-wait, rendering, readback,
encoding, and ready states. Preview-scene mutation and asset loading occur on
the game thread; rendering and readback occur on the rendering thread; encoding
and atomic publication may run on workers. Completed CPU/GPU entries and disk
objects use least-recently-used eviction, while active or pinned entries are
not selected. Closing Content Browser or unloading a provider cancels pending
work and rejects stale completion before releasing scenes and GPU resources on
their owning threads.

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
valid in-memory pixels into a failed thumbnail.

## Presentation And Failure

Grid view scales the fixed 256-by-256 output to the current card size. Pending
states retain the asset icon with nonblocking progress. Invalid material data,
missing dependencies, failed texture builds, render failures, and readback
failures retain the icon and expose one stable asset-qualified diagnostic.
Failures are not persisted as successful output and do not retry every frame;
retry requires a changed key, refresh, or explicit retry.

Material and MaterialInstance previews use the retained shared
`/Engine/Editor/MaterialPreview/Sphere` mesh and the resolved runtime material
values. TextureCube previews use the same sphere with the editor-only
world-reflection shader and the orientation contract in
[Cube Textures](../../Runtime/Rendering/CubeTextures.md). Content Browser cards
never own live viewports, worlds, or per-card preview meshes.
