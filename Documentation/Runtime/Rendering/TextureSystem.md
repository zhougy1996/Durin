# Texture System

Summary: Define texture assets, derived platform data, cooking, GPU upload, materials, and editor integration.

Modules: Engine, TextureEditor, RenderCore, RHI

Last reviewed: 2026-09-03

Durin's Texture2D pipeline has explicit authored-source, derived platform,
cooked-runtime, render-resource, editor, and material boundaries.

## Asset and Build Ownership

- Every concrete texture stores one editor-only `FTextureSource`. `DTexture`
  owns the common validation and access contract, while the reflected field
  remains on each concrete class so old family fields can migrate through the
  serializer's exact declaring-type route. The source contains canonical bulk
  texels plus dimensions, topology, format, channel/transparency metadata,
  schema, and content identity. `DAssetImportData` separately records optional
  physical-source provenance for explicit reimport.
- `Usage` and `bSRGB` remain runtime-authored metadata. `MaxResolution`,
  `CompressionQuality`, `AlphaMipMode`, and `AlphaCoverageThreshold` are
  editor-only build settings.
- `FTexture2DImportedData` is a detached request input copied from an immutable
  `FTextureSource` snapshot. It carries a shared `FEditorBulkData` handle and
  metadata, so computing identity or completing a warm DDC lookup does not
  materialize pixels. `FTextureSourceData` is the decoded RGBA8 recipe value and
  is created only after a DDC miss or when an editor preview explicitly needs
  pixels. Neither value is retained on `DTexture2D`.
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

Texture2D producer version 3 contributes its validated TXPL schema-2 value as
the cooked `PlatformData` BulkData field. Cook serializes runtime settings and
the detached field, strips source provenance and editor fingerprints, and
publishes large bytes in the package's headerless raw segment.

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

Each request carries a detached `FTexture2DImportedData` snapshot, content
identity, all build settings, Win64/Game target identity, scheduling identity,
and a manager-owned monotonic request serial. Key computation and a warm DDC
lookup use source metadata and content identity only. A miss materializes the
bulk payload into `FTextureSourceData`, then workers generate mips, compress,
validate, and atomically persist DDC data before placing a move-only result in
the manager mailbox. The Texture compiling manager commits on the GameThread
only when request id, serial, weak object identity, source identity, and
complete settings still match. Cancellation is cooperative; this comparison
is the live-object mutation boundary.

Runtime Engine and Launch have no Texture2D worker-queue dependency. Launch pumps
the aggregate with a 64-item normal-frame budget. The manager mailbox
remains the durable owner of large move-only results and does not depend on
deferred-executor admission for wakeup. `WaitForTexture2DCompilation` waits until its
request reaches the mailbox and then pumps without the normal-frame item budget.
Shutdown likewise drains all callbacks before the process task scheduler
closes. Every callback is GameThread-only and the compiling manager's request/
generation/identity/settings comparison prevents stale publication.

Cancellation is checked every eight generated or alpha-processing scanlines,
between mips, and every 64 compression blocks. New requests cancel the older
generation. Unload, destruction, document close, failed startup unwind, and
normal shutdown cancel outstanding work. Shutdown stops admission, cancels the
queued and running set, waits for worker quiescence, drains GameThread
completions, and then destroys the manager-owned queue. Request state and
completion history are manager-owned and bounded to 256 records; source payload
handles are released as soon as worker use ends.

CPU readiness is the presence of valid installed platform data. Compilation
phase and terminal build/DDC diagnostics belong to the manager's active or
bounded recent operation record; GPU readiness and failure belong to
`FTextureResourceCompletion` for the current render revision. These owners are
queried separately. Operation diagnostics retain request identity, timings,
byte metrics, DDC key, cache-hit/rebuild origin, source-decoder invocation, and
the matching failure phase; idle textures do not persist those facts.

Normal-frame completion drains retain the 64-item cap; callback duration is
diagnostic rather than a separate time limit. A 16K source has a 1 GiB decoded
allocation, about 1.33 GiB uncompressed mip chain, and 170.67–341.33 MiB BC
result. Its 2.50–2.67 GiB source/intermediate/result working set is admitted
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
leaf retains a single reflected `FTextureSource` storage field, its typed build
settings and installed/cooked platform data, and its editor import metadata; it
retains no legacy source fields, migration shims, DDC state, or build-operation
diagnostic state. The maintained texture asset corpus uses this canonical
layout directly.

