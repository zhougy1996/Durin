# Texture System

Summary: Define texture assets, derived platform data, cooking, GPU upload, materials, and editor integration.

Modules: Engine, TextureEditor, RenderCore, RHI

Last reviewed: 2026-09-09

Durin's Texture2D pipeline has explicit authored-source, derived platform,
cooked-runtime, render-resource, editor, and material boundaries.

## Asset and Build Ownership

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
- Texture2D request assembly synchronously reads `FTextureSource::FMipData`
  and captures `Image::FImage` values for the source mip chain together with
  `FTexture2DBuildSettings`. Images share immutable decoded allocations, but
  retain no texture, source object, mip handle, or package-read handle. There is
  no intermediate imported-data or snapshot type. Cube and volume adapters
  retain their family-specific recipe inputs.
- A Texture2D build request carries source identity separately from its images
  and settings. Workers return platform data and diagnostics, never source
  pixels for installation. Optional source replacements belong to the
  GameThread result-application context. Rebuilds preserve the existing source;
  import/reimport commits the retained candidate only after a successful build
  whose identity matches that candidate.
- Texture2D builder version 4 analyzes alpha from the input image when selecting
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
`EnsurePlatformDataLoadedBlocking()` is the explicit GameThread boundary:
it synchronously reads and validates cooked data, installs it, and calls
`UpdateResource()`; GPU completion remains asynchronous. An already-installed
value succeeds without repeating the resource update. Missing authored builds,
missing cooked fields, and bulk/decode failures log the texture path and reason
inside this boundary and return false. Callers use the result for fallback
without repeating the log. This API does not compile authored source.

### Payload architecture qualification

Texture2D is a qualified production consumer of domain-owned payload schemas.
The tracked VintageLighter derived sources include three 1024 x 1024 x 32-bit
TGA files of 4,194,322 bytes each; each decodes to exactly 4 MiB RGBA8, while
the corresponding `.dasset` packages are only 1,300-1,387 bytes. Large
canonical texels use the authored package's `FEditorBulkData` placement rather
than a reflected byte vector. Neither request-local decoded pixels nor platform
mip vectors are stored in the authored object field tree.

Source image encoding belongs to the ordinary source file and decoder. Texture
payload schema 2 belongs to the owning asset, DDC values are rebuildable
canonical platform data, and cooked `PlatformData` fields are immutable
deployment data loaded through package resources. Request input, decoded recipe
data, platform mips, and RHI resources have independent downstream lifetimes.
The authored bulk source is the sole rebuild authority; request-local family
values are views or snapshots, not a second persistent source container.

Texture2D platform mip chains are content-addressed beneath
`DerivedDataCache/Textures/Objects/` as `.bin` objects. A canonical 128-bit key
includes the imported source-content hash, usage, explicit color-space choice,
maximum resolution, compression quality, alpha-mip policy and threshold, target
platform, and texture-builder version.

`PostLoad` first validates persisted source provenance and compares an available
source's size and stable last-write time with the package fingerprint. An
unchanged source can restore the checksummed, versioned platform payload without
reopening or decoding the image. When the cheap fingerprint changes, the
project-local `DerivedDataCache/SourceFingerprints/Index.bin` maps the current
source path, size, and timestamp to a previously verified content hash. A cold
entry hashes the source once and persists that observation. If the verified
hash still matches the package, loading reuses platform data without dirtying
the asset; subsequent launches reuse the fingerprint index. Only a real content
hash change rebuilds the source and dirties the package. If source is
unavailable, the persisted exact content hash can still restore a matching warm
object without invoking the decoder. Missing, incompatible, corrupt, truncated,
oversized, or invalid cache data is a non-fatal miss and rebuilds from source.
Atomic cache persistence failure does not discard valid in-memory platform data.

An editor DDC miss no longer decodes or compresses in `PostLoad`. It submits an
immutable request and returns with the asset in a queued or running readiness
phase. A warm validated DDC hit and cooked-runtime loading remain synchronous
and behaviorally unchanged. Save and cook can only observe committed asset
state; the Texture Editor requires an explicit Wait for Build or Cancel Build
decision instead of serializing a pending candidate.

The DDC path is derived entirely from the key; `.dasset` never stores a cache
file path or byte offset. Owner-selected texture payload schema 2 uses an
80-byte header, 40-byte records, 16-byte aligned non-overlapping ranges, explicit BC format,
dimension, mip and slice counts, target platform/profile, and XXH3-128
checksums. Texture2D has exactly one slice and TextureCube has six ordered
slices. The selected cross-asset storage and cooked companion contract is documented in
[Asset Data Lifecycle and Storage](../Assets/AssetDataLifecycle.md).

## Cooking and Runtime Loading

Texture2D producer version 4 contributes its validated TXPL schema-2 value as
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
[Volume Textures](../Assets/VolumeTextures.md).

