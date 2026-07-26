# Asset Derived Data and Cooking Plan

Summary: Shared source provenance, derived-data, cooked bulk, and runtime-loading architecture for static meshes, Texture2D, and TextureCube.

Last reviewed: 2026-07-27

## Current Status

This plan supersedes the narrower Static Mesh Derived Data and Cooking plan
without discarding its completed work. The predecessor's StaticMesh Stages 0
through 3 are complete: Durin has optional relocatable mesh source provenance,
a strict deterministic DMSH codec, an atomic content-addressed object store, and
an editor load path that can consume cached mesh data while source art is
unavailable.

Stages 0 and 1 are complete. DBLK version 1, logical cooked-payload descriptors, the
binary cook manifest, publication order, explicit package-load mode, TXPL
version 1, texture and cube keys, source provenance, migration policy, and the
runtime/editor module boundary are frozen below. Logical fixtures for DBLK,
Texture2D, six-face TextureCube, panorama-derived TextureCube, and malformed
inputs live beneath the owning native-test data directories and require no
importer, compressor, RHI, or window. AssetCore now implements deterministic
DBLK and manifest codecs, reflected path-free descriptors, contained companion
resolution, manifest-bounded cook publication and stale cleanup, and an
explicit process package-load context. The complete 47-test AssetCoreTests
suite passes with multi-payload, corruption, relocation, interruption, and
cleanup coverage.

StaticMesh is now the first consumer connected to the shared cook layer. Its
cook adapter deterministically contributes DMSH bytes, serializes a matching
logical descriptor with runtime metadata, and its cooked `PostLoad` path
validates the companion DBLK, descriptor, target, DMSH, and material-slot
mapping before transactional publication. Assimp and its synchronous/asynchronous
source-model adapter now live in editor-only `AssetImport`; runtime-only Engine
builds compile source rebuilding out and deploy neither the module nor Assimp.
Clean-cook, source/DDC isolation, missing bulk, wrong target/schema, corrupt
payload, material-slot mismatch, and runtime render-data smoke coverage are in
place. Stage 2 is complete.

Stages 3 and 4 are complete. Texture2D now uses the canonical key schema, strict TXPL
version 1 codec, shared content-addressed object store, bounded cleanup, and
shared cache diagnostics. Authored loads can consume persisted source identity
when the source image is unavailable, and decode, rebuild, cache-read, and
cache-write failures retain the last complete live platform data. New imports
store normalized project- or engine-relative provenance beneath
`SourceAssets/Textures`; move and delete preserve potentially shared source art,
repair/reimport is transactional, the Texture Editor reports provenance state,
and legacy package-adjacent sources remain readable during migration. Its cook
adapter reuses matching validated TXPL objects, emits descriptor-bearing
source-free runtime packages plus DBLK companions, and cooked loads validate
the complete target, descriptor, bulk, format, mip, and checksum chain before
Vulkan resource publication. Runtime-only Engine builds exclude offline BC
encoder linkage. Deterministic cook, source/DDC isolation, corrupt and
incompatible input, all supported BC identifiers, NPOT/tail mip, and Vulkan
upload/sample coverage are in place. TextureCube still rebuilds from source
during `PostLoad`, and its source/build tooling remains migration work for
Stage 5.

## Goal

Give StaticMesh, Texture2D, and TextureCube one coherent authored-source,
derived-data, cooking, and runtime-loading lifecycle. Normal editor loads reuse
validated native payloads, cooked packages own deployable platform data, and
runtime targets load without source files, Assimp, image decoders, panorama
projection, or offline texture compressors.

## Scope

- Preserve and consume the completed StaticMesh provenance, DMSH codec, key,
  DDC, and editor-load work.
- Portable, optional editor source provenance for Texture2D and TextureCube,
  including six-face and equirectangular source layouts.
- Canonical derived-data keys and strict native platform-payload codecs for all
  three asset classes.
- Shared content-addressed DDC object-store policy and diagnostics.
- A generic DBLK cooked-bulk container, logical payload descriptor, cook
  context, deterministic output manifest, and runtime load mode.
- StaticMesh, Texture2D, and TextureCube cooker integrations.
- Editor load policy for DDC hits, safe source rebuilds, missing sources, and
  failures; runtime load policy with no DDC or source fallback.
- Separation of editor-only import/build dependencies from runtime payload
  decoding and render-resource creation.
- Project and engine source-model placement outside runtime-mounted Content.
- Project and engine source-image placement outside runtime-mounted Content.
- Migration of built-in material-preview meshes to shared `/Engine` assets.
- Unit, integration, corruption, determinism, cook-isolation, and
  editor/runtime validation.

## Non-Goals

- Creating a new DCC interchange format to replace OBJ, FBX, or glTF.
- Runtime importing of arbitrary user models.
- Runtime decoding of PNG, JPEG, BMP, TGA, HDR, or other authoring formats.
- Mesh streaming, virtualized geometry, meshlets, or GPU-driven cluster data.
- New LOD generation, collision generation, lightmap unwrapping, or mesh
  optimization algorithms beyond preserving the current build result.
- Asynchronous import, build, DDC read, or cooked-payload loading in this plan.
- Texture streaming, sparse residency, virtual textures, or changing the
  current full-residency policy.
- HDR or floating-point runtime TextureCube output; current panorama import may
  continue producing the existing LDR cube platform representation.
- A final archive, patch-generation system, install-chunk system, or cooker for
  every future asset class. The loose DBLK and manifest contracts must permit
  those systems to replace physical placement later.
- Deleting source models from version control.
- Deleting source images from version control.

## Design Decisions and Invariants

### Cross-asset lifecycle

- A `.dasset` is the stable referenced asset and contains compact authored or
  cooked object metadata. Asset references never identify source files, DDC
  objects, DBLK files, byte offsets, or workstation paths.
- `SourceAssets` contains authoritative editor inputs. It is versioned but is
  neither a runtime content mount nor cook output.
- DDC `.bin` objects are content-addressed, rebuildable, unreferenced
  accelerators. Cooked `.dbulk` is manifest-owned deployable data. Identical
  asset-specific payload bytes may appear in both storage classes, but runtime
  packages never reference DDC paths or keys.
- AssetCore owns generic atomic object storage, DBLK framing and validation,
  logical payload descriptors, package-relative lookup, cook publication, and
  manifest mechanics. It does not interpret DMSH or texture payload bytes.
- Engine owns asset-specific source provenance, canonical key contribution,
  builder and payload-schema versions, codecs, build policy, and conversion to
  runtime render data.
- Editor/import modules own Assimp, source-image decoding, panorama projection,
  offline texture compression, reimport UI, and source-path repair. Runtime
  modules own only cooked-payload validation, decode, and render-resource
  publication.
- Editor load order is cooked/native payload when explicitly requested, valid
  DDC, source rebuild, then actionable failure. A valid DDC object may load
  while source art is missing.
- Cooked runtime load order is cooked package descriptor, DBLK validation,
  asset-specific decode, then transactional publication. It never falls back to
  source, DDC, importers, projection, or offline builders.
- Cook publication writes and validates bulk data before publishing the package
  and manifest that reference it. Failed output remains unreferenced and
  incomplete temporary files are removed.
- Keys encode source content identity, every semantic build setting, builder
  version, payload schema, target platform, and relevant feature profile using
  explicit little-endian fields. Paths, timestamps, enum storage size, padding,
  and formatted diagnostic strings are not key inputs.

### Texture payload family

- Texture2D and TextureCube share one strict texture-platform payload family
  unless Stage 0 fixtures prove that separate schemas are required. The
  envelope identifies dimension, pixel format, array-slice count, mip count,
  and builder/schema/platform versions.