`DTexture` is the sole high-level owner of one stable `FTextureReference`, one
revision/completion contract, and at most one current
`FTextureAssetResource`. The reference and resource are uniquely owned rather
than shared through C++ smart pointers. Each leaf snapshots validated immutable
platform data into its topology-specific resource; the common base owns publication, replacement,
invalidation, release, and deferred cleanup. The concrete resource owns the
uploaded `FTextureRHIRef`; the stable reference owns a counted
`FRHITextureReferenceRef` whose target can change without changing the
consumer-visible binding identity. `FRHITextureReference` derives from
`FRHITexture`, matching the RHI texture type hierarchy, while current renderer
binding paths call `GetReferencedTexture_RenderThread()` before operations that
require a concrete backend allocation.

Material render data, static-mesh scene proxies, accepted preview work, and
thumbnail work retain counted copies of the stable RHI reference. They do not
own the reflected texture asset or a concrete `FTextureResource`. Copying the
stable RHI reference can keep the referenced GPU allocation alive until RHI
deferred deletion, but it never extends the lifetime of the concrete C++
resource object.

Build requests carry monotonically increasing revisions and immutable platform
data to the rendering thread. A candidate concrete resource is initialized and
fully uploaded before publication. If it succeeds and its revision is still
current, publication retargets the stable reference through
`FDynamicRHI::RHIUpdateTextureReference()` in render-command order. This is the
backend extension point for updating descriptor or bindless state together
with the referenced allocation. Existing material, scene, preview, and
thumbnail bindings then observe the replacement without rebinding. A stale or
failed candidate is released and retired without replacing the last successful
target. Missing or not-yet-ready resources resolve through renderer-owned
default textures.

Ordinary replacement and unload are asynchronous. After publication of a new
candidate, `DTexture` releases the old concrete resource through
`FRenderResource` and transfers its C++ storage to ordered deferred RenderCore
cleanup. Asset destruction first prevents further publication, advances the
shared revision, releases and retires the concrete resource, then releases and
retires the base-owned stable reference. Non-owning concrete pointers in
commands are valid only because their release and cleanup commands are queued
after every accepted command that can dereference them.

Lifecycle diagnostics identify the resource type, owning asset package,
revision, lifecycle phase, initialization phase, and pending queue. Producer
code reads the asset's revision-matched completion state rather than retaining
the concrete resource. Upload failures are reported only for the matching
build; a later request clears the prior failure instead of inheriting it
permanently. At shutdown, RenderCore reports these fields for any live registry
or deferred-cleanup entry before RHI teardown.

Before creating a texture, the render resource asks the active RHI whether the
complete structurally valid description is supported. Vulkan queries the exact
format, image type, tiling, usage, flags, extent, mip, layer, and sample
combination. An unsupported description is rejected before image creation and
remains distinguishable from a general creation or upload failure in the
current revision's render completion. The public capability and support
contract is documented in
[RHI Capabilities and Vulkan Startup](RHICapabilitiesAndVulkanStartup.md).

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

`TexturePayloadInspection.h` defines the texture-domain lifecycle summary used
by Texture2D and VolumeTexture. Live summaries cover source, derived, cooked,
decoded CPU, and GPU stages with schema version, texel count, logical/stored
bytes, placement capability, provenance, state, diagnostic, and an explicit
repair classification. They derive those stages from common source metadata,
installed platform data, any available manager operation diagnostic, cooked
bulk, and current render completion. Package summaries are construct-free and
join reflected domain fields with Engine storage inspection; neither form opens
DDC, rebuilds, or creates runtime resources merely to inspect state.

Texture editors render this summary as a read-only Payload Lifecycle section.
Buttons remain attached to explicit Reimport, Reimport From File, build, and
save workflows; the summary itself performs no mutation or source probing.

`TextureEditor` registers a per-resource workspace for `DTexture2D`. It exposes:

- optional reimport hint, canonical imported dimensions/channel semantics,
  transparency, and decoded format, without filesystem availability probing;
- transactional Usage, sRGB, maximum-resolution, and compression-quality
  controls, plus alpha mip mode and coverage threshold;
- platform format, mip count and range, byte size, residency policy, build
  revision, and current platform-data status;
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
controls. Pending diagnostics show phase, request, generation, elapsed
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
