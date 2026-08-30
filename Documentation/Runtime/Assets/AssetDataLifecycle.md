# Asset Data Lifecycle and Storage

Summary: Define authored, derived, cooked, and runtime asset-data ownership and transitions.

Modules: Engine, DerivedDataCache, StaticMeshBuild, SkeletalBuild, TerrainBuild, TextureBuild, AssetForgeBuiltins

Last reviewed: 2026-08-30

Durin separates asset identity, authoring input, rebuildable derived data, and
deployable runtime data. File suffixes describe those lifecycle contracts, not
merely whether a file contains binary bytes.

## Serialization and production ownership

Persistent values use the common archive protocol rather than paired
direction-named codecs. Runtime `Engine` values own their bidirectional
`Serialize(FArchive&)` field order and validation for DDC and cooked payloads;
Developer `TextureBuild`, `StaticMeshBuild`, `SkeletalBuild`, and `TerrainBuild` own normalized,
source-independent recipes and canonical build-key inputs;
`AssetForgeBuiltins` adapts standard concrete source formats into those
normalized values. `DerivedDataCache` owns the backend-neutral
`bucket + key -> opaque immutable bytes` contract, the private local filesystem
backend, and the family-neutral Build Framework that adapts cache results inside
`FBuildSession`; recipe modules reach cache query/store only through that
session. `Engine` owns package, descriptor, container, manifest, and
atomic-publication formats without interpreting Engine payloads.

Builder and translator versions invalidate production identity. Payload schema
and stable value identifiers determine runtime readability, so a producer
version change does not by itself make a compatible payload unreadable.

`DerivedDataCache` owns a synchronous local derived-data request boundary. An
immutable `FBuildDefinition` selects a versioned local `IBuildFunction`, carries
an existing canonical key plus opaque local inputs, and declares one expected
value. `FBuildSession` performs query, cached-value validation, local build,
built-value validation, store, and cleanup in that order and reports structured
origin, status, failure phase, and bounded nanosecond durations for each
executed phase. It does not own worker threads, priorities,
callbacks, dependency graphs, remote execution, or typed asset interpretation.

StaticMeshBuild registers the StaticMesh render/collision functions as one
atomic module-owned transaction; SkeletalBuild independently registers the
SkeletalMesh and AnimationClip functions as another transaction; TerrainBuild
does the same for TerrainHeightmap and the five Terrain World product functions;
TextureBuild does the same for Texture2D, TextureCube, and VolumeTexture. Each
transaction rolls back registrations acquired by a failed attempt and resets
the complete set in reverse order during owner retirement. Each family retains
its build keys, cache namespace, value schema, codec, and validation policy.
Terrain function identities intentionally retain their historical
`Durin.GeometryBuild.Terrain...` prefix: the identity is persisted production
identity rather than the selectable module name, so this ownership extraction
does not invalidate otherwise compatible disposable cache entries.
TextureBuild's Texture2D compilation domain calls the synchronous session from
its workers and directly owns admission, cancellation, supersession, metrics,
the completion mailbox, and main-thread publication. AssetForgeBuiltins retains TextureCube source
normalization, private Scene parsing/orchestration, Terrain source decoding/coalescing, and GameThread
publication. Shader and other unrelated DDC paths remain direct family clients.

Engine's object-aware compilation aggregate owns asynchronous domain
registration, frame pumping, selected-object finish/cancel, aggregate progress,
successful post-compile notification, and shutdown placement. Concrete domains
retain scheduling, DDC, validation, and publication. DerivedDataCache build
function registration still uses a module callback gate for bounded synchronous
calls but does not become a compilation domain. See
[Asset Compilation](AssetCompilation.md).

Accepted asynchronous Texture2D requests use TextureBuild's terminal
`FTexture2DCompilationResult` vocabulary and complete their observer exactly once,
including cancellation and supersession. The family domain still owns request
identity, workers, typed publication, and the thread on which it pumps that
completion. Editor-side commit and recovery sequencing is separately defined by
[Async Asset Operations](../../Editor/Architecture/AsyncAssetOperations.md);
it does not move scheduling or typed build policy into DurinEd.

