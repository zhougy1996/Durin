# Asset Data Lifecycle and Storage

Summary: Define authored, derived, cooked, and runtime asset-data ownership and transitions.

Modules: Engine, RenderCore, DerivedDataCache, StaticMeshBuild, SkeletalBuild, TerrainBuild, TextureBuild, AssetForgeBuiltins

Last reviewed: 2026-08-31

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
session. `Engine` owns package fields, raw-segment placement, manifest, and
atomic-publication formats without interpreting family payloads.

Builder and translator versions invalidate production identity. Payload schema
and stable value identifiers determine runtime readability, so a producer
version change does not by itself make a compatible payload unreadable.

`DerivedDataCache` owns a synchronous local derived-data request boundary. An
immutable `FBuildDefinition` selects a local `IBuildFunction` by its stable,
case-sensitive `FBuildFunctionName`, carries an existing canonical key plus
opaque local inputs, and declares one expected value. Registration freezes the
function's current version and cache configuration; the version is not part of
registry lookup, and each family canonical key encodes the same builder version
to invalidate incompatible results. `FBuildSession` performs query,
cached-value validation, local build,
built-value validation, store, and cleanup in that order and reports structured
origin, status, failure phase, and bounded nanosecond durations for each
executed phase. It does not own worker threads, priorities,
callbacks, dependency graphs, remote execution, or typed asset interpretation.

The low-level Cache API permits concurrent Get and Put operations under a
logical bucket's shared lock; bounded Trim owns only that bucket exclusively.
Shader compilation is a direct Cache API client because RenderCore already owns
its build orchestration. It stores one complete versioned SPIR-V-plus-reflection
value in `Shaders/CompiledOutput`; machine-local dependency manifests remain a
separate RenderCore optimization and never enter portable DDC values. Asset
recipe families continue to use `FBuildSession` and retain their existing
observable policy.

StaticMeshBuild registers the StaticMesh render/collision functions as one
atomic module-owned transaction; SkeletalBuild independently registers the
SkeletalMesh and AnimationClip functions as another transaction; TerrainBuild
does the same for TerrainHeightmap and the five Terrain World product functions;
TextureBuild does the same for Texture2D, TextureCube, and VolumeTexture. Each
transaction rolls back registrations acquired by a failed attempt and resets
the complete set in reverse order during owner retirement. Each family retains
its build keys, cache namespace, value schema, codec, and validation policy.
Terrain function names intentionally retain their historical
`Durin.GeometryBuild.Terrain...` prefix: the name is a stable production
protocol rather than the selectable module name, so this ownership extraction
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
| Authored bulk segment | Beside its object package | `.dbulk` | External canonical imported fields selected by DAST v9 Bulk Directory metadata | No |
| Derived data | `DerivedDataCache/` | `.bin` | Nothing; it accelerates editor and cook work | Yes |
| Cooked package | `Cooked/<Platform>/...` | `.dasset` | Runtime object metadata for that cook | No |
| Cooked bulk data | Beside its cooked package initially | `.dbulk` | Runtime payload bytes for that cook | No |
| Local state | `Saved/` | Format-specific | Diagnostics, sessions, and user-local state | Yes |

