# Volume Textures

Summary: Define source import, normalized data, deterministic build, TXPL cook,
and revisioned GPU-resource contracts for package-backed volume textures.

Modules: Engine, TextureBuild, RHI, VulkanRHI

Last reviewed: 2026-08-22

## Asset boundary

`DVolumeTexture` is a `DTexture` leaf for one non-array 3D color texture. Its
authored package stores reflected `FVolumeTextureSourceData` and
`FVolumeTextureBuildSettings`. Source data is a validated tightly packed voxel
Blob (`std::vector<std::byte>`) with width, height, depth, and one of five portable formats:
`R8_UNORM`, `RG8_UNORM`, `RGBA8_UNORM`, `R16_FLOAT`, or `RGBA16_FLOAT`.
Dimensions are nonzero and no greater than 2048; the common texture payload byte
ceiling is authoritative. Materials, streaming, and volume-rendering algorithms
are outside this boundary.

The source Blob is one atomic reflected `Bytes` value. Its default-planning
cost is independent of voxel count, while package and allocation byte ceilings
still apply. Historical authored packages from before
`FVolumeTextureSourceVersion::ByteBlob` load the old `Voxels` logical
`Array<UInt8>` through the load-only `Voxels_DEPRECATED` route, convert each
value exactly, validate the complete source, and resave only canonical Blob
bytes. Packages containing both representations are rejected transactionally.

## PNG atlas source import

The existing texture import dialog can interpret a selected PNG as either a
normal `DTexture2D` or a `DVolumeTexture`. Volume mode currently supports one
`PNG Row-Major Atlas`: each tile is one Z slice, tiles advance left-to-right and
then top-to-bottom, and unused cells after `depth` are ignored. The user supplies
slice width and height, depth, tile columns and rows, and one of `red`, `green`,
`blue`, `alpha`, `luminance`, or `rgba`. Scalar selections produce `R8_UNORM`;
`rgba` preserves all four channels as `RGBA8_UNORM`. Luminance is
`(54R + 183G + 19B + 128) / 256` using integer arithmetic.

The PNG dimensions must exactly equal `(slice width * columns) x (slice height *
rows)`, the grid must contain at least `depth` cells, and decoded image and volume
sizes are bounded before allocation. `FVolumeTextureSourceImportData` serializes
the mounted PNG path and XXH3-128 hash together with the visible import format,
channel selection, slice dimensions, depth, grid, and decoder version. Generic
Details exposes the source path and interpretation as read-only asset properties.

External imports copy the PNG beneath the selected mounted source location, save
the `.dasset`, and only then commit the source copy. The shared `DurinImage`
provider supplies immutable snapshots, reimport, and source repair. A missing or
malformed PNG, extent mismatch, build failure, stale publication, or save failure
leaves the previous asset and render resource intact. Cook excludes normalized
authoring source and provenance by default and never loads a PNG decoder at
runtime.

Runtime platform data owns the selected `EPixelFormat` and a complete mip chain.
Every `FVolumeTextureMipData` records width, height, depth, exact row pitch,
exact depth pitch, and owned bytes. Successive axes independently halve with
`max(1, previous / 2)` until the final `1x1x1` mip. Validation rejects missing
tail mips, incorrect pitches or byte counts, unsupported formats, excessive
dimensions, and malformed progression before publication.

## Deterministic build and cache

TextureBuild owns the registered volume recipe. It consumes normalized voxels,
uses a three-axis box filter in linear numeric space, and deterministically
builds the complete chain for all five formats. Odd extents include each valid
source voxel exactly once in the corresponding clamped two-texel footprint;
floating inputs must be finite. Numeric filtering explicitly converts at the
`std::byte` boundary; TXPL, DDC, mip, cook, and RHI upload bytes retain their
existing `uint8` platform representation and byte identity.

The canonical DDC key includes source bytes and dimensions, source/output
format, mip filter, builder and payload schema versions, and Win64/Game target
identity. A validated cache hit and a rebuild publish the same platform value.
Corrupt or incompatible entries are misses; a failed candidate never replaces
the asset's last-known-good CPU or GPU result. Engine reaches the uncooked
post-load policy through `IVolumeTextureAuthoringFeature`, preserving the
Engine-to-TextureBuild dependency direction.

## Authored source bulk data