Texture2D uses three distinct terms at this boundary. Import captures one
physical source file and translates its immutable encoded bytes into canonical
RGBA8 pixels. Build is the detached
`FTexture2DBuildRequest` to `FTexture2DBuildProduct` transformation and never
observes an asset object. Compilation schedules that build for a specific
`DTexture2D`, applies cancellation and supersession, and publishes the product
on GameThread. Authored describes authoritative persisted package state; it is
not the name of the compilation domain or one of its requests.

## Storage Classes

| Class | Typical location | Suffix | Authoritative for | May be deleted locally |
| --- | --- | --- | --- | --- |
| Physical source input | User-selected physical file closure | Source-specific | Explicit Import, Reimport, or Reimport From File only | Yes, after a successful authored save if reimport is not needed |
| Object package | Mounted content directory | `.dasset` | Asset identity, editable object state, and inline canonical imported data | No |
| Authored bulk segment | Beside its object package | `.dbulk` | External canonical imported fields selected by DAST v7 metadata | No |
| Derived data | `DerivedDataCache/` | `.bin` | Nothing; it accelerates editor and cook work | Yes |
| Cooked package | `Cooked/<Platform>/...` | `.dasset` | Runtime object metadata for that cook | No |
| Cooked bulk data | Beside its cooked package initially | `.dbulk` | Runtime payload bytes for that cook | No |
| Local state | `Saved/` | Format-specific | Diagnostics, sessions, and user-local state | Yes |

