# Texture Source Unification Plan

Summary: Unify texture source topology, image access, identity, and build snapshots across Texture2D, TextureCube, and VolumeTexture.

Last reviewed: 2026-09-05

Status: Active
Completed:

## Current Status

Stages 0 through 2 are complete. Core now owns the shared image value,
conversion, analysis, and codec-facing decoded values. Engine owns the generic
private `FTextureSource` descriptor/payload boundary with a `DTexture` owner,
checked subresource reads, content identity, detached snapshots, and authored
generation. Legacy v1 source fields migrate to the descriptor schema in
PostLoad. Family build/import consumers still use their typed adapters; their
snapshot migration and HDR panorama preservation begin in Stage 3.

## Frozen Source Contract

Source texels use top-left origin and little-endian scalar encoding. Payload
order is block, layer, mip, slice, row, texel, channel. Blocks and layers retain
declaration order. Mips are largest to smallest. A 2D or array slice has depth
one; volume mip depth is `max(1, base depth >> mip)`, while array slice and cube
face counts never shrink. Cube slices use `+X, -X, +Y, -Y, +Z, -Z`. A long/lat
cube block has one 2D slice and is distinguished by its interpretation; it does
not pretend to contain six faces. Layer is an independent channel plane and is
never an array slice.

Every dimension, count, index, multiplication, addition, and narrowing is
checked before allocation or subspan creation. Empty descriptor collections,
zero dimensions/counts, overlapping or non-canonical ranges, incomplete mip
chains, trailing bytes, and payloads above 512 MiB are invalid. Metadata
validation performs no payload read. A read returns an owned immutable buffer
view whose backing allocation outlives the view.

The initial source formats are `G8`, `G16`, `RG8`, `RGBA8`, `RGBA16`, `R16F`,
`RGBA16F`, `R32F`, and `RGBA32F`. `G16` and `RGBA16` are unsigned normalized;
float formats require finite values at provider admission where the recipe
depends on that property. BGRA8 is converted explicitly to RGBA8 on import.
Radiance RGBE is decoded to `RGBA32F` with alpha one; packed RGBE is not a
native source format. This leaves UE parity gaps for deprecated formats,
single-channel signed/integer formats, half-float RGB, and BGRA storage; none is
silently reinterpreted.

Gamma is source interpretation metadata, not a pixel format. It is one of
Linear, sRGB, or Unknown; importers must select it explicitly from codec facts
and family policy. Source content identity is XXH3-128 over a versioned
canonical encoding of interpretation, ordered descriptors, and canonical
decoded bytes. It excludes package paths, import hints, and bulk instance GUIDs.
Build identity additionally includes resolved build settings, target/profile,
provider recipe versions, and payload schema versions.

`DTexture` owns the authoritative monotonically increasing source generation.
An edit supplies one complete value that is validated before `DTexture`
atomically replaces Source and advances its generation on the GameThread. A failed edit preserves source,
identity, generation, platform data, and render state. Build capture snapshots
source, gamma, settings, and generation together; workers never read the live
asset, and completion publishes only when the captured generation and settings
generation still match.

### Consumer And Migration Inventory

- Core decoders and encoder currently own three decoded structs; previews,
  branding, thumbnails, Texture2D/Volume import, Scene import, and cube import
  consume them. Stage 1 moves those values to `Image.h` and keeps codec APIs as
  operations over the shared representation.
- Texture2D build/compile, cube normalize/build, volume build, editor previews,
  thumbnails, payload inspection, PostLoad, Cook, and reimport rollback all read
  or reconstruct family-specific source values. Stage 3 switches them to one
  detached generic snapshot; typed values remain only at recipe boundaries.
- Texture2D compilation already applies revisioned completion. Cube and volume
  builds are synchronous. All result application remains last-known-good and
  generation checked when asynchronous work is used.
- DAST v9 reflected field signatures are strict. Therefore the existing
  `DTexture::Source` v1 fields remain readable as a legacy struct and migrate in
  `PostLoad` to the new descriptor form; new saves emit only the new schema.
  Existing RGBA8 2D, projected RGBA8 cube, and five volume formats migrate
  losslessly. A legacy projected cube remains a six-face source. Recovering its
  pre-projection HDR panorama, or precision already discarded by an old import,
  requires explicit reimport and an available source hint.

## Goal

Make the existing base-owned Source the authoritative authored image model.
Preserve original decoded precision and topology, expose checked subresource
access and immutable build snapshots, and rebuild supported texture families
without reopening imported files. Align with UE source semantics while retaining
Durin's immutable EditorBulkData and package ownership boundaries.