Normalized source voxels use `Asset::FAuthoredBulkData` with stable payload id
words `{6fe21a38, 494340a7, a304c2d5, 26f22931}`, format id words
`{2854a7c1, 94cb4ab8, 8cd8be32, c2f680b7}`, and format version 1. Dimensions,
portable voxel format, and import provenance remain ordinary reflected fields.
The source accessor never performs IO; build and import paths require verified
resident bytes and replace the complete payload atomically.

Volume source custom version 2 is the current authored-bulk representation.
Load-only compatibility first converts the historical `Array<UInt8>` route to
the version-1 byte Blob and then converts that Blob to bulk data. Current saves
emit only the bulk field. The 256 KiB authoring threshold changes placement,
not reflection identity, DDC key input, mip bytes, TXPL, cooked DBLK, or upload
bytes. The production `16384 x 128` atlas therefore keeps its exact normalized
2 MiB source while ordinary `.dasset` Value bytes contain only the descriptor.

## Payload and cook

Volume data uses TXPL schema 1 with texture dimension value 3. Stable pixel
format identifiers 8 through 12 were appended for the five portable formats;
existing 2D and cube identifiers retain their meanings. Volume records reuse
the fixed 40-byte record layout and store depth and depth pitch in fields whose
interpretation is selected by the dimension. Ranges are 16-byte aligned,
non-overlapping, checksummed with XXH3-128, bounded, and validated across all
three axes before allocation.

The volume producer version is 1 and the primary cooked payload ID is
`672b164e-4e19-4871-a7b8-41dfe3208b15`. Cook accepts only Win64/Game and emits
one uncompressed PackageCompanion payload. Cooked loading requires the matching
descriptor and valid payload, strips authored source by default, does not query
DDC or invoke an importer, and fails the asset load transactionally on missing
or corrupt bulk.

## GPU resource and diagnostics

`FVolumeTextureResource` creates a public `Texture3D` descriptor with sampled
usage, qualifies the exact format/extent/mip set, and uploads every mip through
`UpdateTexture3D` using its row and depth pitch. Publication uses the shared
`DTexture` reference, revision, completion, replacement, deferred cleanup, and
last-known-good rules. Engine never inspects Vulkan handles.

Logical payload bytes are the sum of exact mip voxel byte counts. Upload bytes
are the same sum because each mip is recorded once with no caller padding.
Backend allocation bytes are exposed per texture by
`FRHITexture::GetBackendAllocationBytes()` and may exceed logical bytes because
of device alignment and tiling; they are deliberately not a serialized asset
constant. For the qualification fixture, a `1x1x2 RGBA8` source records 8
logical/upload bytes, and the Vulkan test asserts a nonzero allocation at least
that size. Memory and transfer diagnostics remain bounded by the existing RHI
snapshot counters rather than retaining per-volume history.

## Public RHI contract

`FRHITexture::GetSizeZ()` is authoritative for volume depth. 2D and cube
textures report one. `FUpdateTextureRegion3D` carries independent source and
destination XYZ offsets, extent, row pitch, and depth pitch. Validation checks
block geometry, pitches, footprint arithmetic, mip bounds, and destination
bounds before recording. Recorded commands pack and own exactly the referenced
source box, so caller memory may change immediately in inline or threaded mode.

3D views use one layer and may be sampled or storage views. Generic texture copy
regions use their existing Z offset and depth extent. Public transitions track
the mip subresource; Z slices do not become array-layer states. Vulkan maps the
resource to `vk::ImageType::e3D`, `arrayLayers = 1`, and
`vk::ImageViewType::e3D`, while `RHIIsTextureSupported` remains authoritative
for every exact format and usage combination.

## Related Documentation

- [Asset data lifecycle](AssetDataLifecycle.md)
- [Texture system](../Rendering/TextureSystem.md)
- [RHI capabilities and Vulkan startup](../Rendering/RHICapabilitiesAndVulkanStartup.md)
- [Render resource lifecycle](../Rendering/RenderResourceLifecycle.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTextureDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTextureRenderResource.cpp`
- `Engine/Source/Developer/TextureBuild/Private/Texture/VolumeTextureBuilder.cpp`
- `Engine/Source/Editor/AssetForge/Private/VolumeTextureSourceTranslation.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