Persistent asset identity is defined by
[Asset Packages](AssetPackages.md#paths-and-mounts). A standalone-family source
hint is instead an optional explicitly based asset-relative,
project-relative, or absolute physical path used only by explicit Reimport.
Neither kind identifies a DDC key, `.bin` object, `.dabulk`/`.dbulk` file, or
byte offset, and asset paths and source hints are not interchangeable.

## Runtime Data Domain

Engine has one immutable `FAssetRuntimeConfiguration` for each initialized
runtime lifetime. `Authored()` selects the authored execution domain with
canonical imported data and disposable DDC available. The validated
`Cooked(...)` factory requires
an absolute normalized cook root and fixes the payload policy to
`CookedPayloadRequired`. `InitializeAssetManager` may reopen a shut-down
runtime with a new configuration, but it rejects replacement while a different
configuration is initialized. Engine post-load code queries only this read-only
domain and payload policy. There is no mutable process-wide package-load mode.

Package creation, publication, load, unload, and residency transitions are
defined by [Asset Packages](AssetPackages.md#runtime-lifetime). Catalog
admission and refresh plus relocation, deletion, and Fix Up transactions are
defined by [Asset Catalog And Mutation](AssetCatalogAndMutation.md). This
document uses those boundaries only to select authored versus cooked payload
policy.

## Authored Packages

An editor DAST v7 package and optional raw `.dbulk` segment contain authoritative object and imported state:

- reflected properties and cross-package asset references;
- bounded decoder-free canonical imported data, inline or in authored bulk;
- optional source hints and provenance used only by explicit Reimport;
- build settings that contribute to derived-data keys.

Large platform render payloads do not belong in authored storage. Canonical
imported arrays use `FEditorBulkData`, allowing Engine to keep small values
inline and place large values in package-resource ranges of the raw segment.
Standalone family import data stores an explicit `AssetRelative`,
`ProjectRelative`, or `Absolute` hint base beside each optional hint. Resolution
never consults an asset mount and occurs only after the user invokes Reimport.

Physical source input is not rebuild authority and is not a runtime asset.
Texture2D, StaticMesh, TextureCube, VolumeTexture, TerrainHeightmap,
SkeletalMesh, and AnimationClip persist the canonical imported data required by
their builders. Runtime build settings remain on their assets; no asset also
persists a generic replay graph or mounted-source request.

Import, Reimport, and Reimport From File are the only paths that read physical
sources, and none mutates them. See
[Source File Workflows](../../Editor/Guides/SourceFileWorkflows.md).

### Import-Time Build Policy

Import is an editor authoring operation, not a cook. It creates or updates the
authored `.dasset`, records portable source provenance, builds data required for
immediate editor use, and populates the DDC. It does not create `Cooked/`
packages, `.dbulk` companions, or `CookManifest.bin`.

Scene import creates ordinary independent output assets and no aggregate
management companion. It is creation-only and offers no whole-scene reimport,
stable reconciliation, or generated-output recovery. Built-in implementation
state, editor diagnostics, and extension-module identities do not enter cooked
runtime ownership. See
[Asset Import Framework](../../Editor/Architecture/AssetImportFramework.md).

Every standalone family is a direct importer: it validates the destination,
captures each selected file once, decodes canonical imported data, builds a
detached product, atomically commits asset state and concrete family import
data, then saves independently. Reimport and Reimport From File repeat that
sequence without a graph, provider, generic job, replay provenance, or
mounted-source mutation.

Scene captures one immutable source closure and owns a private stable
topological order for its peer outputs. It performs complete decode, build,
relationship, resource-limit, and collision validation before the
non-cancelable publication boundary, then binds and saves the package set
atomically. Its transient nodes are neither public framework graphs nor
persisted replay data.

Package persistence is a subsequent operation, not a reversible part of the
asset-state transition. A save failure leaves the newly published package
Dirty and reports persistence failure separately, while the prior disk/catalog
state remains intact. The editor can retry Save without rerunning import.

The current import behavior is:

| Asset | Import-time build | Persistent outputs |
| --- | --- | --- |
| StaticMesh | Import canonical geometry and build render/collision payloads | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| Texture2D | Decode canonical pixels, then generate mips and platform format | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| TextureCube, six-face | Decode and validate six canonical faces, then build platform faces | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| TextureCube, panorama | Decode and project the panorama into canonical faces, then build platform faces | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| VolumeTexture | Decode a canonical voxel volume and build its platform mip chain | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| TerrainHeightmap | Decode canonical uint16 samples and build the terrain payload | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| Skeleton | Validate and persist the canonical reference hierarchy and structural compatibility identity | Authored `.dasset` |
| SkeletalMesh | Persist canonical geometry/influences, validate Skeleton compatibility, and build LOD0 | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| AnimationClip | Persist canonical tracks/keys, validate Skeleton compatibility, and build clip data | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| Assets without an external platform payload | Construct and save reflected authoring state | Authored `.dasset` |

StaticMesh, texture, SkeletalMesh, and AnimationClip import currently build the
Win64 Game platform/profile variant eagerly so the editor has immediately
usable data. This is a platform build stored under rebuildable DDC ownership;
it is not cooked publication. Cook may later validate and reuse equivalent
payload bytes, but only an explicit cook places them under `Cooked/` ownership.

### Optional Asset Operation Boundaries

Runtime Engine owns asset state and typed optional operation contracts:
`IStaticMeshPostLoadFeature`, `IStaticMeshCollisionBuildFeature`,
`ITexture2DPostLoadFeature`, `ITextureCubePostLoadFeature`,
`ITerrainHeightmapDerivedDataLoadFeature`,
and `ISkeletalDerivedDataFeature`.
Runtime consumers invoke exactly one provider through a bounded modular-feature
visitor. No provider reference or provider-authored callable escapes that
visitor; zero providers is an explicit unavailable result and multiple
providers is an explicit ambiguity rather than registration-order selection.

`StaticMeshBuild` owns static-mesh post-load and collision construction,
`TextureBuild` owns texture post-load, `TerrainBuild` owns Terrain derived-data
loading, and `SkeletalBuild` owns skeletal/animation derived-data loading.
`AssetForgeBuiltins` owns only explicit import/reimport providers and editor
save-readiness policy; Engine, Build, and Cook consumers do not acquire an
importer dependency. Each build module instance owns its provider objects and
generation-bound registration tokens, so owner retirement rejects new calls
and waits for admitted visitors before provider state is destroyed.

Terrain post-load is the asynchronous exception to the otherwise synchronous
boundary. Its coalesced workers and Game Thread publishers belong to the
`TerrainBuild`-owned `TerrainDerivedDataLoads` operation group before the
feature visitor returns. Module retirement closes the whole group with
module-shutdown cancellation. Unload may proceed only after the group reports
no active tasks, retained results, or deferred/worker callables. Cooked loads
never invoke these editor-only operation features.

### Skeletal Authored State

`DSkeleton` is a package-only runtime asset. Its parent-before-child canonical
bone array stores name, parent index, and finite decomposable local reference
transform. Compatibility encoding version 1 hashes the exact canonical names,
parents, and float32 reference matrices with XXH3-128. The identity deliberately
excludes package path, source indices, inverse binds, materials, and animation
tracks.

The canonical little-endian compatibility stream begins with ASCII `DSKC`,
encoding version 1, and the bone count. Each bone contributes its signed parent
index, UTF-8 name length and bytes, then the 16 row-major float32 entries of its
local reference matrix. Negative zero is canonicalized to positive zero and the
rotation sign is canonical before encoding. The maximum bone count is 65,535.

`DSkeletalMesh` and `DAnimationClip` each persist both a hard `DSkeleton`
reference and the expected compatibility identity. Validation requires both to
match; an equal name or hash without the referenced Skeleton is insufficient.
The mesh also persists its mesh-node bind transform, stable material slots,
LOD0/section/bounds summary, and payload descriptor. The clip persists its
stable name, duration and track/key summary, and payload descriptor. Their
large geometry, influence, inverse-bind, time, and typed-key arrays are detached
immutable CPU payloads: they contain no source token, reflected object, RHI
handle, playback clock, evaluated pose, or palette state.

Authored editor packages may retain a content-addressed rebuild key and compact
source-hint/import metadata. A loaded package first attempts a validated DDC
object. Missing or corrupt disposable data invokes the owning family build from
resident canonical imported data and current settings; it never issues an
import request, resolves a hint, or rewrites source metadata. Scene outputs
retain no aggregate recipe, but each SkeletalMesh and AnimationClip rebuilds
independently from its own canonical data plus referenced Skeleton.
`DSkeleton` has no external payload and therefore no DDC object or DBLK
companion.

## Derived Data Cache Objects

Generic content-addressed DDC entries are opaque `.bin` values.
`DerivedDataCache` validates logical buckets and canonical lowercase 128-bit
keys, returns immutable `FSharedByteBuffer` values, distinguishes hit, miss,
invalid request, excessive value, and storage failure, and performs bounded
deterministic trim. Its filesystem paths and backend type remain private. The request's
build function and expected value name select one owner-defined decoder; the
cache does not identify a type from the bytes. That owner validates its schema,
producer, bounds, structure, and checksums. Native artifacts such as shader
SPIR-V and reflection sidecars may retain their own strict file grammar beneath
a namespaced subtree. Every DDC entry remains disposable and its authored inputs
remain authoritative.

A DDC key must be built from a canonical byte encoding of every input that can
change the output, including:

- canonical imported-data identity and payload fingerprint;
- normalized build and import settings;
- payload schema and builder versions;
- target platform and any relevant feature profile.

Source hints, timestamps, and physical paths do not enter build keys. DDC paths
are derived from keys and must never be serialized into `.dasset`. Missing,
incompatible, truncated, or corrupt objects are safe cache misses because the
authored package closure retains every local rebuild input.

DDC writes use Core's shared atomic byte-publication API: a fixed-length
same-directory temporary file is flushed and closed before replacement. The
temporary name is independent of the destination name, and DDC round trips are
supported beyond the traditional Windows `MAX_PATH` boundary under the
[physical file I/O contract](../Core/FileIO.md).
Owners validate reserved fields, versions, declared sizes, allocation limits,
structural invariants, and checksums before publishing data. A cache write failure does not
invalidate a complete in-memory build result.

For StaticMesh render/collision, Texture2D/TextureCube/VolumeTexture,
SkeletalMesh/AnimationClip, and TerrainHeightmap requests, invalid cached bytes
are validated by the registered family function and become rebuildable misses
from canonical imported data. Every family retains a complete local result
after a successful build even when best-effort DDC storage fails, and surfaces
the bounded store diagnostic separately.

TextureCube uses `Durin.TextureBuild.TextureCube@1` with value
`TextureCubePayload` under `TextureCube/Objects`. Explicit import or reimport
decodes and projects a panorama into six canonical authored RGBA8 faces before
the immutable request. Ordinary build, PostLoad, DDC recovery, and Cook consume
those faces and never decode or capture a physical source.

### Skeletal Derived Data

SkeletalMesh objects live under `SkeletalMesh/Objects`; AnimationClip objects
live under `AnimationClip/Objects`. Their schema-2 keys are XXH3-128 over an
explicit canonical encoding of builder and payload versions, target
platform/profile, the owning canonical imported-data identity and payload
fingerprint, current output identity, normalized settings, and Skeleton
compatibility.

The key is an editor rebuild locator, not runtime identity. Import stores a
complete in-memory candidate even when its best-effort DDC write fails. A
missing or corrupt object is always a safe authored miss: ordinary package load
decodes the owning asset's canonical bulk and never replays Scene import.

The registered functions are `Durin.GeometryBuild.SkeletalMesh@1` and
`Durin.GeometryBuild.AnimationClip@1`, both returning `SkeletalPayload`.
Definitions carry the exact Skeleton/count/material target context. PostLoad
validates the complete owner-selected value and rebuilds it from the owning
canonical bulk when necessary; Scene import owns only the initial detached
multi-package transaction and hard Skeleton edges.

### Terrain Heightmap Derived Data

TerrainHeightmap uses `Durin.GeometryBuild.TerrainHeightmap@1`, value
`TerrainHeightmapPayload`, and `TerrainHeightmap/Objects`. Persisting direct
builds query/build/store; explicit non-persisting builds disable both query and
store. Authored load first performs a cache query using the metadata-only
content identity. A validated hit reads no package range; only a miss or invalid
value requests one immutable row-major uint16 sample snapshot before worker execution.
The worker adapts its cancellation token to the session while TerrainBuild
retains coalescing, admission, generation checks, and deferred GameThread
publication. Diagnostics map the session phases and never expose a physical
DDC path or probe a source hint.

## Cooked Packages and Bulk Payloads

Cook produces a platform-qualified runtime view under
`Cooked/<Platform>/`. It writes a cooked `.dasset` containing runtime object
metadata and moves large, already-built payloads into `.dbulk`. Source files,
source-only editor metadata, DDC keys, and DDC paths are not runtime
dependencies.

Cook reachability resolves explicit roots, built-in roots, and registered
external runtime roots before traversal. Hard and soft edges are validated
against their final real asset classes. Produced package bytes rewrite every
redirected reference to that final identity; authored packages and external
stores remain byte-stable. Missing targets, redirect cycles/depth overflow,
type mismatch, corrupt aliases, incomplete reference indexing, or a remaining
redirected runtime identity fail before manifest publication.

Redirector packages are authoring-only and never appear in normal cooked
output. `FCookContext` canonicalizes registered output identities, rejects
redirector package bytes and post-resolution duplicates, and verifies that each
cooked dependency and reflected hard/soft field names an exact real asset. A
cooked runtime therefore needs neither redirector packages nor a mutable alias
table.

Packages without external runtime payloads publish only their cooked
`.dasset`; `FCookContext` does not create an empty `.dbulk` or manifest entry.
`/Engine/Materials/DefaultMaterial` is such a package. Engine exposes it as a
fixed built-in Cook root so a minimal project includes it even though empty
material slots deliberately serialize no reference.

StaticMesh, texture, SkeletalMesh, and AnimationClip cooked packages also omit
import source provenance, rebuild keys, and editor diagnostics. SkeletalMesh
and AnimationClip retain exact hard Skeleton dependencies and compatibility
identities; their logical payload descriptors select fixed type payload IDs in
the package companion. Runtime targets do not deploy `AssetForgeBuiltins`,
Assimp, or editor image decoders.

Cook package construction is a read-only projection of the authored object
graph. Stable source provenance and editor diagnostics are declared
`DPROPERTY(EditorOnly)` and are recursively filtered by the cooked Archive.
Generated `CookedPayload` values, including StaticMesh BodySetup collision
metadata, remain runtime properties and are supplied as owned per-save
replacements. Texture2D, TextureCube, VolumeTexture, TerrainHeightmap,
StaticMesh, SkeletalMesh, AnimationClip, and EnvironmentLighting do not install
those descriptors or clear and restore source fields on their live objects.
Success and failure therefore preserve reflected values, package dirty state,
build revisions, and diagnostics.

`FCookContext` owns the target platform/profile and the single editor-only-data
policy for all contributing asset families. Production contexts filter by
default; a diagnostic context may retain the same editor-only field set across
families. Asset APIs do not expose per-family retention switches. Graph
discovery and value capture run on the asset-owning thread and freeze an owned,
immutable package value; DAST encoding and publication consume that value and
must not read the live graph after capture.

The initial loose-file convention is at most one companion bulk container per
package:

```text
Content/Textures/T.dasset
Cooked/Win64/Game/Textures/T.dasset
Cooked/Win64/Game/Textures/T.dbulk
```

When a package owns external payloads, the companion name is derived from the
cooked package's mount-relative path by
replacing `.dasset` with `.dbulk`. Case and path normalization follow the
package path rules. A cooked package must not persist that physical path.

A package refers to each external payload with a logical descriptor equivalent
to:

```text
PayloadId
LocationKind
Offset
StoredSize
UncompressedSize
Alignment
PayloadHash
PayloadSchemaVersion
TargetPlatform
CompressionMethod
```

`PayloadId` is unique within the package and remains stable when the payload is
moved from a loose companion into a future archive. `LocationKind` initially
selects the package companion; future kinds may select an archive or install
chunk without changing asset references or the asset-specific payload schema.
Offsets and sizes are unsigned, explicitly encoded values rather than native
structure layouts.

The `.dbulk` companion is DURF/DBLK v2. DURF selects the storage format; DBLK
records target/profile and a bounded payload table. Ranges are aligned,
non-overlapping, file-contained, and independently checksummed so failures can
name the owning asset and `PayloadId`.

DBLK and DABK share Engine's bounded little-endian container primitives, but
remain separate formats and authority services. CMNF reuses only lower-level
codec primitives; it remains a Cook manifest, not a payload container.

The reflected asset class and payload slot select exactly one codec before
lookup. That codec owns its schema and bytes—for example texture mip records or
static-mesh vertex/index streams. Engine owns container lookup, bounded I/O,
and descriptor validation, but neither dispatches from payload bytes nor
interprets them. C++ object memory, STL layouts, pointers, and RHI handles are
never serialized.

Runtime asset loaders use `LoadCookedPackagePayload` for the common package
path, `.dbulk` companion, target/profile, and exact-descriptor lookup. Its
`FCookedPackagePayload` result owns the decoded container, so the selected
opaque byte span remains valid for the result lifetime. The owning asset
validates its slot descriptor and schema before lookup, decodes into a detached
candidate, and publishes only after complete type-specific validation. Engine-private wire
helpers provide bounded little-endian reads/writes and checked alignment to the
StaticMesh, Skeletal, Texture, and Terrain codecs; their field order, chunks,
limits, hashes, compatibility rules, and diagnostics remain locally owned.

When an Engine serializer adapts one complete asset-specific payload to an
`FArchive`, the Archive region must advertise an exact remaining-payload bound.
The Engine-private adapter checks that bound and the format's allocation ceiling,
transfers the complete region, decodes into a default-constructed detached value,
and move-replaces the destination only after successful validation. Saving passes
the current value explicitly to its encoder and checks the encoded size before
writing. Missing bounds, excessive sizes, incompatible payloads, corrupt payloads,
and raw Archive failures remain distinct structured outcomes; pointer ownership
and caller-defined commit callbacks do not cross this whole-payload boundary.

The initial texture payload uses no additional container compression because BC
texture data is already compressed and must remain independently addressable by
mip. Other payload types may select an explicit compression method when their
codec and loading policy support it.

### Skeletal Payload Schemas

SkeletalMesh schema 2 and producer 2 use a required-zero first header word. Its
required chunks store metadata and bounds, sections,
positions, vertex attributes, indices, canonical four-slot influences, and
palette indices with inverse-bind matrices. AnimationClip schema 2 and producer
2 use the same framing; its required chunks store clip
metadata, track records, key times, and typed translation/rotation/scale values.

Both codecs use an explicit 64-byte little-endian header, 32-byte chunk
records, 16-byte aligned non-overlapping ranges, zero padding, an XXH3-64 body
checksum, at most 64 chunks, and complete byte consumption. Decoders bound all
counts and allocations to at most 8 GiB per decoded payload, then reject
incompatible target/profile, duplicate or
unknown required chunks, invalid references or enums, non-finite values,
invalid influences/transforms/times, truncation, overlap, overflow, checksum
failure, and trailing required data before publishing a detached candidate.
Neither format serializes a native structure image, pointer, `size_t`, source
token, reflected object, physical cache path, or RHI state.

The logical cooked identities are `<asset-path>#SkeletalMeshPayload.v1` and
`<asset-path>#AnimationClipPayload.v1`. They select fixed type payload IDs in
the package's DBLK companion; no physical DDC or companion path enters the
asset package.

### Implemented Container Contract

`DBLK` version 2 uses DURF header version 1, permanent `FormatId`
`76c5d46c-a3744b7e-9cda6c8f-e0dbcd17`, a 64-byte format header after the
common preamble, 80-byte payload-table entries, and 16-byte default payload
alignment. The header records target platform/profile, table and data offsets,
payload count, and an XXH64 table checksum; DURF owns exact file extent and
front-header integrity. Each entry records the stable payload GUID,
flags, schema, location, compression, alignment, range, uncompressed size, and
XXH3-128 payload hash. Unknown required entries, duplicate identities, invalid enums,
misaligned or overlapping ranges, overflow, trailing size disagreement, and
checksum failure reject the complete container before payload publication.

`FCookedPayloadDescriptor` serializes the same logical identity and compatibility
fields but never a physical path. The implemented target identifiers are Win64
platform `1`, Game profile `1`, and EditorValidation profile `2`;
PackageCompanion location is `1`, no compression is `0`, and Zstandard is
reserved as `1`.

`CookManifest.bin` uses `CMNF` version 1. Entries are sorted by normalized
cook-relative path and name every package and companion with kind, required
flag, size, and XXH3-128 hash. `FCookContext` validates all packages and bulk
containers in staging, publishes companions before their packages, publishes
the manifest last, and removes stale outputs only from the previous valid
manifest. Manifest paths consequently name only canonical real package
identities.

## Cook and Publication Rules

Cooking must be deterministic for identical source bytes, settings, builder and
schema versions, and target platform. It may reuse a validated DDC payload, but
the result is copied into cooked ownership; the runtime never follows a DDC
reference.

For payload-bearing packages, the cooker writes bulk data to a temporary file,
flushes and closes it, validates
the completed container, and publishes it before publishing the cooked package
that references it. Failed cooks remove their temporary output. A stale
unreferenced bulk file is harmless and can be removed by manifest-driven output
cleanup; a package must never reference a partially written container.

Cook output and its deployment manifest are a consistency unit. The manifest
must include every cooked `.dasset` and only the required `.dbulk` companions.
Packaging,
patch generation, installation, and cleanup operate from that manifest rather
than by assuming that every `.dbulk` in a directory is live.

This contract covers asset-level Cook contribution and deterministic
publication. Project-wide Cook-set discovery and installable-build
orchestration require a separately selected workflow.

## Load and Failure Policy

Editor loading may use a valid DDC object and rebuild from source on a safe miss.
Cooked runtime loading has no source or DDC fallback:

1. load the cooked package and resolve its logical bulk location;
2. validate the container, descriptor, platform, schema, ranges, and hash;
3. decode the complete asset-specific payload transactionally;
4. publish runtime data only after all required validation succeeds.

A missing or invalid required cooked payload is an asset-qualified hard load
failure and indicates an incomplete or corrupt installation. Optional payloads
must be explicitly marked by the asset schema; absence is not inferred as
optional merely because a file cannot be found.

Neither editor nor runtime readers trust counts, offsets, sizes, compression
ratios, enum values, or cross-record references from disk. Readers enforce
per-payload and process-appropriate allocation limits and reject integer
overflow, overlapping ranges, trailing required data, and decompression bombs.

## Versioning

The following versions are independent and change for different reasons:

- `.dasset` package format version: object-package envelope changes;
- bulk container version: descriptor table or container framing changes;
- asset payload schema version: texture, mesh, audio, or another payload layout
  changes;
- builder version: output semantics change without necessarily changing its
  readable disk layout;
- target platform/profile: output compatibility differs by runtime target.

A reader may retain deliberate backward compatibility for old package,
container, or payload schemas. A builder-version change normally creates a new
DDC key and cooked output but does not by itself require the runtime reader to
reject an otherwise supported payload schema.

## Naming Decision

`.bin` and `.dbulk` may contain identical asset-specific payload bytes, but they
are not interchangeable:

- `.bin` means a content-addressed, rebuildable cache object with no persistent
  asset reference.
- `.dbulk` means manifest-owned, deployable bulk data referenced logically by a
  cooked package and required by runtime.

The dedicated `.dbulk` suffix lets cook, staging, patching, deployment,
diagnostics, and cleanup distinguish required runtime payloads from arbitrary
binary caches without inspecting every file. A future archive may absorb loose
`.dbulk` containers; `.dbulk` is the loose cooked representation, not a promise
that distribution will always ship one operating-system file per package.

## Repository Policy

Authored `.dasset` packages and source inputs are versioned according to the
content storage policy. `DerivedDataCache`, `Cooked`, and `Saved` remain ignored
generated directories. Cooked output is nevertheless authoritative within a
specific staged build: ignored means reproducible distribution output, not
disposable while that build is running or installed.

## Authored bulk ownership and failure behavior

`FBulkData` owns bounded runtime storage metadata, optional allocation, lock
state, and an optional logical package-resource range; it owns no content hash,
payload GUID, DDC key, schema, target, or physical path. `FEditorBulkData` is an
independent authored value with instance identity, content-derived payload
identity, asynchronous immutable retrieval, and atomic whole-payload update.

DAST v7 live load validates the raw segment extent, digest, ranges, ordering,
and padding before object publication, while leaving external field allocations
unloaded. Family build keys use the editor payload identity before requesting
bytes, so a validated DDC hit performs zero source-range reads. A miss obtains
and owns exactly one immutable payload snapshot before worker execution.

DAST v6/DABK remains a read-only migration input. Canonical resave validates
that complete old closure, transactionally publishes DAST v7 plus raw `.dbulk`,
updates catalog authority, and only then removes `.dabulk`; rollback restores
both companion names and prior registry state. Exact state, wire, and resource
rules are defined by [Package Bulk Data](BulkData.md).

## Domain-qualified inspection and repair ownership

Texture payload inspection reports source, derived, cooked, decoded CPU, and
GPU stages without creating a shared authority descriptor. Construct-free
inspection reads package field trees and storage descriptors; live inspection
joins source-file diagnostics, DDC diagnostics, decoded data, and
render-resource state. Placement labels are capability descriptions such as `SourceFile`,
`EditorPackageCompanion`, `DerivedDataCache`, and `CookedPackageCompanion`, not
backend paths supplied to domain callers.

Repair classifications name the owning explicit workflow:

| Finding | Action owner |
| --- | --- |
| Missing/changed/malformed standalone source | Reimport or select a replacement file. |
| Missing/corrupt authored segment | Restore the package-matching raw `.dbulk`, canonically resave a valid v6/DABK closure, or reimport. |
| Unreferenced editor companion | Explicit package cleanup; inspection never deletes it. |
| Missing/corrupt/incompatible DDC | Domain rebuild; cache data is disposable. |
| Missing/unsupported/failed cooked payload | Recook or upgrade/resave; runtime has no source fallback. |
| Failed GPU publication | Retry the runtime resource after addressing the reported capability/upload failure. |

Inspection is read-only. No status query invokes fallback, rebuild, reimport,
recook, publication, cleanup, or deletion.

## Related Documentation

- [Asset Packages](AssetPackages.md)
- [Asset Catalog And Mutation](AssetCatalogAndMutation.md)
- [Texture System](../Rendering/TextureSystem.md)
- [Content Version Control](../../Development/VersionControl/ContentVersionControl.md)
- [Asynchronous Texture2D Build and Readiness Plan](../../Plans/Archive/2026-08/AsynchronousTexture2DBuildAndReadiness.md)
- [Asset Derived Data and Cooking Plan](../../Plans/Archive/2026-07/AssetDerivedDataAndCooking.md)