Cooked-runtime package mode accepts only Win64/Game and schema 2. Metadata load
attaches the field without reading its range; first platform-data/resource
access locks and decodes it. Decode validates every mip
dimension, block row pitch, byte range, padding, format, checksum, and allocation
limit before replacing live platform data. Missing or malformed bulk is a hard,
asset-qualified load failure with no source decoder, DDC, or offline compressor
fallback.

## Asynchronous Editor Build Coordination

Engine registers `DTexture2D` to the `Durin.Texture` typed manager in its
[asset-compilation aggregate](../Assets/AssetCompilation.md). Editor-enabled
Engine computes Texture keys, validates DDC Get results, invokes TextureBuild's
pure synchronous provider only on a miss, and performs best-effort Put. The
typed modular-feature registry retires admitted provider calls before provider code unloads.
`FTextureCompilingManager` directly owns two
worker admissions and a conservative 1 GiB estimated in-flight byte
budget. Requests are FIFO within background and interactive classes. At most
four interactive requests are admitted consecutively while background work is
waiting; an already admitted job is never preempted. A single valid request
larger than the budget runs alone so maximum-dimension textures cannot deadlock
the queue.

Each request carries owned source mip images, content identity, all build
settings, Win64/Game target identity, scheduling identity,
and a manager-owned monotonic request serial. Key computation and a warm DDC
lookup use source metadata and content identity only. Input assembly captures the decoded images before queue admission. On a miss,
workers consume those images to generate mips, compress,
validate, and atomically persist DDC data before placing a move-only result in
the manager mailbox. The Texture compiling manager commits on the GameThread
only when request id, serial, weak object identity, and complete captured input
identity match the manager-owned candidate. Accepted external source or settings
edits explicitly cancel outstanding work; cancellation plus serial validation is
the live-object mutation boundary.

Runtime Engine and Launch have no Texture2D worker-queue dependency. Launch pumps
the aggregate with a 64-item normal-frame budget. The manager mailbox
remains the durable owner of large move-only results and does not depend on
deferred-executor admission for wakeup. `WaitForTexture2DCompilation` waits until its
request reaches the mailbox and then pumps without the normal-frame item budget.
Shutdown likewise drains all callbacks before the process task scheduler
closes. Every callback is GameThread-only and the compiling manager's request
serial and input-identity comparison prevent stale publication.

Cancellation is checked every eight generated or alpha-processing scanlines,
between mips, and every 64 compression blocks. New requests and accepted
authored edits cancel older work. Unload, destruction, document close, failed startup unwind, and
normal shutdown cancel outstanding work. Shutdown stops admission, cancels the
queued and running set, waits for worker quiescence, drains GameThread
completions, and then destroys the manager-owned queue. Request state and
completion history are manager-owned and bounded to 256 records; source image
buffers are released as soon as worker use ends.

CPU readiness is the presence of valid installed platform data. Compilation
phase and terminal build/DDC diagnostics belong to the manager's active or
bounded recent operation record; GPU readiness and failure belong to
`DTexture` resource availability, pending operation, and latest consumed result. These owners are
queried separately. Operation diagnostics retain request identity, timings,
byte metrics, DDC key, cache-hit/rebuild origin, source-decoder invocation, and
the matching failure phase; idle textures do not persist those facts.

Normal-frame completion drains retain the 64-item cap; callback duration is
diagnostic rather than a separate time limit. A 16K source has a 1 GiB decoded
allocation, about 1.33 GiB uncompressed mip chain, and 170.67¨C341.33 MiB BC
result. Its 2.50¨C2.67 GiB source/intermediate/result working set is admitted
alone. Two typical 4K builds remain below the 1 GiB admission budget, while
larger requests serialize. These are allocation bounds, not wall-clock promises;
the full 16K high-quality matrix is not a routine gate.

Historical synchronous stalls and scheduling calibration remain in
[Asynchronous Texture2D Build and Readiness](../../Plans/Archive/2026-08/AsynchronousTexture2DBuildAndReadiness.md).

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
reflected transaction path. Direct build-setting setters follow the same
rebuild rule and dirty the package after success.

## Render-Thread Boundary

`DTexture` is the reflected abstract boundary shared by `DTexture2D`,
`DTextureCube`, and `DVolumeTexture`. It cannot be instantiated as a concrete
asset type. It provides the common source and render-resource contract. Each
leaf retains its typed build settings, installed platform data, family codec,
and resource-construction hook. The base retains the sole reflected source and
  import pointer plus the sole cooked bulk slot; there are no forwarding accessors
  or duplicate family storage fields. Authored saves emit only the canonical base
  identities.

`DTexture` directly owns the current `FTextureResource`, a stable
`FTextureReference`, and at most one executing `FTextureResourceUpdate`.
Concrete `FTexture2DResource`, `FTextureCubeResource`, and
`FVolumeTextureResource` inherit `FTextureResource` directly and remain the
asset's rendering representation until replacement or teardown. Initialization
consumes their immutable CPU upload input, releasing that copy even when
initialization fails; the persistent resource retains only its RHI allocation.
An explicit retry constructs a new candidate from installed asset platform data.

