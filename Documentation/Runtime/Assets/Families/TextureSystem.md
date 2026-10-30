# Texture System

Summary: Define texture assets, derived platform data, cooking, GPU upload, materials, and editor integration.

Modules: Engine, TextureEditor, RenderCore, RHI

Last reviewed: 2026-10-29

Durin's Texture2D pipeline has explicit authored-source, derived platform,
cooked-runtime, render-resource, editor, and material boundaries.

## Asset and Build Ownership

Texture recipe inputs, settings, errors, compilation diagnostics, and the build
module interface require `DURIN_WITH_EDITORONLY_DATA`. Runtime retains platform
data, payload validation, resource publication, and compilation readiness queries.

- `DTexture` stores the one editor-only reflected `FTextureSource` and the one
  editor-only reflected `DAssetImportData` pointer shared by Texture2D,
  TextureCube, and VolumeTexture. The source contains canonical bulk texels plus
  dimensions, topology, format, channel/transparency metadata, schema, and
  content identity; import data records optional physical-source provenance for
  explicit reimport. Checked-in authored packages store both fields with the
  canonical `DTexture` declaring identity.
- `Usage` and `bSRGB` remain runtime-authored metadata. `MaxResolution`,
  `CompressionQuality`, `AlphaMipMode`, and `AlphaCoverageThreshold` are
  editor-only build settings.
- `FTextureSource` retains a non-persistent, locally locked `LockedMipData`
  cache after the first payload read/decompression. Its `FMipData` handle
  exposes the complete decoded chain and checked shared-buffer views for
  individual mips. `ReleaseSourceMemory` explicitly evicts the cache; existing
  handles remain valid through immutable shared ownership. Source replacement
  installs fresh cache state rather than comparing a separate cache identity.
- Texture2D request assembly captures a torn-off `FTextureSource` and
  `FTexture2DBuildSettings` without reading source bytes. Cache-miss execution
  acquires `FTextureSource::FMipData` and supplies shared `Image::FImage` values
  to the recipe. These images retain immutable decoded allocations without
  retaining a texture or package-read handle. Cube recipes likewise consume
  shared decoded images through `FTextureCubeFaceImages`; their storage and
  identity boundary is described in [Cube Textures](CubeTextures.md). Volume
  adapters retain their family-specific recipe inputs.
- A Texture2D build request carries source identity in its retained source
  snapshot alongside settings. Workers return platform data and diagnostics, never source
  pixels for installation. Optional source replacements belong to the
  GameThread result-application context. Rebuilds preserve the existing source;
  Texture2D import/reimport commits validated Source, settings and metadata before
  starting independent DDC work. Build failure or cancellation retains the new
  authored data; `PostEditChange` triggers independent compilation and saving
  remains a separate upper-layer operation.
- The Texture2D builder analyzes alpha from the input image when selecting
  its platform format. Cube builds retain their whole-cube transparency override
  so all six faces use one format.
- `FTexturePlatformData` is rebuilt from source data. It contains a complete,
  tightly packed desktop BC mip chain selected from usage, transparency, and
  color space.
- Source and platform data are intentionally separate. Platform cache hits and
  rebuilds install a complete platform-data value without mutating source.
- Normal usage generates linear-space mips by averaging and renormalizing the
  encoded normal vector. Color usage filters RGB in linear space when sRGB is
  enabled. Data/Mask usage averages channels independently.
- Opaque Color uses BC1, transparent Color uses BC3, Normal uses BC5, and
  Data/Mask uses BC7. Color and Data/Mask select the matching sRGB variant when
  the explicit color-space setting is enabled.
- NPOT edges are extended by clamping to the last source texel before each 4x4
  block is encoded. This keeps every mip valid without changing its logical
  dimensions.
- `MaxResolution` is zero when the source-sized base mip should be retained.
  Otherwise the builder selects the first generated mip whose width and height
  both fit the limit. The cap is mip-aligned and preserves the usage-aware
  filter path and source aspect ratio.