- Texture2D has exactly one array slice. TextureCube has exactly six slices in
  the documented PositiveX, NegativeX, PositiveY, NegativeY, PositiveZ,
  NegativeZ order. Every slice has the same format and complete compatible mip
  chain.
- Mip records use explicit width, height, row pitch, offset, and stored byte
  count. Readers validate pixel-format layout, mip progression, slice
  consistency, ranges, allocation limits, checksum, and trailing data before
  publication.
- BC platform bytes receive no additional asset-payload compression. The
  initial DBLK container may copy them directly and preserves future
  independently addressable mip records.
- Texture2D source identity is the exact source-file content hash. Its key also
  includes usage, sRGB, maximum resolution, compression quality, alpha-mip mode
  and threshold, texture builder/schema version, and target platform/profile.
- Six-face TextureCube source identity is an ordered tuple of six exact source
  content hashes. Panorama source identity is the panorama content hash plus
  face dimension and canonical exposure bits. Both layouts include source
  layout, cube projection version, texture builder/schema version, sRGB, and
  target platform/profile in the key.
- A projection algorithm or cube orientation change increments the projection
  version even when the texture payload schema remains readable.

## Frozen Cross-Asset Contracts

All integers and IEEE-754 values in DBLK, TXPL, keys, and the cook manifest use
little-endian byte order. Hashes name the exact byte range stated by the
contract and use XXH3. Writers emit zero for reserved fields and padding;
readers reject nonzero reserved fields or padding. Addition, multiplication,
alignment, and narrowing are checked before allocation or I/O.

### Target and compression identifiers

The shared identifiers are stable serialized values:

| Type | Value | Meaning |
| --- | ---: | --- |
| target platform | `0` | Invalid or unknown; never emitted |
| target platform | `1` | Win64 |
| target profile | `0` | Invalid or unknown; never emitted |
| target profile | `1` | Game runtime feature profile |
| target profile | `2` | Editor validation profile; not deployable as a game cook |
| compression | `0` | None |
| compression | `1` | Zstandard |

DBLK version 1 writers emit `None`. A version 1 reader validates the structural
rules for `Zstandard`, including a maximum uncompressed-to-stored ratio of
`64:1`, but fails with `UnsupportedCompression` unless its build explicitly
provides the shared Zstandard decoder. Texture payload descriptors always use
`None`; BC bytes receive no second compression layer.

### DBLK version 1

DBLK version 1 begins with this 64-byte header:

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | `uint32` | magic `0x4B4C4244` (`DBLK` in file order) |
| 4 | `uint32` | container version, `1` |
| 8 | `uint32` | target platform |
| 12 | `uint32` | target profile |
| 16 | `uint32` | flags, zero in version 1 |
| 20 | `uint32` | header size, exactly `64` |
| 24 | `uint32` | payload count, `1..64` |
| 28 | `uint32` | table-entry size, exactly `80` |
| 32 | `uint64` | payload-table offset, exactly `64` |
| 40 | `uint64` | complete stored file size |
| 48 | `uint64` | XXH3-64 of the complete 80-byte-entry table |
| 56 | `uint64` | reserved zero |

Each 80-byte table entry is:

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | four `uint32` | `PayloadId` in `FGuid` A/B/C/D order |
| 16 | `uint32` | flags; bit 0 is `Required`, all other bits reserved |
| 20 | `uint32` | asset-payload schema version |
| 24 | `uint32` | target platform |
| 28 | `uint32` | target profile |
| 32 | `uint32` | compression method |
| 36 | `uint32` | required alignment |
| 40 | `uint64` | absolute stored-data offset |
| 48 | `uint64` | stored byte count |
| 56 | `uint64` | uncompressed byte count |
| 64 | two `uint64` | XXH3-128 of uncompressed bytes, low then high |

Entries are strictly sorted by the unsigned A/B/C/D `PayloadId` tuple and IDs
are unique. Alignment is a power of two in `[16, 4096]`. The first payload
begins at or after the 16-byte-aligned end of the table; every payload meets
its declared alignment. Ranges are ordered, nonoverlapping, and contained by
the stored file size. Gaps and trailing alignment padding are zero. A
`None` entry has equal stored and uncompressed sizes. Empty payloads are
invalid.

The maximum stored or uncompressed size of one payload is 8 GiB. The maximum
complete container size is 64 GiB. Readers validate the table and every stored
range before allocating or exposing any entry. They verify an entry's
uncompressed hash after decompression and before publication. The table hash
does not replace per-payload hashes.

AssetCore does not assign semantics to an unreferenced payload ID. It validates
all entries structurally, exposes a descriptor-selected entry by exact ID, and
may ignore an otherwise valid unreferenced entry. An asset consumer fails on a
missing descriptor-selected required payload. Unknown DBLK flags, location
kinds, compression values, or descriptor mismatches fail closed.

### Logical cooked-payload descriptor

`FCookedPayloadDescriptor` is a reflected value in a cooked `.dasset`. Its
fields and serialized widths are:

| Field | Representation | Contract |
| --- | --- | --- |
| `PayloadId` | `FGuid` | Nonzero and unique within the package |
| `LocationKind` | `uint32` enum | `0` invalid, `1` package companion |
| `Offset` | `uint64` | Absolute DBLK offset and exact table-entry match |
| `StoredSize` | `uint64` | Exact table-entry match |
| `UncompressedSize` | `uint64` | Exact table-entry match |
| `Alignment` | `uint32` | Exact table-entry match |
| `PayloadHashLow/High` | two `uint64` | XXH3-128 of uncompressed bytes |
| `PayloadSchemaVersion` | `uint32` | Asset-specific schema |
| `TargetPlatform` | `uint32` | Shared identifier above |
| `TargetProfile` | `uint32` | Shared identifier above |
| `CompressionMethod` | `uint32` | Shared identifier above |

All fields except the physical location kind are duplicated deliberately from
the DBLK table. Resolution accepts the payload only when every duplicated field
matches. The descriptor stores no filename, relative path, DDC key, or DDC
object path. `PackageCompanion` replaces the cooked package's final `.dasset`
suffix with `.dbulk`; a package name without that exact suffix is invalid.
Future archive kinds may reinterpret physical location but do not change the
descriptor-selected `PayloadId` or asset references.

Asset-specific stable payload IDs are:

| Asset payload | `PayloadId` |
| --- | --- |
| StaticMesh primary DMSH | `6d9f79b5-7b68-4d91-a42c-2a6063fcab16` |
| Texture2D primary TXPL | `53aa6a89-dc49-401a-b409-adc498ac4f8b` |
| TextureCube primary TXPL | `d52878ce-8f50-48c7-a3c7-ff846e2c4c5a` |

### Cook mapping, publication, and manifest

A cook context receives an explicit authored mount table, target platform,
target profile, and cook root. It maps `/Engine/A/B` to
`<CookRoot>/Engine/A/B.dasset` and `/Game/A/B` to
`<CookRoot>/Game/A/B.dasset`; companion bulk replaces the suffix. The virtual
path must already satisfy package normalization and the physical result must
remain a lexical descendant of the cook root. Absolute paths, empty
components, `.` or `..`, alternate separators in a component, device names,
and DDC paths are rejected rather than sanitized.

For a transaction, payloads are sorted by ID within each package and packages
are sorted by normalized mount-relative UTF-8 path bytes. Publication order is:

1. write, flush, close, reopen, and fully validate every temporary DBLK;
2. atomically publish all DBLK companions in package order;
3. write and validate temporary cooked packages, then atomically publish them
   in package order;
