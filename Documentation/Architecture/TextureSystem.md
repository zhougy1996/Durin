# Texture System

Durin's current texture path is an editor-oriented Texture2D pipeline with
explicit source, platform, render-resource, editor, and material boundaries.

## Asset and Build Ownership

- `DTexture2D` owns the copied source-file reference plus the reflected `Usage`,
  `bSRGB`, `MaxResolution`, `CompressionQuality`, `AlphaMipMode`, and
  `AlphaCoverageThreshold` build settings.
- The package also retains the imported source-content hash, source file
  fingerprint, dimensions, channel count, and transparency. These lightweight
  fields preserve diagnostics and derived-data identity without keeping decoded
  pixels resident.
- `FTextureSourceData` is decoded RGBA8 edit data. It records source dimensions,
  original channel count, and whether transparency is present. A warm derived-
  data load leaves it non-resident; changing a build setting decodes it on demand
  before validating the candidate build.
- `FTexturePlatformData` is rebuilt from source data. It contains a complete,
  tightly packed desktop BC mip chain selected from usage, transparency, and
  color space.
- Source and platform data are intentionally separate. Platform cache hits and
  rebuilds replace platform data without mutating a resident decoded source
  representation.
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

Texture2D platform mip chains are content-addressed beneath
`DerivedDataCache/Textures/Objects/` as `.bin` objects. A canonical 128-bit key
includes the imported source-content hash, usage, explicit color-space choice,
maximum resolution, compression quality, alpha-mip policy and threshold, target
platform, and texture-builder version.

`PostLoad` first requires the editor source file and compares its size and stable
last-write time with the package fingerprint. An unchanged source can restore
the checksummed, versioned platform payload without reopening or decoding the
image. A changed fingerprint decodes the source, recomputes its content hash,
builds a new key and payload, and dirties the package so the new source identity
can be saved. Missing, incompatible, corrupt, truncated, oversized, or invalid
cache data is a non-fatal miss and rebuilds from source. Atomic cache persistence
failure does not discard valid in-memory platform data.

The DDC path is derived entirely from the key; `.dasset` never stores a cache
file path or byte offset. Cooked source-free platform payloads and any future
external `.dbulk` descriptor are not implemented by this editor cache. The
selected cross-asset storage and cooked companion contract is documented in
[Asset Data Lifecycle and Storage](AssetDataLifecycle.md).

## Transactional Build-Setting Edits

The Texture Editor changes `Usage`, `bSRGB`, `MaxResolution`,
`CompressionQuality`, `AlphaMipMode`, and `AlphaCoverageThreshold` through
reflected-property transactions.
`DTexture2D::PreEditChangeProperty` builds complete candidate platform data
from detached proposal storage before the live setting changes. An invalid
usage or quality value, or any failed build, rejects the proposal without
changing the asset.

After a successful write, `PostEditChangeProperty` atomically installs the
validated candidate and queues a new render-resource revision. Cancel, Undo,
and Redo use the same hooks and therefore rebuild the matching platform data.
Changing usage resets sRGB to that preset's default; editing sRGB afterward is
an explicit override. Committed edits dirty the package through the shared
reflected transaction path. Direct build-setting setters follow the same
rebuild rule and dirty the package after success.

## Render-Thread Boundary

Each texture owns one shared `FTexture2DRenderResource` proxy. Game-thread code
may retain the proxy but never reads its RHI texture. Build and release requests
carry monotonically increasing revisions to the render thread. Stale commands
cannot replace newer data, and every mip is uploaded before the new RHI texture
becomes the applied revision. Diagnostic resource state is tagged with the
request revision, and upload failures are reported only for the matching build;
a later request clears the prior failure instead of inheriting it permanently.

Material render data and static-mesh scene proxies retain the shared proxy, not
a reflected texture object or a raw RHI texture. Rebuilding a texture updates
that same proxy, so already-bound materials and previews observe the replacement
without rebinding reflected dependencies. Missing or not-yet-ready resources
resolve through renderer-owned default textures.

Before creating a texture, the render resource asks the active RHI whether the
selected format supports the requested optimal-tiling usage. Vulkan derives this
answer from physical-device format features. An unsupported format is rejected
before image creation and remains distinguishable from a general creation or
upload failure in the asset's persistent editor diagnostics.

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

`TextureEditor` registers a per-resource workspace for `DTexture2D`. It exposes:

- source file, dimensions, source channel count, transparency, and decoded format;
- transactional Usage, sRGB, maximum-resolution, and compression-quality
  controls, plus alpha mip mode and coverage threshold;
- platform format, mip count and range, byte size, residency policy, build
  revision, and current platform-data status;
- normal workspace save, Dirty, close protection, Undo, and Redo behavior.

The editor previews the built platform representation and allows each mip level
to be selected. Every open texture document owns independent preview state and
one registered preview texture, so simultaneously visible documents cannot
reuse or overwrite one another's image. Missing or invalid platform data falls
back to decoded source data when available; otherwise the preview is released.
Persistent source, decode, build, upload, and format status is shown with retry
controls. Content Browser thumbnails remain persistent derivatives of the
copied source image rather than the built platform representation.

## Current Limitations

- Texture2D has an editor derived-data cache, but there is no cooked platform
  payload and runtime packages still require the source image to exist.
- Build work is synchronous, every mip is fully resident, and there is no memory
  accounting or streaming.
- The shipped material shader consumes only the base-color texture parameter.

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Editor/TextureEditor/`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialTypes.cpp`
- `Engine/Shaders/Slang/StaticMesh.slang`