The update operation owns its candidate and coalesces one uninitialized
successor. During execution, another `UpdateResource()` replaces that successor.
Only the latest retained input starts after the active result is consumed.
An already admitted operation may publish before its successor; no request
revision or token comparison suppresses it. Closing the operation discards its
successor without initializing it.

RenderThread initializes the candidate and records every mip upload before
publication. `FUpdateTexture2DCommand` and `FUpdateTexture3DCommand` copy upload
bytes and retain the target allocation; Vulkan replay copies into transfer
storage retired by GPU completion tokens. CPU initialization completion means
upload recording and publication have been ordered, not that GPU execution is
complete. The existing void upload API does not convert later device or replay
failure into a recoverable per-texture result.

Publication switches the stable reference through
`FDynamicRHI::RHIUpdateTextureReference()`. Material and scene bindings retain
counted copies of this stable identity and observe replacements without
reacquiring the asset. The operation owns the published candidate until
GameThread acquires its terminal handoff. Successful consumption installs the
candidate as the asset's current resource and retires the previous resource;
failure retires only the candidate and preserves the last successful resource.
Delayed release resets the stable target only when it still matches the released
allocation, so retiring an old resource cannot unbind its replacement.

There is no extra GameThread allocation cache. The current resource's allocation
remains immutable from acquired initialization until queued release. Within
that interval the owning GameThread can capture a counted allocation through
`GetTextureRHI_GameThread()` without reading the stable reference's mutable
RenderThread target. New updates always use a separate candidate.

`PumpTextureResourceUpdates()` runs in `FEngineLoop::TickPostEventFrame` before
UI and outside the rendering/minimized branch. It consumes terminal operations
and starts retained successors; getters never advance work. Native hosts use
this same explicit pump. Pending membership is GameThread-owned, and the pump
checks membership again before dereferencing its iteration snapshot because
listeners can destroy other assets. Completion needs neither Task admission nor
editor polling. The engine shuts the Task system down before asset collection,
so texture destruction reconciles pending operations independently.

`HasUsableResource()` reports availability of a consumed successful allocation;
`IsResourceUpdatePending()` includes terminal-but-not-consumed operations.
`GetResourceUpdateState()` reports CPU update progress/result independently.
There is no retained failure-category enum on the asset or resource. Unsupported
descriptions, failed allocation, missing RHI, exceptions, and rejected command
admission are logged; the operation retains only its completion state. Editors
use that state for generic failure and explicit update retry, with details in
the log. Missing platform data rejects before admission. The exact descriptor
support check precedes allocation; see
[RHI Capabilities and Vulkan Startup](RHICapabilitiesAndVulkanStartup.md).

Ordinary replacement is asynchronous. `BeginDestroy` stops admission, removes
pump membership, discards the retained successor, and closes the active operation
under the same mutex used for publication. It joins accepted CPU initialization,
then queues release and deferred cleanup of candidates/current before the stable
reference. No publication can occur after that close boundary. Accepted commands
retain operation storage without capturing a UObject. Reference and candidate
initialization share one checked command admission; a rejected command leaves
both uninitialized. Producers must close initialized owners before RenderCore
stops accepting required cleanup commands.

`GetPublishedTexture()` is a GameThread-only capture of the last consumed
successful `FTextureRHIRef`. It retains a concrete allocation, not a UObject or
C++ resource, and does not follow later replacements. RHI operations still run
on their owning rendering thread. Ordinary long-lived bindings continue to use
`GetTextureReferenceRHI()` instead.

Cube thumbnail rendering creates a fixed reference on demand from its captured
allocation. Material thumbnail sessions retain allocation refs for all eight
built-in texture roles and reject delayed output if an observed dependency
changes. No asset-owned snapshot wrapper or eagerly allocated fixed reference
exists. The GameThread `OnTextureResourceChanged` event distinguishes admitted
input, consumed completion, and close; it carries no generation. Editor preview
caches invalidate through this event and source identity changes. Thumbnail
acceptance checks pending state, captured allocations, and existing
package/material versions; unchanged stable-reference pointers or cache keys
cannot certify it.

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
Persistent canonical-data, build, upload, and format status is shown with retry
controls. Pending diagnostics show phase, request, elapsed
queue/worker time, and memory estimates, with Cancel Build and Wait for Build
controls. Reimport resolves the persisted explicit hint only when invoked;
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
  [Material System](MaterialSystem.md); Texture assets do not define pass policy.

## Related Documentation

- [Render Resource Lifecycle](RenderResourceLifecycle.md)
- [Asset Data Lifecycle And Storage](../Assets/AssetDataLifecycle.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Editor/TextureEditor/`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialTypes.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