4. write and validate the complete temporary manifest and atomically publish
   it last;
5. after manifest success, remove only stale files named by the previous valid
   manifest and absent from the new one, after resolving each beneath the exact
   cook root.

Temporary names are fixed-length random siblings independent of destination
length. Any failure removes known temporary files. A DBLK or package already
published before a later failure is left as harmless unreferenced output; the
previous manifest remains the consistency boundary and is never edited in
place. Cleanup never enumerates arbitrary files for deletion and never removes
an output not owned by the previous manifest.

The manifest is `CookManifest.bin`. Its 48-byte header is:

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | `uint32` | magic `0x464E4D43` (`CMNF` in file order) |
| 4 | `uint32` | manifest version, `1` |
| 8 | `uint32` | target platform |
| 12 | `uint32` | target profile |
| 16 | `uint32` | entry count |
| 20 | `uint32` | header size, exactly `48` |
| 24 | `uint64` | total record byte count |
| 32 | `uint64` | XXH3-64 of all record bytes |
| 40 | `uint64` | complete manifest file size |

Records are sorted by raw normalized relative-path UTF-8 bytes and duplicate
paths are invalid. Each record is `{uint8 kind, uint8 flags, uint16 reserved,
uint32 path byte count, uint64 file byte count, uint64 hash low, uint64 hash
high}`, followed immediately by path bytes without a terminator or padding.
Kinds are `1` cooked package and `2` DBLK; flag bit 0 is `Required`. Version 1
requires every record to be required. Hash is XXH3-128 of the complete
published file. Paths use `/`, are relative to the cook root, and are limited
to 1,024 UTF-8 bytes. Entry count is at most 1,000,000 and total record bytes
are at most 256 MiB. The manifest lists every published cooked package and
DBLK, but never lists itself, a source file, DDC object, or temporary file.

### Explicit package mode

`EPackageLoadMode` has stable values `0 AuthoredEditor` and `1 CookedRuntime`.
The process host selects one mode explicitly while initializing AssetCore and
provides the authored mount table or cooked root appropriate to it. Every
package load receives the immutable process mode through its load context.
Tests may construct an isolated load context of either mode but may not switch
the global manager after its first package is loaded.

`AuthoredEditor` resolves mounted authored packages, permits DDC and source
rebuild policy in asset-specific editor code, and does not consume a neighboring
DBLK unless a cook-validation test explicitly supplies a cooked context.
`CookedRuntime` resolves packages only beneath the selected cook root, requires
cooked descriptors for required native data, and forbids source and DDC
fallback. Mode is never inferred from executable name, build configuration,
package fields, source availability, DDC contents, or the existence of a
neighboring `.dbulk`.

## Frozen Texture Contracts

### TXDD compatibility decision

The existing private TXDD schema version 1 is documented exactly by
`Texture2D.cpp`: a 16-byte Core cache header
`{TXDD magic, schema 1, builder 1, 0x01020304}`, followed by payload XXH3-64,
payload byte count, then `{uint8 EPixelFormat, uint32 mip count}` and, for each
mip, `{uint32 width, uint32 height, uint32 row pitch, uint64 byte count, bytes}`.
Its key hashes length-prefixed strings for `"DurinTexture2D"`, `"Win64"`, and
the lowercase source hash plus the current settings fields.

TXDD is deliberately replaced rather than promoted. It lacks dimension,
slices, target profile, stable pixel-format IDs, explicit offsets, and
builder/schema separation suitable for cooked data. Stage 3 treats every TXDD
object as a disposable legacy miss, writes only TXPL objects through
`FDerivedDataObjectStore`, and may remove TXDD only through bounded texture-DDC
cleanup. No cooked package may contain or reference TXDD. A warm legacy object
does not justify retaining a runtime TXDD reader because its source package
remains authoritative during migration.

### TXPL version 1

TXPL is the shared native Texture2D and TextureCube platform payload. Its
80-byte header is:

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | `uint32` | magic `0x4C505854` (`TXPL` in file order) |
| 4 | `uint32` | payload schema version, `1` |
| 8 | `uint32` | asset-specific builder version |
| 12 | `uint32` | target platform |
| 16 | `uint32` | target profile |
| 20 | `uint32` | dimension: `1` Texture2D, `2` TextureCube |
| 24 | `uint32` | stable texture pixel-format identifier |
| 28 | `uint32` | array-slice count |
| 32 | `uint32` | mip count per slice |
| 36 | `uint32` | header size, exactly `80` |
| 40 | `uint32` | subresource-record count |
| 44 | `uint32` | subresource-record size, exactly `40` |
| 48 | `uint64` | record-table offset, exactly `80` |
| 56 | `uint64` | complete stored object size |
| 64 | `uint64` | XXH3-64 of stored bytes `[80, stored size)` |
| 72 | `uint64` | reserved zero |

Stable pixel-format values are `1 BC1_UNORM`, `2 BC1_UNORM_SRGB`,
`3 BC3_UNORM`, `4 BC3_UNORM_SRGB`, `5 BC5_UNORM`, `6 BC7_UNORM`, and
`7 BC7_UNORM_SRGB`. They are translated explicitly to RHI `EPixelFormat`; the
RHI enum's native numeric values are not serialized.

Each 40-byte record is `{uint32 slice, uint32 mip, uint32 width, uint32 height,
uint32 row pitch, uint32 reserved, uint64 offset, uint64 stored byte count}`.
Records are slice-major and mip-minor with no omissions or duplicates. Texture2D
has one slice; TextureCube has six in PositiveX, NegativeX, PositiveY,
NegativeY, PositiveZ, NegativeZ order. Cube faces are square and every slice
has identical format, dimensions, row pitches, stored counts, and a complete
compatible mip chain.

Subresource data begins at the next 16-byte boundary after the record table.
Every data offset is 16-byte aligned; ranges are ordered, nonoverlapping, and
contained by stored size. Gaps and trailing alignment padding are zero.
`row pitch = max(1, ceil(width / 4)) * bytes per BC block`; stored bytes equal
row pitch times `max(1, ceil(height / 4))`. BC1 uses 8-byte blocks; BC3, BC5,
and BC7 use 16-byte blocks. Mip zero dimensions are nonzero and each following
dimension is `max(1, previous / 2)` until the final required `1x1` mip.

Maximum base dimension is 16,384 for Texture2D and 4,096 for TextureCube.
Maximum mip count is 32, record count is at most 192, and complete stored data
is at most 2 GiB. Readers reject unsupported magic, schema, builder policy,
platform/profile, dimension, pixel format, counts, arithmetic, layout,
checksum, ranges, padding, or trailing data before publishing detached platform
data. Asset-specific build policy decides whether a readable builder version
is acceptable; disk layout is controlled solely by schema version.

Texture2D builder version is `2`: version 1 names the legacy TXDD builder and
version 2 starts the shared TXPL/key contract even when resulting BC bytes are
unchanged. TextureCube builder version is `1`. Cube projection version is `1`.
TXPL schema version is `1` for both.

### Canonical texture keys

All source hashes below are the XXH3-128 of exact source bytes and are encoded
as `uint64 HashLow` then `uint64 HashHigh`, never as text. The final object key
is lowercase `XXH3-128(canonical key bytes)` rendered as 32 hex characters.
Paths, timestamps, diagnostic dimensions, enum storage sizes, padding, and
formatted strings are excluded.

Texture2D key schema 1 emits:

1. `uint32` key schema `1`, then asset kind `1`;
2. source hash low and high;
3. usage, sRGB, compression quality, and alpha-mip mode as four `uint8`;
4. `uint32` maximum resolution;
5. alpha-coverage threshold as canonical finite `float32` bits;
6. `uint32` builder version, TXPL schema, target platform, and target profile.