## Selected Design

- Core owns `Image/Image.h`: raw image format, gamma interpretation, image
  description, read-only view, and owning image value. These types contain no
  asset, RHI, package, or DDC dependencies.
- `ImageDecoder.h` and `ImageEncoder.h` include the shared image types and retain
  encoded-bytes-to-image and image-to-encoded-bytes operations respectively.
  Migrate their existing decoded structs to the common image model. Do not
  merely rename the decoder header or make the foundational header depend on
  codec APIs. Conversion and analysis algorithms may live in a separate
  `ImageUtils.h` if required by their size.
- Engine owns `TextureSource.h/.cpp`, with private reflected source metadata,
  checked initialization, validation, identity, and subresource reads. `DTexture`
  remains the sole persistent Source owner; Source remains EditorOnly.
- Describe blocks, per-layer formats, mip counts, and slice interpretation.
  Source layout is independent of the platform GPU format. Volume depth shrinks
  with mip level; array and cube face counts do not. Layer is distinct from
  array slice. Long/lat cube source remains a single image per cube.
- Source validation establishes a structurally valid image. Family/provider
  admission independently rejects unsupported topology or formats with an
  explicit diagnostic, never silently discarding layers, blocks, or mips.
- Reuse immutable `FEditorBulkData` and package-resource requests. Read views
  retain buffer ownership. Editing uses complete-value validation and atomic replacement;
  failed edits preserve the previous source, identity, and accepted build state.
  Do not introduce mutable editor bulk locks merely to copy UE API names.
- Source identity covers canonical content and its interpretation/layout.
  Build keys additionally cover settings, target, and recipe version. Package
  paths and bulk instance GUIDs do not become content identity.
- Capture source, gamma interpretation, settings, and generation together on
  the owning thread. Workers use immutable detached values and never consult a
  live texture. Completion validates the captured generation before publishing.
- Keep projection, exposure, mip generation, and platform encoding in build
  recipes. Preserve decoded HDR panoramas in Source rather than persisting only
  projected RGBA8 faces. Import file hints and hashes remain in import data.

## Scope And Deferred Capabilities

Required: Core image foundation, generic multi-mip/layer/block source storage and
access, existing 2D/Cube/Volume integration, HDR panorama preservation, source
mutation/build coherence, asset migration, and bounded lossless source storage.

Deferred: new array asset classes, UDIM importer UI, virtual-texture runtime and
page building, remote editor payload virtualization, additional codec families
such as EXR, and full ICC/OCIO color management. Source representation support
does not claim a corresponding importer or runtime consumer exists. Follow-ups
must name any supported source formats the current build recipes still reject.

## Implementation Stages

### Stage 0: Freeze layout, format, and migration contracts

- [x] Audit current source consumers, family identities, asynchronous completion,
  reflection/schema loading, source preview, and import rollback paths.
- [x] Freeze block/layer/mip/slice byte ordering, top-left origin, cube face
  order, volume interpretation, index bounds, and checked size/offset arithmetic.
- [x] Freeze the initial format table: retain current R8/RG8/RGBA8/R16F/RGBA16F;
  add G16, RGBA16 UNORM, R32F, and RGBA32F equivalents. Decide whether BGRA8 and
  packed RGBE need native storage or explicit import conversion; document any
  remaining UE format gap without copying deprecated enum values.
- [x] Specify source content identity versus build identity, gamma ownership,
  edit failure behavior, and authoritative source generation.
- [x] Resolve old schema admission and migration through the existing package
  compatibility mechanism; identify assets requiring external-source reimport.

Completion: one unambiguous layout/format/migration contract and a consumer
inventory with no unresolved choices blocking Stage 1.

### Stage 1: Introduce Core image values and migrate codecs

Depends on Stage 0.

- [x] Add `Image.h` types with checked byte sizes, format information, gamma,
  slice count, buffer ownership, and safe view lifetimes.
- [x] Supply required integer/float conversions, gamma-aware conversion, and
  channel analysis without coupling to texture build settings.
- [x] Move shared decoded image representation out of `ImageDecoder.h`; migrate
  decoder/encoder, preview, importer, and recipe callers while preserving exact
  grayscale16 samples and HDR values.
- [x] Validate malformed sizes, overflow, format conversion, gamma behavior,
  view ownership, grayscale16 fidelity, and existing codec round trips.

Completion: current codec consumers compile against the common image values;
existing decoding behavior and error contracts remain covered.

### Stage 2: Implement the generic texture source

Depends on Stage 1.

- [x] Extract source declarations from `Texture.h`; implement block/layer/mip
  descriptors, 32-bit slice counts, long/lat interpretation, and bounded payloads.