- A supplied canonical source mip chain is preserved verbatim through the
  recipe input instead of being regenerated. A source with only its base mip
  selects deterministic mip generation.
- Compression quality is an offline search-effort choice. Low, Normal, and High
  map to progressively stronger endpoint, channel, and BC7 partition searches.
  It changes build time and encoded quality, not the selected pixel format or
  runtime memory layout.
- Alpha coverage preservation is an explicit opt-in for alpha-tested Color
  textures. `Average` retains ordinary alpha filtering for translucent content.
  `PreserveCoverage` measures the source fraction whose alpha meets
  `AlphaCoverageThreshold`, then scales only the alpha channel of each generated
  mip before compression so its thresholded coverage is as close as the mip's
  discrete texel count permits. The threshold must be strictly between zero and
  one and defaults to `0.5`. RGB filtering is unchanged. The setting remains
  serialized but inactive for opaque Color, Normal, and Data/Mask textures.

## Derived Platform Data

For Texture2D, TextureCube, and VolumeTexture, `GetPlatformData()` returns only
installed CPU data and returns null until installation. Queries and payload
inspection never read cooked bulk, deserialize data, or update render resources.
Cube metadata uses `GetSizeX()`, `GetNumMips()`, and `GetPixelFormat()`; it does
not expose a separate set of built-resource queries. Full-object edits use the
shared `PostEditChange()` compilation path across all three texture families.
`CachePlatformData(bAsyncCache)` prepares the current texture's CPU platform
data. False selects synchronous preparation; true requests background work.
`BeginCachePlatformData()` is the background convenience entry point.
Current-asset caching preserves authored source storage and package dirty state.
Canonical cube faces and HDR panoramas use the same source identity and DDC path
for synchronous and asynchronous caching, without decoding source bytes on a hit.
`FinishCachePlatformData(TimeoutSeconds)` only completes existing authored
compilation or a cooked read/decode request and installs its result on GameThread;
it never starts a new request. Its result reports installed CPU-data readiness;
request completion separately reports success, failure, or cancellation. A failed
request can retain a usable earlier product. With no installed data it returns false.
Automatic publication and explicit completion share the same decode, validation
and installation logic. Synchronous cooked caching with no request reads,
decodes and installs CPU data without requiring a task scheduler or deferred
executor. An existing request is reused rather than decoded twice, and can
finish without a deferred executor.
Installed data succeeds without another resource update. Zero only queries
installed data; negative or non-finite timeouts are rejected. A positive timeout
bounds waiting on an existing request, without canceling it. Synchronous I/O,
decode and installation cannot be preempted and are not covered by that wait
timeout. Explicit completion never pumps unrelated GameThread callbacks.
GPU upload remains asynchronous and is skipped when no RHI exists.
These guarantees concern texture CPU-product completion after the object exists;
package-object loading retains its own asset-I/O runtime requirements.
Read/decode failures write the existing failure state and diagnostic directly,
without invalidating a previously published GPU resource or calling `UpdateResource`.

### Payload architecture qualification

Texture2D stores large canonical texels in authored `FEditorBulkData`.
Request-local decoded pixels and platform mip vectors are not persisted in the
authored object field tree.

Source image encoding belongs to the ordinary source file and decoder. Texture
payload schema 3 belongs to the owning asset, DDC values are rebuildable
canonical platform data, and cooked `PlatformData` fields are immutable
deployment data loaded through package resources. Request input, decoded recipe
data, platform mips, and RHI resources have independent downstream lifetimes.
The authored bulk source is the sole rebuild authority; request-local family
values are views or snapshots, not a second persistent source container.

Texture2D platform mip chains are content-addressed beneath
`DerivedDataCache/Texture2D/` as `.bin` objects. A canonical 128-bit key
includes the imported source-content hash, usage, explicit color-space choice,
maximum resolution, compression quality, alpha-mip policy and threshold, target
platform, and texture-builder version.