Six-face TextureCube key schema 1 emits:

1. `uint32` key schema `1`, asset kind `2`, and source layout `0`;
2. six source hashes, low then high for each, in the frozen face order;
3. sRGB as `uint8`;
4. `uint32` cube builder version, TXPL schema, projection version, target
   platform, and target profile.

Panorama TextureCube key schema 1 emits:

1. `uint32` key schema `1`, asset kind `2`, and source layout `1`;
2. panorama source hash low then high;
3. `uint32` requested face dimension, where zero retains the documented
   source-derived default;
4. exposure EV as canonical finite `float32` bits and sRGB as `uint8`;
5. `uint32` cube builder version, TXPL schema, projection version, target
   platform, and target profile.

NaN, infinity, negative zero, and values outside the authored validation range
are rejected before key construction. Exposure and alpha-threshold validation
therefore makes their ordinary IEEE bit representation canonical.
Texture2D objects use
`DerivedDataCache/Texture2D/Objects/<prefix>/<key>.bin`; TextureCube uses
`DerivedDataCache/TextureCube/Objects/<prefix>/<key>.bin`.

### Texture source provenance and migration

`FTextureSourceFile` is `{SourcePath UTF-8 string, SourceContentHash two
uint64}`. The path is forward-slash normalized, relative to project or engine
root, and rooted beneath `SourceAssets/Textures`; the hash is XXH3-128 of exact
bytes. An empty path requires a zero hash and means no source dependency.
Absolute paths, backslashes, `.`/`..`, and paths outside the exact source root
are invalid persistent metadata.

`FTexture2DSourceImportData` contains one nonempty `FTextureSourceFile`,
`DecoderId` UTF-8 string, and `uint32 DecoderVersion`. The first identity is
`DurinImage` version `1`. `FTextureCubeSourceImportData` contains source layout,
six `FTextureSourceFile` values in frozen face order, or one panorama source,
plus `DecoderId`, `DecoderVersion`, and `uint32 ProjectionVersion`. Inactive
layout fields must be empty. Face dimension, exposure, and sRGB remain ordinary
authored build settings rather than duplicated provenance.

Each provenance value is optional as a whole and may be stripped from a cooked
package. New project imports copy beneath project
`SourceAssets/Textures`; engine imports copy beneath
`Engine/SourceAssets/Textures`. Name collisions use the importer transaction's
deterministic disambiguation policy and persist the resulting normalized path.
Moving a `.dasset` alone does not move shared source art. Explicit reimport or
source relocation updates provenance only after source copy, hash, decode,
build, and package mutation can commit transactionally.

Legacy Texture2D package-adjacent `SourceFile` and TextureCube face/panorama
filenames remain readable during Stages 3 through 5 and are never written by a
new import after the owning migration lands. Each asset class removes its
legacy resolver independently in Stage 6 only after all repository packages
carry normalized provenance, an asset-registry scan is clean, editor repair and
reimport use the new value, and compatibility tests deliberately reject the old
form. Legacy paths and file metadata never enter the new canonical keys.

### Module and target boundary

The selected end-state modules are:

| Owner | Responsibilities | Forbidden dependencies |
| --- | --- | --- |
| runtime `AssetCore` | `.dasset`, DDC object store, DBLK, descriptors, cook-root containment, manifest codec, package load context | Assimp, stb/source decoders, projection, BC encoders |
| runtime `Engine` | DMSH and TXPL validation/decode, runtime mesh/texture data, transactional render-resource publication | Assimp, source decoders, projection, BC encoders |
| editor `AssetImport` | Assimp adapter, encoded-image/HDR decoders, source copy and provenance repair | RHI resource construction |
| editor `EngineAssetBuild` | mesh import conversion, Texture2D/cube mip generation, panorama projection, BC compression, DDC/cook adapters | runtime package-mode selection |

`AssetImport` and `EngineAssetBuild` are enabled only by editor/cooker/test
profiles that request authored-source work. The runtime `DurinGame` dependency
closure contains neither module and does not deploy Assimp. `bc7enc_rdo`,
`rgbcx`, and equivalent offline encoders link only to `EngineAssetBuild`.
Asset-specific DMSH/TXPL codecs remain in runtime `Engine` because editor DDC,
cooker validation, and cooked runtime loading share them. DBLK and manifest
code remain asset-agnostic in runtime `AssetCore`.

The current placement of Assimp and image decode in runtime AssetCore and
texture build/projection/BC encoding in runtime Engine is migration debt, not
an exception to this boundary. Stages 2, 4, and 5 move those implementations;
Stage 6 adds a dependency-closure test that fails if the forbidden libraries or
modules reappear.

## Inherited Frozen StaticMesh Contracts

The following contracts were frozen and implemented by the predecessor plan.
They remain authoritative until moved into owning runtime documentation during
the final handoff stage. Changing one requires the corresponding schema,
builder, or key version rather than silently reinterpreting existing DDC data.

All integer and IEEE-754 floating-point fields below use little-endian byte
order. Booleans and enums use their stated unsigned integer width. Writers emit
zero for reserved fields and readers reject non-zero reserved fields. Byte
offsets are from the start of the DMSH object.

### Source import data

`FStaticMeshSourceImportData` is an optional reflected value with these fields:

| Field | Representation | Contract |
| --- | --- | --- |
| `SourcePath` | UTF-8 string | Empty means no source dependency. Otherwise forward-slash normalized, relative to project or engine root, and rooted beneath `SourceAssets/Models`. Absolute paths and `..` are invalid. |
| `SourceContentHash` | 32 lowercase hex characters | XXH3-128 of the exact source bytes. Empty is accepted only while upgrading legacy package metadata. |
| `ImporterId` | UTF-8 string | Stable case-sensitive implementation identity. The first importer uses `Assimp`. |
| `ImporterVersion` | `uint32` | Changes whenever importer behavior can change the build result independently of settings. |
| `ImportSettings` | three `uint8` enum values | Forward, right, and up axes, in that order, using `EStaticMeshImportAxis` values. |

The value is absent when `SourcePath` is empty. Source metadata is editor
provenance and may be stripped from cooked packages.

### DMSH envelope and chunk table

Schema version 2 uses a 64-byte header. It differs from version 1 by allowing
an LOD to declare zero UV channels; readers continue to reject unsupported
schema versions.

| Offset | Type | Field |
| ---: | --- | --- |
| 0 | `uint32` | magic `0x48534D44` (`DMSH` in file order) |
| 4 | `uint32` | payload schema version, currently `2` |
| 8 | `uint32` | mesh-builder version, currently `1` |
| 12 | `uint32` | target platform (`0` invalid/unknown, `1` Win64) |
| 16 | `uint32` | payload flags; bit 0 means at least one chunk is compressed |
| 20 | `uint32` | header size, exactly `64` |
| 24 | `uint32` | chunk count, from `6` through `64` |
| 28 | `uint32` | reserved zero |
| 32 | `uint64` | chunk-table offset, exactly `64` |
| 40 | `uint64` | sum of uncompressed chunk byte counts |
| 48 | `uint64` | complete stored object size, including header, table, padding, and chunks |
| 56 | `uint64` | XXH3-64 of stored bytes `[64, stored size)` |

Each 32-byte chunk-table entry is `{uint32 type, uint32 flags, uint64 offset,
uint64 stored size, uint64 uncompressed size}`. Bit 0 of flags is `Required`;
bits 8 through 15 encode compression (`0` none, `1` Zstandard), and all other
bits are reserved. Schema 2 writers use no compression. Readers reject an
unsupported compression method, a compressed chunk ratio above `64:1`, or a
payload whose declared total uncompressed size exceeds 8 GiB.