Persistent asset identity is defined by
[Asset Packages](AssetPackages.md#paths-and-mounts). A standalone-family source
hint is instead an optional explicitly based asset-relative,
project-relative, or absolute physical path used only by explicit Reimport.
Neither kind identifies a DDC key, `.bin` object, `.dbulk` file, or
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
admission and refresh plus relocation/Fix Up jobs and destructive deletion are
defined by [Asset Catalog And Mutation](AssetCatalogAndMutation.md). This
document uses those boundaries only to select authored versus cooked payload
policy.

## Authored Packages

An editor DAST v9 package and optional raw `.dbulk` segment contain authoritative object and imported state:

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
and LOD0/section/bounds summary. The clip persists its stable name, duration,
and track/key summary. Their
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
`DSkeleton` has no external payload and therefore no DDC object or cooked bulk
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

TextureCube uses function `Durin.TextureBuild.TextureCube` with value
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

The registered function names are `Durin.GeometryBuild.SkeletalMesh` and
`Durin.GeometryBuild.AnimationClip`, both returning `SkeletalPayload`.
Definitions carry the exact Skeleton/count/material target context. PostLoad
validates the complete owner-selected value and rebuilds it from the owning
canonical bulk when necessary; Scene import owns only the initial detached
multi-package transaction and hard Skeleton edges.

### Terrain Heightmap Derived Data

TerrainHeightmap uses function `Durin.GeometryBuild.TerrainHeightmap`, value
`TerrainHeightmapPayload`, and `TerrainHeightmap/Objects`. Persisting direct
builds query/build/store; explicit non-persisting builds disable both query and
store. Authored load first performs a cache query using the metadata-only
content identity. A validated hit reads no package range; only a miss or invalid
value requests one immutable row-major uint16 sample snapshot before worker execution.
The worker adapts its cancellation token to the session while TerrainBuild
retains coalescing, admission, generation checks, and deferred GameThread
publication. Diagnostics map the session phases and never expose a physical
DDC path or probe a source hint.

## Cooked Packages and Bulk Fields

Cook produces a target-qualified runtime projection beneath
`Cooked/<Platform>/<Profile>/`. Each family owns one PlatformData schema and
uses it for DDC values, Cook capture, and runtime decode. A valid target product
may be reused from DDC, but Cook copies it into package ownership; cooked
runtime never follows a DDC reference or rebuilds from authored/source data.

A cooked Archive supplies persistent, Cook, editor-filter, platform, and
profile context and dispatches `DObject::SerializeCooked` during discovery,
NoDelta planning, value capture, and load. Family overrides serialize detached
or stack-local projections and never mutate authored fields, dirty state,
diagnostics, build revisions, DDC state, or live residency. Editor-only source
provenance, canonical imported data, and rebuild keys are omitted.

Payload-bearing packages use DAST v9 BulkData linker values. Values selected
for inline storage occupy aligned ranges in the `.dasset` Inline Bulk section;
external values occupy aligned ranges in one stable, headerless `.dbulk`
sibling. Registry owns the exact external extent and whole-segment digest, and
Bulk Directory binds every inline/external range to its reflected value. The
raw segment has no DURF header, nested directory, target, schema, or physical
path. Metadata-only packages, including Skeleton and packages whose fields stay
inline, have no companion and no empty manifest record.

The implemented family projections are:

| Family | Cooked field | First runtime consumer |
| --- | --- | --- |
| Texture2D, TextureCube, VolumeTexture | `PlatformData` | texture resource upload |
| StaticMesh | `RenderData`, `CollisionData` | render and physics publication |
| SkeletalMesh, AnimationClip | `PlatformData` | render or animation setup |
| TerrainHeightmap | `PlatformData` | terrain render/collision |
| Material | `ProgramData` | material render-layer publication |
| EnvironmentLighting | `PlatformData` | lighting resource upload |
| Skeleton | metadata only | skeleton compatibility lookup |

Family serializers retain their existing versioned headers, target/profile
facts, bounds, checksums, and semantic validation. Cooked package load validates
the complete package/segment closure and attaches each external `FBulkData`
range before publishing the object graph. It performs no field-range read.
The first explicit family request locks only the required field, decodes into a
detached candidate, validates the family schema, and publishes transactionally.
Missing, truncated, corrupt, wrong-target, or incompatible data is an
asset-qualified hard failure; there is no source, importer, DDC, or synthetic
fallback.

### Cooked mesh runtime residency

`DStaticMesh` and `DSkeletalMesh` expose side-effect-free RenderData and payload
getters. `RequestRenderDataAndResources()` is the ordinary non-blocking entry
point used by mesh-component assignment, registration, and SceneProxy fallback.
Its generation-qualified snapshot separates `Unloaded`, queued/read/decode,
`CpuReady`, `Failed`, and `Cancelled` CPU phases from independently
`Unavailable`, `Queued`, `Ready`, and `Failed` GPU phases. SplineMesh shares its
source StaticMesh request and residency.

The Engine cooked-mesh manager bounds active and pending request bytes, active
request count, retained completion bytes, and completions published per pump.
Its diagnostics expose current and peak counts/bytes plus cumulative async-read
readiness, worker decode/build, and GameThread completion-publication time. The
timings are observational: correctness gates use thread ownership and bounded
work, while performance comparison requires a quiet qualification lane.
Package resources perform the asynchronous reads. Worker tasks receive owned
immutable bytes plus copied material, collision, bounds, bind-transform, bone,
and Skeleton-compatibility facts; they never resolve or mutate a `DObject`,
component, package, BodySetup, live Skeleton, or render resource. The GameThread
alone rechecks object/load/resource generations and metadata identity, installs
the detached candidate and diagnostics, queues GPU initialization, and
invalidates registered consumers.

`EnsureRenderDataAndResourcesBlocking()` is an explicit compatibility boundary.
It submits or joins the same request identity and pumps only that owner's
manager work before using the shared synchronous fallback. It is not used by
getters, component ticks, SceneProxy creation, or renderer preparation. A
failure or cancellation remains sticky for that load generation;
`RetryRenderDataAndResourcesBlocking()` advances the generation and explicitly
repeats the same contract.

Manager shutdown stops admission, cancels reads and decode work, reports a
current cancellation terminal to live assets without publishing CPU or GPU
state, and drains tasks and completions before package, task, render, or Engine
lifetime ends. Reinitialization creates a new manager scope; a cancelled asset
resumes only through explicit retry. Package retirement, unload, destruction,
or a newer generation cannot publish stale candidates.

Terrain World is a manifest-owned opaque-stream exception rather than an asset
field container. `TWMF` records the exact offset, size, product hash,
dependencies, region extent, and region hash for each installed product.
`FCookContext::AddRawPackage` publishes one headerless region segment and CMNF
records it as `PackageBulk`. Runtime validates region and product bounds and
hashes before decoding the requested product. It never interprets the region as
a structured bulk container.

## Cook and Publication Rules

`FCookContext::AddPackage` accepts canonical cooked package bytes; package
serialization captures any `FBulkData` ranges and produces the optional raw
segment. `AddRawPackage` admits an already laid-out opaque segment such as a
Terrain region. The ten asset families contribute through class-keyed
registrations owned by `RegisterEngineCookContributors`; individual family
Cook methods are private.

Publication stages and validates complete outputs, writes a required segment
before its referencing package, and publishes `CookManifest.bin` last. CMNF
entries are sorted by normalized cook-relative path and record kind, required
flag, byte extent, and XXH3-128 digest. Raw companions use `PackageBulk`;
package-only output has no companion entry. Cleanup removes stale outputs only
when owned by the previous valid manifest, so an interrupted Cook retains a
complete prior generation or diagnosable staged files.

Win64/Game Cook also supplies `Shaders/ShaderLibrary.dslb` as a
`ShaderLibrary` auxiliary output. It is detached before the store transaction,
validated by its producer, staged and committed with package outputs, and
recorded in CMNF. Failure at its stage or commit participates in the same
reverse-order rollback; it is never published beside the Cook transaction.

Cook reachability resolves explicit, built-in, and registered external runtime
roots before traversal. Redirectors are authoring-only: references are rewritten
to final real identities and redirector packages are omitted. Missing targets,
cycles, type mismatches, corrupt aliases, duplicate output identities, or
incomplete reference projections fail before manifest publication.

`FCookCoordinator` owns project Cook. Explicit roots augment the configured
default Level and registered runtime roots, one asset-registry/reference
snapshot determines the closure, and normalized final package identities are
loaded and captured serially in canonical order. Class-keyed, owner-gated
contributors may prepare derived state and return detached save plans; the
coordinator rejects any contributor that changes authored package bytes or
dirty state. Unsupported classes, stale registry facts, missing or mistyped
references, and duplicate output identities fail before publication.

`CookState.bin` is a canonical editor/tool-only incremental database separate
from CMNF. Its fingerprint includes source package content, resolved dependency
facts and content, target/profile, project Cook settings, contributor revision,
and family producer revision; it excludes timestamps, output paths, schedule,
and DDC location. A Cook hit additionally validates stored output size and
digest. Missing or corrupt output is recaptured and repaired. DDC hits,
rebuilds, captures, and validated Cook hits remain distinct provenance.

All save plans are detached before `ICookOutputStore` opens its transaction.
The local loose store enforces one writer per output root, stages and validates
every changed file, retains overwritten bytes, commits segments before packages,
then `CookState.bin`, and commits `CookManifest.bin` last. Failure or
cancellation before the manifest commit restores the prior closure in reverse
order. Only after manifest publication may paths owned solely by the previous
manifest be removed; unowned files are never cleanup candidates.

## Compatibility, targets, and inspection

DAST v9 main bytes and their exact optional headerless raw segment are the only
supported cooked asset-package representation. Construct-free inspection reads
validated linker BulkData descriptors and never constructs an asset or a
second container model.

The implemented compatibility identifiers are Win64 platform `1`, Game
profile `1`, and EditorValidation profile `2`. Production family Cook and
runtime qualification currently select Win64/Game. Other target/profile pairs
are unsupported and fail explicitly rather than falling back or guessing.

Construct-free inspection reports DAST fields, inline/external placement,
declared segment extent/digest, and compatibility status without constructing
assets or loading field ranges. Live family inspection may additionally report
DDC/build, decoded CPU, and renderer/physics publication state. Inspection is
read-only and never rebuilds, recooks, repairs, deletes, or publishes.

## Versioning and naming

Package format, family PlatformData schema, builder version, and Cook target/profile
are independent. A builder-version change
invalidates DDC/Cook production identity without necessarily changing readable
family bytes. A package or family schema change requires an explicit supported
reader or a hard unsupported-version result.

`.bin` is disposable content-addressed DDC storage. A cooked `.dbulk` is a
manifest-owned deployable raw package segment whose layout is authoritative only
through its `.dasset` or Terrain manifest. The suffix does not promise one
operating-system file per package after future archive/store integration.

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

DAST v9 live load validates the raw segment extent, whole and per-value digests,
ranges, ordering, alignment, and padding before object publication, while
leaving external field allocations unloaded. Family build keys use editor
payload identity before requesting bytes, so a validated DDC hit performs zero
source-range reads. A miss obtains and owns exactly one immutable payload
snapshot before worker execution.

DAST v9 plus its exact optional raw `.dbulk` segment is the sole supported
authored package closure. Canonical resave republishes that closure through the
same bounded artifact publisher as ordinary Save. Projection failure after
content commit is reconciled from authored files and does not roll them back. Exact
state, wire, and resource rules are defined by
[Package Bulk Data](BulkData.md).

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
| Missing/corrupt authored segment | Restore the package-matching raw `.dbulk` or reimport. |
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