`PostLoad` validates persisted source metadata and submits a detached immutable
source snapshot. `CopyTornOff()` removes the live owner and isolates the decoded
cache while retaining immutable bulk storage. Worker DDC lookup uses the saved
content identity; a hit avoids source payload reads, decompression and hashing.
A miss recovers source pixels and builds platform mips on the worker. Missing,
incompatible, corrupt or invalid cache data is a non-fatal miss. Atomic cache
persistence failure does not discard valid in-memory platform data.

Both warm DDC reads and cold builds return from authored Texture2D/TextureCube
`PostLoad` in a pending CPU readiness phase. Thumbnails poll
`IsAsyncCacheComplete()` before inspecting platform data. Cook and reload use
`FinishCachePlatformData()` explicitly. Cooked PostLoad starts asynchronous
range reads and decode when the owner executor is available, then applies and
uploads through the resource owner. Getters remain nonblocking; see
[load preparation](../../Rendering/Pipeline/LoadPreparation.md#object-and-program-readiness).
Save and cook can only observe committed asset
state; the Texture Editor requires an explicit Wait for Build or Cancel Build
decision instead of serializing a pending candidate.

The DDC path is derived entirely from the key; `.dasset` never stores a cache
file path or byte offset. Owner-selected texture payload schema 3 uses an
76-byte header, 40-byte records, 16-byte aligned non-overlapping ranges, explicit BC format,
dimension, mip and slice counts, target platform/profile, and XXH3-128
checksums. Texture2D has exactly one slice and TextureCube has six ordered
slices. The selected cross-asset storage and cooked companion contract is documented in
[Asset Data Lifecycle and Storage](../Packages/AssetDataLifecycle.md).

## Cooking and Runtime Loading

Texture2D contributes its validated TXPL schema-3 value as
the cooked `PlatformData` BulkData field. Cook serializes runtime settings and
the detached field, strips source provenance and editor fingerprints, and
publishes large bytes in the package's headerless raw segment.

`DTexture` owns the common cooked `FBulkData` slot and its side-effect-free const
getter. Each leaf retains its typed installed platform data, codec, validation,
and explicit wire identity: `DTexture2D::PlatformData`,
`DTextureCube::PlatformData`, or `DVolumeTexture::PlatformData`. Storage
consolidation therefore does not change TXPL bytes, DAST version, DDC keys, or
the family dispatch boundary. Cube-specific and volume-specific payload rules
remain in [Cube Textures](CubeTextures.md) and
[Volume Textures](VolumeTextures.md).

Cooked-runtime package mode accepts only Win64/Game and schema 3. Metadata load
attaches the field without reading its range; first platform-data/resource
access locks and decodes it. Decode validates every mip
dimension, block row pitch, byte range, padding, format, checksum, and allocation
limit before replacing live platform data. Missing or malformed bulk is a hard,
asset-qualified load failure with no source decoder, DDC, or offline compressor
fallback.

## Asynchronous Editor Build Coordination

`ValidateTexture2DBuildSettings` and `ValidateTexture2DSourceMips` return
`std::expected<void, FTexture2DInputError>` without diagnostic outputs. Errors own
rejected settings, mip index, image descriptors and accumulated bytes with distinct
failure reasons.
`ITextureCompressorModule::BuildTexture2D` returns
`std::optional<FTexturePlatformData>` by value. It logs a failure once inside
TextureCompressor and returns no partial product. The synchronous recipe accepts no
cancellation control; Engine adapters own derived-data policy and cancellation.
`BuildTextureCube` and `BuildVolumeTexture` follow the same contract, returning
`std::optional<FTextureCubePlatformData>` and
`std::optional<FVolumeTexturePlatformData>` without pointer wrappers. Detailed
recipe failure reasons remain in module logs.
Object-level platform-data work follows the UE cache lifecycle names:
`DTexture2D::BeginCachePlatformData(Request, Completion)` admits explicit asynchronous
work; `DTexture2D::CachePlatformData`, `DTextureCube::CachePlatformData` and
`DVolumeTexture::CachePlatformData` synchronously construct and apply platform data.
All explicit synchronous build overloads borrow their request and default their
application options. Cube and volume builds share `FTextureBuildApplicationContext`
for dirty marking; source preservation is internal to current-asset caching,
not a caller policy. Texture2D retains its optional `SourceReplacement` for
transactional edits that validate and commit a different source on GameThread.
Cube import reuses its normalized source for both building and installation.
The base texture's no-argument `BeginCachePlatformData` and `FinishCachePlatformData`
remain the ordinary metadata-driven lifecycle operations.
Admission, synchronous application and editor rebuild helpers return
`std::expected<void, FTextureBuildOperationError>`. The public error contains only
Failed, InvalidInput, Unavailable, Busy or Canceled disposition and actionable input
text. It has no success state, provider stage, nested compilation/input cause,
object identity or expected/actual build hashes. Callers format it through
`FormatTextureBuildOperationError`. Rejected asynchronous admission has no callback;
accepted work completes exactly once on GameThread. Application validates candidates
before publication. Invalid settings, source changes and failed builds preserve
existing installed data until a complete candidate is accepted.
The complete internal 2D build entry, execution controls, build/compiler diagnostics,
cache-only mip lookup and test metrics live in Engine private headers. Cube normalization
keeps its detailed validation codes inside TextureCompressor and exposes only actionable
input text through the module contract. No provider diagnostic type crosses that boundary.
Detached public construction returns a complete CPU product by value or the same narrow
operation error; success has no nullable product alternative. Its synchronous 2D entry has
no cancellation predicate; import orchestration checks cancellation before construction
and before accepting the completed result.

Engine registers `DTexture` to the `Durin.Texture` manager in its
[asset-compilation aggregate](../Build/AssetCompilation.md). Editor-enabled
Engine captures definitions and immutable inputs, then executes the texture-family
shared-output session. DDC owns record lookup, integrity validation, fallback,
and best-effort persistence; Engine owns typed output assembly and application.
Authored Texture2D, TextureCube and VolumeTexture caching use this same manager,
request-owner cancellation, completion mailbox and GameThread application path.
Volume inputs capture source metadata and build settings without acquiring voxels;
its synchronous and asynchronous paths share definition validation, cache policy
and output assembly. No volume-specific queue or persistent request state exists.
Registered Engine functions invoke TextureCompressor's pure synchronous module on a
cache miss. TextureCompressor stays resident until editor shutdown; consumers drain
work before module unload. See the
[shared build protocol](../Build/DerivedDataBuild.md#family-boundaries).
`FTextureCompilingManager` directly owns two
worker admissions and a conservative 1 GiB estimated in-flight byte
budget. Requests are FIFO within background and interactive classes. At most
four interactive requests are admitted consecutively while background work is
waiting; an already admitted job is never preempted. A single valid request
larger than the budget runs alone so maximum-dimension textures cannot deadlock
the queue.

Each request carries owned source mip images or a detached deferred source, content identity, all build
settings, Win64/Game target identity, and scheduling identity. Key computation and a warm DDC
lookup use source metadata and content identity only. Explicit edit requests can
carry decoded images; authored PostLoad defers payload recovery until a worker
cache miss. Workers consume those images to generate mips, compress,
validate, and request best-effort DDC persistence before placing a move-only result
in the manager mailbox. Worker status and detailed diagnostics remain private.
Public `FTextureOperationResult` uses `ETextureCompilationStatus` (Succeeded, Failed,
Canceled); it carries a generic failure message or no message on success/cancellation.
Private bounded snapshots and module logs retain detailed diagnostics. Asset
callers query `IsAsyncCacheComplete()` and installed platform data; compiler phases
remain private rather than forming a second Texture2D-only lifecycle interface.
The request and texture share one progress snapshot. Retired work records are
erased; only bounded object diagnostics remain.
The Texture compiling manager commits on the GameThread
after validating request id, weak object identity, and the selected source.
Texture2D result application uses one validated private commit; public setters
retain the selected mutation barrier. Compiler completion uses the plain
`FTextureOperationResult`; Texture2D import returns its authored commit result
without waiting for platform-data readiness.
`BlockOnAnyAsyncBuild()` waits for the selected compilation and GameThread result
application through the asset compiling manager, preserving its terminal outcome
and diagnostics. It neither starts nor cancels work, and GPU initialization remains
asynchronous. Source/settings/platform-data replacement, new submissions,
`Modify`, `PreEditChangeProperty`, and `PreEditUndo` reuse this wait before changing or capturing
live data. Same-owner work submitted by completion observers is drained before
mutation proceeds. There is no separate mutation barrier or mutation-state set.

Runtime Engine and Launch have no Texture2D worker-queue dependency. Launch pumps
the aggregate with a 64-item normal-frame limit and a shared 2 ms cooperative
deadline checked between completion applications. The manager mailbox
remains the durable owner of large move-only results and does not depend on
deferred-executor admission for wakeup. For authored compilation,
`DTexture::FinishCachePlatformData(TimeoutSeconds)` waits until its
request reaches the mailbox and then applies only that request without the
normal-frame budget.
Shutdown likewise drains all callbacks before the process task scheduler
closes. Every callback is GameThread-only and the compiling manager's request
serial and input-identity comparison prevent stale publication.

Cancellation is checked by the external build system before and after recipe
execution. An admitted texture recipe finishes processing;
a canceled request discards its result before output publication or persistence.
New requests and accepted authored edits cancel older work. Unload, destruction,
document close, failed startup unwind, and normal shutdown cancel outstanding work. Shutdown stops admission, cancels the
queued and running set, waits for worker quiescence, drains GameThread
completions, and then destroys the manager-owned queue. Request state and
completion history are manager-owned and bounded to 256 records; source image
buffers are released as soon as worker use ends.

CPU readiness is the presence of valid installed platform data. Compilation
phase and terminal build diagnostics belong to the manager's active or
bounded recent operation record; GPU readiness and failure belong to
`DTexture` resource availability, pending operation, and latest consumed result. These owners are
queried separately. Operation diagnostics retain request identity, timings,
byte metrics, source-decoder invocation, internal build cause, and the matching
failure phase. They contain no DDC key or cache-hit/rebuild origin. Cache
infrastructure failures are logged by DDC rather than retained in manager
snapshots; idle textures do not persist operation diagnostics.

Normal-frame completion drains retain the 64-item cap; callback duration is
diagnostic rather than a separate time limit. A 16K source has a 1 GiB decoded
allocation, about 1.33 GiB uncompressed mip chain, and 170.67-341.33 MiB BC
result. Its 2.50-2.67 GiB source/intermediate/result working set is admitted
alone. Two typical 4K builds remain below the 1 GiB admission budget, while
larger requests serialize. These are allocation bounds, not wall-clock promises;
the full 16K high-quality matrix is not a routine gate.

Historical synchronous stalls and scheduling calibration remain in
[Asynchronous Texture2D Build and Readiness](../../../Plans/Archive/2026-08/AsynchronousTexture2DBuildAndReadiness.md).

## Transactional Build-Setting Edits

The Texture Editor changes `Usage`, `bSRGB`, `MaxResolution`,
`CompressionQuality`, `AlphaMipMode`, and `AlphaCoverageThreshold` through
reflected-property transactions.
`DTexture2D::PreEditChangeProperty` captures complete candidate settings from
detached proposal storage and defers application through the reflected-edit
protocol. The worker result remains private until it succeeds. Invalid values,
decode/build/DDC failures, cancellation, supersession, and document close leave
the reflected values, package Dirty state, undo history, platform data, and
stable texture target unchanged.

After a successful worker result, the GameThread applies the reflected value
once, `PostEditChangeProperty` installs that exact persisted candidate without
rebuilding it, and the edit session registers one transaction and one Dirty
transition. Cancel, Undo, and Redo use the same asynchronous proposal path.
Changing usage resets sRGB to that preset's default; editing sRGB afterward is
an explicit override. Committed edits dirty the package through the shared
reflected transaction path. Direct source and build-setting setters complete authored work and invalidate
installed platform data, obsolete cooked bytes, and GPU publication. They do not
build or mark the package dirty themselves; the operation applying those values
owns rebuilding and dirty marking. `SetPlatformData()` completes authored
compilation and invalidates GPU publication before adopting its CPU input.
Compiler application detaches the active request before installation, so it never
joins itself. Retained request diagnostics do not override installed CPU-data
readiness after a synchronous retry or external product replacement.
Call `UpdateResource()` to upload installed data.

## Render-Thread Boundary

`DTexture` is the reflected abstract boundary shared by `DTexture2D`,
`DTextureCube`, and `DVolumeTexture`. It cannot be instantiated as a concrete
asset type. It provides the common source and render-resource contract. Each
leaf retains its typed build settings, installed platform data, family codec,
and `CreateResource` construction hook. The base retains the sole reflected source
and import pointer plus the sole cooked bulk slot; there are no forwarding
accessors or duplicate family storage fields. Authored saves emit only the
canonical base identities.

`DTexture` directly owns the current `FTextureResource`, a stable
`FTextureReference`, and one `FRenderCommandFence` for resource readiness.
Concrete `FTexture2DResource`, `FTextureCubeResource`, and
`FVolumeTextureResource` inherit `FTextureResource` directly. Initialization
consumes immutable CPU upload input and releases its shared ownership; the
persistent resource retains its RHI allocation.

`UpdateResource()` releases the prior resource, constructs its replacement from
installed platform data on GameThread, and queues reference initialization,
resource initialization and publication on RenderThread. Each admitted update
executes in command order. There is no candidate/successor operation, cancellation
state machine, GPU completion task or GameThread ownership handoff. GPU updates
need an active RHI and open render-command admission, independently of the task
scheduler and GameThread deferred executor. CPU asynchronous preparation retains
its existing worker scheduling, invalidation and publication rules.

`ReleaseResource()` queues fallback, release and deferred deletion, retaining
installed CPU data, pending CPU preparation and the reusable texture reference.
Source/CPU-product mutation separately retires stale preparation and invalidates
rendering. `UpdateResource()` never loads or builds CPU data. Preparation failures
leave untouched GPU resources alone; explicit updates and invalidation have no
old-allocation rollback guarantee.

RenderThread records every mip upload before publishing the stable reference
through `FDynamicRHI::RHIUpdateTextureReference()`. `FUpdateTexture2DCommand` and
`FUpdateTexture3DCommand` copy upload bytes and retain the target allocation;
Vulkan replay copies into transfer storage retired by GPU completion tokens.
The resource fence confirms that initialization and publication have been ordered,
not that GPU execution has finished. Later device/replay failures are not
recoverable per-texture update results.

`GetResource()` borrows the current render representation on GameThread;
it returns null after release or when no resource was created.
`HasPendingRenderResourceInitialization()` checks only that resource's initialization
fence, returning false for a nonexistent resource. CPU caching is queried separately
through `IsAsyncCacheComplete()`. After initialization completes, callers can read
the allocation through `FTextureResource::GetTextureRHI_GameThread()`. None of
these queries advances work.
There is no combined update-state enum, retained update-error string or independent
texture teardown flag. Object `BeginDestroyed`/garbage flags own admission and
destruction checks; editor diagnostics combine resource and cache facts locally.
Missing platform data and closed render admission reject before initialization.
Preparation, invalid descriptions, allocation failures and exceptions are logged.
CPU data remains available for an explicit retry. Descriptor support
is checked before allocation; see
[RHI Capabilities and Vulkan Startup](../../Rendering/RHI/RHICapabilitiesAndVulkanStartup.md).

Package reload completes selected compilation through the asset compiling manager
and validates installed CPU data, then waits for admitted uploads with a generic
render-command fence before validating resource allocations. Textures expose no
reload-specific completion API. Preparation failures reject replacement without
implicitly retrying compilation. Headless reload accepts valid CPU data without
requiring a GPU resource; no wait pumps unrelated GameThread callbacks.

`BeginDestroy()` closes admission and cancels CPU preparation. All build/cache
entry points reject closed textures, including teardown
begun by a completion callback during request retirement. Completion queries do
not report success from retained CPU data after teardown. Existing generic
deferred cleanup owns each resource through its queued initialization, release
and deletion, followed by retirement of the stable reference. No render command
captures the texture UObject, and destruction never joins GPU initialization.
Initialized owners must retire before RenderCore closes cleanup admission.
Cancellation routing accepts objects already marked for destruction, since the
object lifecycle sets `BeginDestroyed` before invoking the override. The compiler
uses the object's serial key to retire work even after its weak reference expires.

Material and scene bindings retain counted copies of the stable reference and
observe replacements without reacquiring the asset. Delayed release resets that
target only when it still matches the released allocation, so retiring an old
resource cannot unbind its replacement. Failed direct updates leave fallback,
which may be null under the existing family binding policy.

Fixed-input work captures `FTextureResource::GetTextureRHI_GameThread()` after
checking resource presence and completed initialization. The counted
`FTextureRHIRef` retains neither a UObject nor a C++ resource and does not follow
replacements. GameThread never reads the
stable reference's mutable RenderThread target. Ordinary long-lived bindings use
`GetTextureReferenceRHI()` instead.

Cube thumbnail rendering creates a fixed reference on demand from its captured
allocation. Material thumbnail sessions retain allocation refs for all eight
built-in texture roles and reject delayed output if an observed dependency
changes. No asset-owned snapshot wrapper or eagerly allocated fixed reference
exists. Editor CPU previews compare source identity and weak ownership of
immutable platform data. Upload resources share that installed data until
initialization completes, avoiding a second pixel-buffer copy. Thumbnail
sessions validate weak asset identity, asset revisions and the actual captured
GPU allocation. Direct invalidation clears the captured allocation, invalidating dependent
thumbnail sessions; no resource-update generation is stored.

RHI pixel-format metadata also owns the tightly packed block layout calculation.
Platform-data validation and Vulkan uploads use the same block count, row pitch,
and payload size for both uncompressed and BC formats. Vulkan repacks update
regions by block row and permits a non-block-aligned extent only when it reaches
the mip edge, so NPOT base levels and sub-4x4 tail mips remain valid.

`VulkanRHITests` is the hardware-backed acceptance boundary for this path. It
starts the runtime Vulkan module without creating a window, uploads three
distinct mip levels, samples each with explicit LOD in a compute shader, and
reads results from host-visible memory. The same dispatch covers linear and sRGB
RGBA8 plus builder-produced BC1, BC3, BC5, and BC7 textures, so format upload,
mip addressing, hardware color-space conversion, and compressed sampling are
checked against known values rather than inferred from editor startup.

## Editor Contract

TextureEditor owns `Diagnostics/TexturePayloadInspection.h`, the editor-only
texture-domain lifecycle summary used by Texture2D, TextureCube, and
VolumeTexture. Live summaries cover source, derived, cooked,
decoded CPU, and GPU stages with schema version, texel count, logical/stored
bytes, placement capability, provenance, state, diagnostic, and an explicit
repair classification. They derive those stages from common source metadata,
installed platform data, any available manager operation diagnostic, cooked
bulk, and current resource availability and update result. Package summaries are construct-free and
join reflected domain fields with Engine storage inspection; neither form opens
DDC, rebuilds, or creates runtime resources merely to inspect state.
While CPU caching is pending, live summaries suppress derived/decoded rebuild
actions and show preparation progress instead of suggesting another rebuild.

Engine exposes the underlying texture, storage, compilation, and render-resource
facts but does not own the combined editor diagnostic or its repair guidance.

Texture editors render this summary as a read-only Payload Lifecycle section.
Buttons remain attached to explicit Reimport, Reimport From File, build, and
save workflows; the summary itself performs no mutation or source probing.

## Source capability matrix

| Family/source | Accepted source formats | Mip policy | Recipe result |
| --- | --- | --- | --- |
| Texture2D | RGBA8 | One mip generates; a supplied chain is preserved | BC1/BC3/BC5/BC7 according to settings |
| TextureCube six-face | RGBA8, six equal square slices | Generates from faces | Six matching BC face chains |
| TextureCube long/lat | RGBA8 sRGB or finite RGBA32F linear | Source panorama is retained; projection regenerates faces | Six matching BC face chains |
| VolumeTexture | R8, RG8, RGBA8, R16F, RGBA16F | Generates a full 3D box-filtered chain | Matching uncompressed portable format |

Generic Source can represent additional blocks, layers, arrays, G16, RGBA16,
and R32F. Current family validators reject combinations absent from this table
with a family-specific import/build diagnostic; representation support does not
imply a provider exists.

## Source storage and budgets

Texture source schema 3 separates raw texel format from storage compression.
Raw is the package default and therefore retains existing inline/external Bulk
Directory placement. Explicit initialization may select bounded byte-run
lossless compression when it produces a smaller value. Metadata records decoded
size and the canonical decoded XXH3-128 hash, so raw and compressed storage have
the same content identity. Snapshot creation verifies decompression and the
canonical hash before publishing an immutable decoded buffer. Successful reads
retain that buffer as non-persistent `LockedMipData`; subsequent `FMipData`
handles and snapshots share it without another package read or decompression.

The qualification fixtures freeze behavioral rather than machine-time budgets:
metadata access performs zero payload reads; the first mip-data request performs
at most one package read; later handles and snapshot copies share their backing
allocation and add zero payload bytes. Explicit source-memory release evicts the
cache without invalidating existing handles, and the next request performs one
reload. Stored plus decoded source bytes are each capped at 512 MiB. Texture2D
recipe peak intermediates remain reported by the compiling manager and bounded
by its 1 GiB admission estimate. These invariants remain portable across
developer machines where a millisecond threshold would not.

`TextureEditor` registers a per-resource workspace for `DTexture2D`. It exposes:

- optional reimport hint, canonical imported dimensions/channel semantics,
  transparency, and decoded format, without filesystem availability probing;
- transactional Usage, sRGB, maximum-resolution, and compression-quality
  controls, plus alpha mip mode and coverage threshold;
- platform format, mip count and range, byte size, residency policy, update progress,
  and current platform-data status;
- normal workspace save, Dirty, close protection, Undo, and Redo behavior.

The editor previews the built platform representation and allows each mip level
to be selected. The preview can show the original RGBA result or visualize the
R, G, B, or A channel as opaque grayscale. Channel filtering renders into an
offscreen RGBA8 texture and does not alter the shared ImGui shader. Every open
texture document owns independent preview state and registered preview textures,
so simultaneously visible documents cannot reuse or overwrite one another's
image. Missing or invalid platform data falls back to canonical imported pixels
when available; otherwise the preview is released.
Volume source previews consume canonical `FTextureSource` mip bytes only when
their input or slice selection changes, without rematerializing import BulkData.
Canonical-data availability, cache activity, upload, and format information is
shown with retry and Cancel Build controls. Detailed compiler failures remain in
module logs. Reimport resolves the persisted explicit hint only when invoked;
Reimport From File adopts a newly selected hint after a successful candidate
commit. Content Browser thumbnail keys use authored package and canonical
content identity and never inspect a physical source.

## Current Limitations

- Every mip remains fully resident and there is no texture streaming, sparse
  residency, partial upload, or eviction policy. Initial asset creation and
  scene-import candidate construction remain synchronous; the asynchronous
  contract owns ordinary editor DDC misses, retries, direct reimports, and
  build-setting changes.
- Material role validation, UV transforms, opacity, and mask coverage follow
  [Material System](../../Rendering/Materials/MaterialSystem.md); Texture assets do not define pass policy.

## Related Documentation

- [Render Resource Lifecycle](../../Rendering/Pipeline/RenderResourceLifecycle.md)
- [Asset Data Lifecycle And Storage](../Packages/AssetDataLifecycle.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Editor/TextureEditor/`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialTypes.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