The table immediately follows the header. Chunk data starts after the table,
and every chunk offset is 16-byte aligned. Padding bytes are zero. Ranges must
be ordered, non-overlapping, contained by the stored size, and arithmetically
representable. Required chunks are `Bounds(1)`, `MaterialSlots(2)`, `LODs(3)`,
`Sections(4)`, `VertexStreams(5)`, and `IndexBuffers(6)`, exactly once each.
Unknown required chunks fail; unknown optional chunks may be skipped after
their range and checksum are validated.

The required chunk payloads are:

- `Bounds`: six finite `float32` values, minimum XYZ then maximum XYZ.
- `MaterialSlots`: `uint32 count`, followed by `count` GUIDs encoded as four
  `uint32` values in `FGuid` A/B/C/D order. The limit is 4,096.
- `LODs`: `uint32 count`, then one 40-byte record per LOD:
  `{uint32 vertex count, uint32 index count, uint32 section count, uint8 UV
  count, uint8 flags, uint16 reserved, float32 bounds[6]}`. Flags bit 0 means
  vertex colors are present. UV count is from zero through four; zero stores no
  UV arrays and runtime render data supplies zero-filled packed coordinates.
  Limits are eight LODs, 100,000,000 vertices, 300,000,000 indices, and 65,536
  sections per LOD.
- `Sections`: for each LOD, `uint32 count`, then 44-byte records containing five
  `uint32` values (`first index`, `index count`, `minimum vertex`, `maximum
  vertex`, `material slot`) followed by six finite bounds `float32` values.
- `VertexStreams`: for each LOD, tightly packed structure-of-arrays in this
  order: positions (`3 x float32`), normals (`3 x float32`), tangents (`4 x
  float32`), each UV channel (`2 x float32`), then colors when present (`4 x
  float32`). Every array has the LOD vertex count; no native padding is stored.
- `IndexBuffers`: for each LOD, exactly the declared number of `uint32` indices.

All bounds require minimum components not greater than maximum components.
Positions, normals, tangents, UVs, colors, and bounds reject NaN and Infinity.
Every index is less than the LOD vertex count. Every section is non-empty,
contained by the index buffer, references valid vertices and a valid material
slot, and section index ranges cover the LOD index buffer exactly without
overlap. The mesh and every LOD contain non-empty geometry.

Schema changes whenever a field, enum value, validation meaning, or byte layout
changes. Readers fail closed on unsupported schemas. A builder-version change
changes build semantics and the DDC key but does not change how an otherwise
supported schema is decoded.

### Derived-data key encoding

`BuildStaticMeshDerivedDataKeyBytes` emits exactly:

1. `uint32` key-schema version (`1`);
2. source XXH3-128 as `uint64 HashLow`, then `uint64 HashHigh`;
3. importer identity as `uint64 byte count` followed by unmodified UTF-8 bytes;
4. `uint32` importer version;
5. forward, right, and up axes as three `uint8` values;
6. `uint32` mesh-builder version;
7. `uint32` payload-schema version;
8. `uint32` target-platform identifier.

The object key is lowercase `XXH3-128(canonical bytes)` rendered as 32 hex
characters. The source path, timestamps, diagnostics, host enum sizes, padding,
and formatted strings are never key inputs. Objects use
`DerivedDataCache/StaticMesh/Objects/<first-two-key-characters>/<key>.bin`.

### Fixtures and migration window

The canonical logical fixtures live at
`Engine/Tests/Native/EngineTests/Data/StaticMeshDerivedData/README.md`. They
freeze a one-section mesh and a multi-material mesh with four UV channels and
vertex colors, plus the malformed payload derivations required by the Stage 2
reader suite. Tests construct payload bytes from the listed values rather than
checking in compiler- or platform-produced structure memory.

Legacy `SourceFile` resolution remains enabled through Stages 1–5. It accepts
only the existing package-relative and mounted-content forms, reports that the
asset needs migration, and never writes those forms for a newly imported asset.
Removal requires all repository-owned static-mesh packages to carry normalized
`FStaticMeshSourceImportData`, a clean asset-registry scan with no legacy
diagnostics, and explicit compatibility coverage that verifies the old form is
rejected. Removal is a Stage 6 change and must not be inferred from elapsed
time or an engine release number.

### Asset, source, and payload identity

- `DStaticMesh` remains the referenced asset. Levels, components, materials, and
  editor tools never reference OBJ files, DDC object paths, or cooked bulk files.
- OBJ, FBX, and glTF remain authoritative authoring inputs. They are not Durin
  runtime formats.
- Source import metadata is optional. A mesh with a valid native payload can
  load without a source model; a procedural or future generated mesh is not
  required to invent a source path.
- Editor assets retain source provenance for reimport. Cooked runtime packages
  may strip that metadata and never require the source file.
- Source paths are normalized project- or engine-relative paths. Absolute
  workstation paths are rejected as persistent metadata.

### Source directory policy

- New project source models live under project-root `SourceAssets/Models/`.
  Engine-owned source models live under `Engine/SourceAssets/Models/`.
- `SourceAssets` is versioned authoring input but is not a runtime content mount
  and is excluded from cooked output.
- `.dasset` packages remain under mounted `Content`.
- Existing colocated Content sources continue to resolve during migration.
  Migration must not make an existing checkout unloadable before its package
  metadata is updated.

### Native payload format

- The native payload is an internal, chunked binary schema, not a public
  `.dmesh` asset type.
- DDC objects use the `.bin` suffix and begin with a `DMSH` magic value.
- The header records schema version, mesh-builder version, target platform,
  payload flags, uncompressed and stored sizes, and a content checksum.
- Payload chunks describe bounds, material-slot/section mapping, LOD metadata,
  vertex streams, and index buffers. Every count, offset, alignment, and byte
  range is validated before allocation or publication.
- Material slot GUIDs and editable default-material references remain
  `DStaticMesh` asset metadata. The payload stores only the stable identifiers
  and section mapping needed by rendering.
- `FStaticMeshRenderData` remains the runtime representation. Encoding and
  decoding use an explicit disk schema rather than serializing C++ object
  memory, STL layouts, padding, or RHI handles.
- Unknown required chunks or unsupported schema versions fail closed. Unknown
  optional chunks may be skipped only when the header marks them optional.
- The first schema preserves current rendering behavior. Future vertex
  compression or platform-specific layouts require a builder-version or schema
  change and must not silently reinterpret old payloads.

### Derived-data key and storage

- The derived-data key includes source content hash, normalized import settings,
  mesh-builder version, payload schema version, and target platform.
- File timestamps and absolute paths are not part of the content identity.
- Objects are stored at
  `DerivedDataCache/StaticMesh/Objects/<prefix>/<key>.bin`.
- Writes use a same-directory temporary file, flush and close it, then
  atomically replace the final object. Readers never consume a partial write.
- Cache files are rebuildable, ignored output. Missing, stale, truncated,
  corrupt, or unsupported objects are cache misses in the editor.
- Cache cleanup must resolve and verify every deletion target beneath the exact
  static-mesh DDC root.

### Load and failure policy

- Editor load order is:
  1. use an explicitly supplied cooked/native payload;
  2. use a valid DDC object for the computed key;
  3. rebuild from accessible source import data and populate DDC;
  4. fail with an actionable diagnostic.
- A corrupt DDC object is quarantined or safely ignored before a source rebuild;
  it never partially updates the live mesh.