- [x] Add atomic initialization, reset, metadata validation, mip image info,
  checked offsets/sizes, owned read views, and explicit asynchronous/ blocking
  read APIs that preserve package failure diagnostics.
- [x] Implement generic identity and detached immutable snapshots; remove
  public mutation paths that can leave descriptors inconsistent with payloads.
- [x] Implement complete-value editing and source generation integration with
  `DTexture`; invalid kind, invalid indices, and arithmetic overflow fail safely.
- [x] Verify multi-block/layer/mip layouts, volume depth reduction, fixed array
  slice counts, metadata-only identity, snapshot isolation, and failed edits.

Completion: source storage and access work independently of family recipes;
unsupported build combinations can be represented without being accepted by a
provider. Source edits have a single observable commit boundary.

### Stage 3: Migrate import and build consumers

Depends on Stage 2.

- [ ] Replace duplicate persistent-source vocabulary and family identity code
  with generic source snapshots; retain only necessary typed recipe settings.
- [ ] Migrate Texture2D, Cube, Volume, Scene texture imports, previews, and
  thumbnails. Coordinate importer entrypoints with the active
  [Content Browser Import Extensions plan](ContentBrowserImportExtensions.md).
- [ ] Persist decoded full-precision panoramas; derive cube faces and exposure
  in the recipe. Rebuild after parameter changes without imported-file access.
- [ ] Define supplied-mip preservation versus mip generation explicitly. Publish
  a tested source-format/provider capability matrix with actionable rejection.
- [ ] Update DDC keys/recipe versions and completion validation so old results
  cannot overwrite a newer source/settings generation.
- [ ] Validate all three families, HDR panorama rebuild with the original file
  unavailable, preserved source mips, cache hit/miss behavior, and stale completion.

Completion: existing families consume a single authoritative Source; duplicate
source models and migration adapters are removed once their consumers migrate.

### Stage 4: Complete source storage and package migration

Depends on Stage 3; migration mechanics are selected in Stage 0.

- [ ] Separate source pixel format from source storage compression. Start with
  raw plus a selected bounded lossless codec; specify encoded-payload identity
  versus canonical decoded-source identity before enabling compression.
- [ ] Preserve current package Bulk Directory semantics. If generic compressed
  package flags are needed, implement their package contract explicitly rather
  than silently changing the meaning of existing EditorBulkData hashes.
- [ ] Define residency release/reload and reset behavior over immutable package
  sources, including cancellation, retirement, and corrupt compressed data.
- [ ] Migrate old single-mip sources without invented precision. Old projected
  RGBA8 cubes remain usable; recovering original HDR panoramas requires reimport.
- [ ] Verify authored save/load, copy/duplicate, transaction snapshots, failed
  save/edit rollback, and cooked editor-only stripping for all three families.

Completion: new sources survive package round trips; legacy assets either load
through a verified migration or produce a documented actionable compatibility
diagnostic. Cooked runtime loading never requires Source or a source codec.

### Stage 5: Qualify and publish the final contracts

Depends on Stage 4.

- [ ] Run the targeted native suites and required editor/game build checks using
  the repository [build](../Agents/BuildAndRun.md) and
  [testing](../Agents/Testing.md) workflows.
- [ ] Measure bounded metadata access, first mip read, snapshot copy, and peak
  rebuild memory; freeze budgets derived from explicit representative fixtures.
- [ ] Update implemented image, asset lifecycle, bulk, and volume contracts;
  record remaining format/codec/VT/virtualization gaps with concrete owners.
- [ ] Remove obsolete adapters, refresh status with validation evidence, and
  complete the plan only after all required gates pass.

Completion: documented source semantics match tested behavior, and deferred
capabilities are distinguishable from implemented support.

## References

- [Core image codec](../Runtime/Core/ImageCodec.md)
- [Package bulk data](../Runtime/Assets/BulkData.md)
- [Asset data lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Volume textures](../Runtime/Assets/VolumeTextures.md)
- [UE FTextureSource API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FTextureSource)

## Related Code

- `Engine/Source/Runtime/Core/Public/Image/ImageDecoder.h`
- `Engine/Source/Runtime/Core/Public/Image/ImageEncoder.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture.h`
- `Engine/Source/Runtime/Engine/Public/Asset/EditorBulkData.h`
- `Engine/Source/Runtime/Engine/Private/Texture/`
- `Engine/Source/Developer/TextureBuild/`
- `Engine/Source/Editor/AssetForgeBuiltins/`
- `Engine/Source/Editor/TextureEditor/`
