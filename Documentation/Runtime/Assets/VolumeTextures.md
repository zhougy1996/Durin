# Volume Textures

Summary: Define source import, normalized data, deterministic build, cooked payload,
and revisioned GPU-resource contracts for package-backed volume textures.

Modules: Engine, TextureBuild, AssetForgeBuiltins, RHI, VulkanRHI

Last reviewed: 2026-09-02

## Asset boundary

`DVolumeTexture` is a `DTexture` leaf for one non-array 3D color texture. Its
authored package stores reflected `FVolumeTextureSourceData` and
`FVolumeTextureBuildSettings`. Source data is validated tightly packed authored
BulkData with width, height, depth, and one of five portable formats:
`R8_UNORM`, `RG8_UNORM`, `RGBA8_UNORM`, `R16_FLOAT`, or `RGBA16_FLOAT`.
Dimensions are nonzero and no greater than 2048; the common texture payload byte
ceiling is authoritative. Materials, streaming, and volume-rendering algorithms
are outside this boundary.

The source payload is one atomic reflected `BulkData` value. Its default-planning
cost is independent of voxel count, while package and allocation byte ceilings
still apply. The supported baseline contains only this current schema; the
historical `Array<UInt8>` and byte-Blob voxel fields are no longer registered or
loaded. Assets from those retired schemas must be upgraded by a compatible
older build before entering the current repository baseline.

## PNG atlas source import

The existing texture import dialog can interpret a selected PNG as either a
normal `DTexture2D` or a `DVolumeTexture`. Volume mode currently supports one
`PNG Row-Major Atlas`: each tile is one Z slice, tiles advance left-to-right and
then top-to-bottom, and unused cells after `depth` are ignored. Selecting a PNG
decodes its actual dimensions and sampled channel content without interpreting
its filename. Cubic layouts whose slice size divides both atlas axes are ranked
by power-of-two dimensions and atlas-cell utilization. A uniquely strong
candidate is applied automatically; ambiguous candidates remain explicit user
choices, and non-cubic layouts remain available through advanced slice width,
height, depth, column, and row fields. The inferred or user-selected channel is
one of `red`, `green`, `blue`, `alpha`, `luminance`, or `rgba`. Scalar selections
produce `R8_UNORM`; `rgba` preserves all four channels as `RGBA8_UNORM`. Luminance is
`(54R + 183G + 19B + 128) / 256` using integer arithmetic.

The PNG dimensions must exactly equal `(slice width * columns) x (slice height *
rows)`, the grid must contain at least `depth` cells, and decoded image and
volume sizes are bounded before allocation. `DVolumeTextureImportData`
serializes an optional explicitly based source hint and XXH3-128 provenance
together with channel selection, slice dimensions, depth, and grid. PNG
row-major atlas interpretation and the decoder version are importer behavior,
not persisted replay selectors. Details may display the hint and
interpretation as read-only properties, but never probes the physical file.

Import and Reimport From File capture the selected PNG without copying or
moving it. The shared `DurinImage` decoder translates that immutable capture
into canonical voxels before live-state commit. Reimport resolves the retained
hint only after explicit invocation. A missing or malformed PNG, extent
mismatch, build failure, or cancellation leaves the previous asset and render
resource intact; a save failure preserves the prior disk bundle and leaves the
complete new live state Dirty. Cook excludes normalized authoring data and
provenance by default and never loads a PNG decoder at runtime.

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
`std::byte` boundary; cached, cooked, mip, and RHI upload bytes retain their
`uint8` platform representation and byte identity.

The canonical DDC key includes the canonical voxel identity and dimensions,
source/output format, mip filter, builder and payload schema versions, and
Win64/Game target identity. It excludes source hints and physical files. A
validated cache hit and a rebuild publish the same platform value.
Corrupt or incompatible entries are misses; a failed candidate never replaces
the asset's last-known-good CPU or GPU result. Engine invokes the typed
`IVolumeTextureBuildProvider` for authored builds and uncooked PostLoad, then
validates and publishes the derived-only result on the GameThread. TextureBuild
never receives or mutates a `DVolumeTexture`.

## Authored source bulk data

Normalized source voxels use `FEditorBulkData` with stable storage
payload id words `{6fe21a38, 494340a7, a304c2d5, 26f22931}`. Source payload
schema version 1, dimensions, portable voxel format, and import provenance are
ordinary reflected VolumeTexture fields. Old packages that predate the schema
field receive its v1 default; unsupported values fail domain validation.
The source identity accessor performs no I/O. Build keys use that identity
before bytes are requested; a validated DDC hit reads no package range, while a
miss obtains one immutable owned snapshot through `GetPayload()`. Import and
reimport replace the complete payload atomically. The VolumeTexture fields
define voxel meaning and require a tightly
packed row-major depth-slice encoding whose exact byte width comes from
`EVolumeTextureFormat`; DAST/package-resource
placement and replacement remain authored-only capabilities.

Ordinary and explicit saves emit canonical DURF/DAST v9 and only the authored
BulkData field. Small voxel values stay in Inline Bulk. External values produce
one matching Bulk Directory record and a range in the stable headerless
`<package-stem>.dbulk` segment. Other DAST versions and nested authored-bulk
containers are unsupported in production.

The 256 KiB authoring threshold changes placement,
not reflection identity, DDC key input, platform payload, cooked field, or upload
bytes. The production `16384 x 128` atlas therefore keeps its exact normalized
2 MiB source while ordinary `.dasset` Value bytes contain only the descriptor.

## Payload and cook

Cook projects the validated VolumeTexture TXPL value into the cooked
`PlatformData` BulkData field. Post-load validates target/profile field
metadata without reading the range. First platform-data or resource access
locks the field and decodes transactionally into
`FVolumeTexturePlatformData`; no common descriptor or provider translation
participates.

The owner-selected texture payload schema 2 uses dimension value 3. Stable pixel
format identifiers 8 through 12 were appended for the five portable formats;
existing 2D and cube identifiers retain their meanings. Volume records reuse
the fixed 40-byte record layout and store depth and depth pitch in fields whose
interpretation is selected by the dimension. Ranges are 16-byte aligned,
non-overlapping, checksummed with XXH3-128, bounded, and validated across all
three axes before allocation.

The volume producer version is 2 and the primary cooked payload ID is
`672b164e-4e19-4871-a7b8-41dfe3208b15`. Cook accepts only Win64/Game and emits
one uncompressed field value. Cooked loading requires valid field metadata and
payload, strips authored source by default, does not query DDC or invoke an
importer, and fails transactionally on missing or corrupt bulk. New output is
canonical DAST v9 plus its exact optional headerless raw `.dbulk` segment.

## GPU resource and diagnostics

VolumeTexture participates in the shared texture-domain inspection contract,
not a generic bulk element registry. Construct-free inspection reads the nested
source schema/dimensions and `FEditorBulkData` field metadata, and validates the
referenced raw segment without modifying recovery state. Generation-named
companions are not a supported production route. Live
inspection independently reports source, DDC/platform,
cooked field/segment, decoded CPU, and GPU stages. Missing/corrupt authored bulk
maps to restore, canonical resave, or reimport; DDC failure maps to rebuild,
cooked failure maps to recook,
and GPU failure maps to resource retry.

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
- `Engine/Source/Editor/AssetForgeBuiltins/Private/VolumeTextureImport.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