- A valid DDC object permits editor loading when the source file is temporarily
  unavailable, but the editor reports that reimport and cache regeneration are
  unavailable.
- Cooked runtime load order contains no source fallback. Missing, corrupt, or
  incompatible cooked mesh data is a hard asset-load failure.
- Payload decode and render-data replacement are transactional. Bound components
  observe either the previous complete render data or the new complete render
  data, never a partially decoded mesh.

### Cooking and packaging

- The initial cooker writes the same validated native payload used by DDC into
  package-relative cooked bulk data. The first implementation may use a
  `.dbulk` companion beside the cooked `.dasset`; a later archive system may
  absorb that bulk data without changing mesh references or the payload schema.
- Cooked package metadata identifies the bulk object, expected payload hash,
  schema version, and target platform.
- Cooking is deterministic for identical source bytes, settings, builder
  version, and target platform.
- Cook output excludes source OBJ/FBX/glTF files and does not require Assimp in
  a runtime-only build.

### Ownership and thread model

- `Engine` owns static-mesh build settings, key contribution, payload schema,
  codec, and conversion to/from `FStaticMeshRenderData`.
- `AssetCore` owns reusable atomic DDC object-store and cooked bulk-location
  mechanics. It does not interpret mesh payload contents.
- The editor owns import/reimport UI and source-path repair.
- The first implementation performs source import and payload decode
  synchronously on the game thread, matching current `PostLoad` behavior.
  Asynchronous build and load are deferred until the synchronous state machine
  and failure behavior are proven.
- RHI resources and backend-native handles are never serialized. Existing
  render-thread publication and destruction rules remain authoritative.

### Material preview

- Preview sphere and box become engine-owned static-mesh assets with stable
  virtual identities under `/Engine/Editor/MaterialPreview/`.
- Their authoring models live under engine `SourceAssets`; cooked editor/runtime
  data lives through the same native payload path as project meshes.
- All material-preview instances share the loaded mesh assets or immutable
  render data. A preview document does not import or root its own copy.

## Current Foundations and Gaps

### Foundations

- `.dasset` packages provide stable virtual asset identities and serialize
  reflected metadata and cross-asset references.
- Project-local `DerivedDataCache` exists, is ignored, is outside content
  mounts, and has shared atomic byte publication.
- `FDerivedDataObjectStore` provides content-addressed read, atomic write,
  bounded cleanup, path containment, and disk-budget accounting.
- StaticMesh has optional relocatable source provenance, canonical keys, a
  deterministic strict DMSH codec, safe cache-miss recovery, transactional
  render-data replacement, and source-unavailable cache hits.
- Texture2D already builds complete desktop BC mip chains and stores versioned,
  checksummed TXDD objects under the project DDC.
- TextureCube already has validated six-face and equirectangular source paths,
  deterministic panorama projection, complete compatible per-face mip chains,
  and a revisioned render-resource path.
- The asset-data lifecycle documentation selects `.bin` versus `.dbulk`,
  logical descriptor fields, loose companion naming, publication order, and the
  future archive boundary.
- The asset registry and package loader already distinguish virtual assets from
  physical files.

### Gaps

- AssetCore has no DBLK codec, logical cooked-payload descriptor implementation,
  cook context, output manifest, or cooked-runtime loader.
- No asset class writes or consumes cooked bulk data.
- Texture2D's key, codec, path, and cache I/O are private helpers in
  `Texture2D.cpp`; they do not use `FDerivedDataObjectStore`, lack the mesh
  cache's diagnostics and cleanup policy, and require the source file before
  accepting a cache hit.
- Texture2D source provenance remains a required package-adjacent filename
  rather than an optional normalized `SourceAssets` reference.
- TextureCube has no persisted source-content hashes, canonical key, native
  platform-payload codec, DDC object, or source-unavailable load path.
- Runtime target dependency graphs still inherit Assimp through AssetCore and
  have no enforced boundary excluding source-image decoders, panorama
  projection, or offline BC compression.
- Material previews import `Sphere.obj` and `Box.obj` into per-document transient
  meshes and manually root them.

## Implementation Stages

### Stage 0: Reconcile Cross-Asset Contracts and Fixtures

- [x] Freeze the DBLK header, payload-table byte encoding, alignment, checksum,
  allocation limits, platform field, compression enum, and unknown-entry policy.
- [x] Freeze the logical cooked-payload descriptor serialization in `.dasset`,
  including stable `PayloadId`, location kind, sizes, hash, schema, target
  platform/profile, alignment, and compression.
- [x] Freeze deterministic cook-root mapping, package publication order,
  manifest encoding, stale-output behavior, and failure cleanup.
- [x] Decide and document how authored versus cooked package mode is selected
  without inferring mode from the existence of a neighboring file.
- [x] Extract the implemented Texture2D TXDD contract into a standalone texture
  payload design and decide its deliberate compatibility or replacement path.
- [x] Freeze the shared Texture2D/TextureCube payload schema, canonical key
  encodings, builder versions, projection version, limits, and exact face order.
- [x] Define optional Texture2D and TextureCube source-provenance values,
  `SourceAssets/Textures` placement, legacy package-adjacent compatibility, and
  removal criteria.
- [x] Freeze the module and target dependency boundary for Assimp, image
  decoders, panorama projection, BC encoders, asset-specific codecs, and runtime
  render-data construction.
- [x] Add deterministic logical fixtures for DBLK, Texture2D, six-face
  TextureCube, panorama-derived TextureCube, and malformed variants without
  invoking an importer, compressor, RHI, or window.

#### Acceptance Gate

- Every shared container, descriptor, texture payload, key, provenance,
  publication, mode-selection, and dependency field has one explicit
  representation, and fixtures can exercise all readers independently of
  editor-only source tooling.

### Stage 1: Implement Shared Cooked-Bulk and Cook Publication

Dependencies: Stage 0.

- [x] Implement AssetCore DBLK encode/decode with bounded structural validation,
  platform validation, per-payload hashes, and unknown-entry handling.
- [x] Implement reflected or package-native logical payload descriptors without
  persisting physical companion paths.
- [x] Implement package-relative companion resolution beneath the selected cook
  root and reject traversal, DDC locations, authored source paths, and
  mismatched mounts.
- [x] Add a cook context that accumulates asset payloads, writes and validates a
  temporary DBLK, publishes bulk before package metadata, and removes incomplete
  output on failure.
- [x] Add a deterministic manifest containing every cooked package and required
  bulk companion, with manifest-driven stale-output cleanup.
- [x] Add an explicit cooked/runtime package-load mode that rejects source and
  DDC fallback.
- [x] Add AssetCore unit and integration tests for multi-payload containers,
  deterministic bytes, relocation, path containment, missing files, wrong
  platform, corrupt tables and hashes, overlaps, truncation, excessive sizes,
  publication interruption, and cleanup.

#### Acceptance Gate

- AssetCore can deterministically publish, relocate, resolve, and validate a
  cooked package plus DBLK consistency unit without knowing the payload's asset
  type, and interrupted or malicious input cannot publish a referencing package
  or escape the cook root.

### Stage 2: Make StaticMesh the First Cooked Consumer

Dependencies: Stage 1 and the completed predecessor StaticMesh Stages 0–3.

- [x] Add a StaticMesh cook adapter that obtains or rebuilds a matching DMSH
  payload and contributes it to the package's DBLK.
- [x] Serialize only runtime mesh metadata and its logical payload descriptor;
  strip source provenance and legacy source fields unless an explicit
  diagnostic cook policy retains non-runtime metadata.
- [x] Add cooked `DStaticMesh` loading that accepts only a matching descriptor,
  DBLK entry, target platform, DMSH schema, checksum, and material-slot mapping.
- [x] Ensure cooked decode and render-data publication remain transactional and
  never interpret a DDC path as a runtime reference.
- [x] Move Assimp and source-model import out of the runtime dependency graph
  needed by the cooked StaticMesh path.
- [x] Add clean-cook reproducibility, missing-source isolation, missing bulk,
  wrong platform/schema, corrupt payload, material-slot, and render smoke tests.

#### Acceptance Gate

- Two clean StaticMesh cooks are byte-identical, the cooked asset loads and
  renders after source models and Assimp are removed, and every invalid required
  payload fails with an asset-qualified diagnostic before publication.

### Stage 3: Converge Texture2D on the Shared Derived-Data Model

Dependencies: Stage 0; may proceed alongside Stage 2 after shared contracts are
frozen, but has one source/build writer.

- [x] Add optional normalized Texture2D source provenance under project or
  engine `SourceAssets/Textures`, retaining explicit legacy package-adjacent
  loading during migration.
- [x] Extract the texture key builder and platform-payload codec from
  `Texture2D.cpp` into Engine-owned testable units with explicit schema,
  builder, platform/profile, checksum, and allocation limits.
- [x] Move Texture2D cache I/O to `FDerivedDataObjectStore` and adopt the shared
  hit, miss, corrupt, incompatible, write-failure, key, budget, and cleanup
  diagnostics.
- [x] Change editor load order so a valid object can load from persisted source
  identity while the source image is unavailable; require source only for a
  rebuild or edit that needs decoded pixels.
- [x] Preserve valid live platform and render-resource data across decode,
  rebuild, cache-read, or cache-write failure.
- [x] Migrate import, move, delete, reimport, Texture Editor, thumbnails, and
  focused tests to the new provenance without breaking legacy packages.

#### Acceptance Gate

- Cold Texture2D load builds once, warm load performs no image decode, valid DDC
  loads with source removed, semantic input changes miss deterministically,
  corrupt data rebuilds safely when source exists, and source/provenance
  operations cannot escape `SourceAssets`.

### Stage 4: Cook Texture2D

Dependencies: Stages 1 and 3.

- [x] Add a Texture2D cook adapter that contributes the validated platform
  payload to DBLK without decoding or recompressing on a valid matching DDC hit.
- [x] Serialize runtime texture metadata and a logical descriptor while
  stripping source provenance, fingerprints, DDC keys, editor diagnostics, and
  decoded source data.
- [x] Add cooked Texture2D loading that validates descriptor, DBLK, payload
  schema, platform/profile, pixel-format support, mip layout, and checksum
  before render-resource publication.
- [x] Remove source-image decoders and offline BC encoders from the runtime
  dependency graph required by cooked Texture2D.
- [x] Add deterministic cook, all supported BC formats, NPOT/tail mips,
  missing-source isolation, corrupt payload, wrong platform/profile, and Vulkan
  upload/sample tests.

#### Acceptance Gate

- Cooked Texture2D assets load and sample every supported platform format with
  all source images, decoders, and offline compressors absent; clean cooks are
  byte-identical and invalid payloads never partially replace a live resource.

### Stage 5: Add TextureCube Derived Data and Cooking

Dependencies: Stages 1, 3, and 4's proven texture payload reader.

- [ ] Persist optional source provenance for the ordered six-face layout and the
  panorama layout, including exact source hashes and projection inputs.
- [ ] Build canonical cube keys that distinguish every face and its order,
  source layout, panorama projection version, face dimension, canonical exposure
  bits, sRGB, builder/schema version, and target platform/profile.
- [ ] Extend the shared texture payload codec to six compatible slices and
  validate face order, dimensions, format, mip progression, ranges, and total
  allocation before publication.
- [ ] Add TextureCube DDC load, rebuild, diagnostics, object-store budgeting,
  corruption recovery, and source-unavailable cache hits.
- [ ] Add a TextureCube cook adapter and cooked loader with no six-image decode,
  panorama decode, projection, or offline compression fallback.
- [ ] Migrate six-face and panorama import, reimport, move/delete behavior,
  thumbnails, skybox use, and focused tests to the new provenance and payload.
- [ ] Add directional face fixtures and tests proving that DDC/cook round trips
  preserve the documented cube orientation.

#### Acceptance Gate

- Six-face and panorama-derived cubes have deterministic cache and cook
  identities, warm editor loads invoke no source pipeline, cooked cubes load and
  render with all sources and projection/build tools absent, and malformed or
  orientation-incompatible data fails before publication.

### Stage 6: Complete Migration, Shared Preview Assets, and Handoff

Dependencies: Stages 2, 4, and 5.

- [ ] Import sphere and box as shared
  `/Engine/Editor/MaterialPreview/Sphere` and
  `/Engine/Editor/MaterialPreview/Box` StaticMesh assets whose authoring models
  live under engine `SourceAssets`.
- [ ] Replace per-document transient OBJ import and manual rooting with a
  retained acquisition service keyed only by canonical virtual asset identity.
- [ ] Migrate repository-owned mesh and texture packages to normalized source
  provenance and run an asset-registry scan for legacy diagnostics.
- [ ] Remove legacy colocated-source resolution separately for each asset class
  only after its migration criteria and explicit rejection coverage pass.
- [ ] Enforce runtime-only target dependency tests that exclude Assimp, source
  image codecs, panorama projection, and offline texture compressors.
- [ ] Move lasting DBLK, descriptor, DMSH, texture payload, key, provenance,
  cooking, load, and failure contracts into their owning runtime documentation.
- [ ] Run the repository's focused native suites, required full build,
  hidden-window editor smoke test, cooked-runtime smoke tests, and ignored-output
  inspection.

#### Acceptance Gate

- All three asset classes load reproducible cooked output without authoring
  inputs or editor builders, shared material previews perform no transient OBJ
  import, migrated packages emit no legacy diagnostics, runtime dependencies
  enforce the selected boundary, and no lasting contract remains only here.

## Completed Predecessor StaticMesh Stage Record

Only completed predecessor work is retained below. Its former cooking,
material-preview, and final-handoff stages are superseded by integrated Stages
1, 2, and 6 above.

### Predecessor Stage 0: Freeze StaticMesh Contracts and Fixtures

- [x] Define `FStaticMeshSourceImportData`, including optional normalized source
  path, source content hash, importer identity/version, and import settings.
- [x] Define the `DMSH` header, chunk table, required chunks, numeric limits,
  alignment, checksum, endianness, and schema-version policy.
- [x] Define the target-platform identifier and mesh-builder version ownership.
- [x] Define the exact derived-data key byte encoding; do not hash formatted
  diagnostic strings or native struct memory.
- [x] Define cooked `.dbulk` naming and package-relative lookup rules, including
  how a future archive replaces the loose companion.
- [x] Add small deterministic fixtures covering one section, multiple material
  slots, multiple UV channels, vertex colors, and malformed payloads.
- [x] Record the source-directory migration compatibility window and removal
  criteria for legacy colocated source resolution.

#### Acceptance Gate

- The format and key contract has one selected representation for every field,
  no unresolved alternative layouts, and fixtures sufficient to test the reader
  without invoking Assimp or an RHI.

### Predecessor Stage 1: Make StaticMesh Source Provenance Optional and Relocatable

- [x] Replace the required `SourceFile`/settings coupling with optional
  `FStaticMeshSourceImportData` while retaining backward load compatibility.
- [x] Resolve new source paths only beneath project or engine `SourceAssets`.
- [x] Continue resolving legacy package-relative and mounted source paths during
  migration.
- [x] Update import to copy source models into the correct `SourceAssets/Models`
  hierarchy while creating `.dasset` packages under Content.
- [x] Update asset move/delete contributors so moving a `.dasset` does not
  accidentally relocate shared source art, and source deletion requires an
  explicit source operation.
- [x] Add editor diagnostics and source-path repair for missing or moved sources.
- [x] Update version-control documentation for the new directory boundary and
  LFS policy.

#### Acceptance Gate

- New assets import with portable source provenance outside mounted Content;
  legacy assets still load; and an asset with no source metadata can exist
  without failing solely because the source field is empty.

### Predecessor Stage 2: Implement the Native StaticMesh Codec

- [x] Add explicit encode/decode structures independent of
  `FStaticMeshRenderData` memory layout.
- [x] Encode and decode bounds, LOD metadata, vertex streams, index buffers,
  sections, and stable material-slot identifiers.
- [x] Validate magic, versions, platform, checksum, counts, offsets, overlaps,
  alignment, allocation limits, enum values, and cross-chunk references.
- [x] Reject NaN/Infinity positions and bounds, invalid indices, empty required
  geometry, and sections outside index or vertex ranges.
- [x] Add deterministic round-trip tests against Stage 0 fixtures.
- [x] Add truncation, corruption, integer-overflow, decompression-bomb, unknown
  chunk, and unsupported-version tests.
- [x] Confirm encoding the same render data twice produces byte-identical output.

#### Acceptance Gate

- Valid fixtures round-trip to render-equivalent data deterministically, all
  malformed fixtures fail without unbounded allocation or partial publication,
  and codec tests do not require Assimp, Vulkan, or a window.

### Predecessor Stage 3: Add StaticMesh Derived-Data Caching

- [x] Add or reuse an `AssetCore` atomic content-addressed object-store API.
- [x] Implement the static-mesh key builder from source bytes, canonical
  settings, builder version, schema version, and target platform.
- [x] Read valid cache objects before invoking source import.
- [x] Write the encoded payload after a successful source build.
- [x] Treat missing, stale, corrupt, and incompatible objects as safe editor
  misses and rebuild when the source is available.
- [x] Preserve the last complete live render data when a rebuild or cache write
  fails.
- [x] Add cache diagnostics for hit, miss reason, key, rebuild, write failure,
  and source-unavailable-but-cached state.
- [x] Add bounded cleanup and disk-budget accounting for static-mesh objects.

#### Acceptance Gate

- A cold load imports once and publishes a valid cache object; a warm load does
  not invoke the source importer; source/settings/builder/platform changes miss
  deterministically; and corrupt cache data is recovered without escaping the
  DDC root or damaging the asset package.

## Validation Matrix

| Area | Validation | Required result |
| --- | --- | --- |
| DBLK codec | Golden encode/decode plus malformed header, table, range, hash, platform, and size fixtures | Deterministic bounded decode with no partial publication |
| Cook transaction | Failure before bulk publish, between bulk and package, and before manifest publish | No published package or manifest references incomplete data |
| Relocation and containment | Move a complete cooked tree and exercise traversal/DDC-path inputs | Valid trees remain loadable; hostile paths cannot escape the cook root |
| StaticMesh DDC | Cold, warm, changed input, corrupt object, missing source | Existing predecessor behavior remains intact |
| StaticMesh cook | Two clean cooks, source/Assimp removal, invalid DMSH and descriptor variants | Byte-identical valid output; invalid input hard-fails transactionally |
| Texture2D key and DDC | Change source, every build setting, schema, builder, platform/profile; remove source after warm build | Every semantic change misses; valid source-independent cache hit succeeds |
| Texture2D cook | BC1, BC3, BC5, BC7, NPOT and tail mips with source/build tools removed | Cooked payload validates, uploads, and samples correctly |
| TextureCube key and DDC | Change each ordered face, face order, layout, panorama dimension/exposure, projection version, sRGB, and platform | Every output-affecting change changes identity |
| TextureCube codec | Six-slice round trip, inconsistent face/mip/format, corrupt ranges, directional fixture | Valid orientation survives; inconsistent or corrupt data is rejected |
| TextureCube cook | Six-face and panorama clean cooks with source/projection tools removed | Deterministic cooked cubes load and render correctly |
| Runtime dependencies | Inspect/link runtime-only target without import/build dependencies | No Assimp, source decoder, projection, or offline compressor dependency |
| Migration | Repository package scan plus deliberate legacy acceptance/rejection tests | Compatibility remains until each class meets documented removal criteria |
| Preview lifetime | Multiple material documents, sphere/box switching, GC, teardown | Shared native assets remain valid and transient OBJ import is absent |
| Rendering | Required full build, Vulkan texture coverage, hidden editor and cooked-runtime smoke tests | Meshes, materials, 2D textures, cubes, and previews render without validation errors |
| Repository output | Inspect tracked/ignored source, authored packages, DDC, Cooked, and manifest products | Inputs are tracked as policy requires; generated DDC and Cooked output remains ignored |

## Definition of Done

- StaticMesh, Texture2D, and TextureCube editor loads prefer valid
  content-addressed platform data and can use it while source art is
  unavailable.
- All three asset classes produce deterministic cooked packages plus DBLK
  payloads and load them through an explicit runtime mode with no source or DDC
  fallback.
- Runtime-only targets do not depend on Assimp, source-image decoders, panorama
  projection, or offline texture compression.
- DBLK containers, logical descriptors, manifests, keys, and asset payloads are
  versioned, platform-qualified, checksummed, bounds-checked, path-contained,
  and transactionally published.
- Mesh and texture source provenance is optional, portable, normalized beneath
  project or engine `SourceAssets`, and sufficient for editor reimport.
- Material-preview meshes are stable shared `/Engine` assets and no longer
  create or root transient meshes from OBJ per preview.
- Required focused tests, full build, editor smoke, cooked-runtime smoke,
  deterministic cook, corruption, dependency, and repository-policy validation
  pass.
- Lasting behavior is documented by owning runtime domains and this active plan
  contains no sole architectural specification.

## Deferred Follow-ups

- Asynchronous source hashing, import, DDC I/O, payload decode, compression, and
  GPU upload.
- Texture and mesh streaming, partial mip or LOD reads, residency budgets, and
  memory statistics.
- Remote/shared DDC and distributed asset building.
- Final archive, patch, install-chunk, and content-delivery systems that replace
  loose `.dbulk` placement without changing logical payload descriptors.
- Additional asset consumers such as skeletal meshes, audio, animation,
  collision, ray-tracing acceleration data, or runtime-generated persistence.
- HDR/floating-point texture platform formats, texture arrays, cube arrays,
  volume textures, and virtual textures.
- Automatic mesh LOD generation, platform optimization, meshlets, and clustered
  geometry.

## Related Documentation

- `Documentation/Runtime/Assets/AssetPackages.md`
- `Documentation/Runtime/Assets/AssetDataLifecycle.md`
- `Documentation/Runtime/World/LevelSystem.md`
- `Documentation/Runtime/Rendering/MaterialSystem.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Rendering/TextureSystem.md`
- `Documentation/Runtime/Rendering/CubeTextures.md`
- `Documentation/Development/VersionControl/ContentVersionControl.md`
- `Documentation/Plans/TextureSupport.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`

## Related Code

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/EquirectangularTextureCube.cpp`
- `Engine/Source/Runtime/AssetCore/Public/DerivedDataObjectStore.h`
- `Engine/Source/Runtime/AssetCore/Private/DerivedDataObjectStore.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/DerivedDataCache.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/StaticMeshImportDialog.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialPreview.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetImportTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/MaterialTests.cpp`
