# Asset Data Lifecycle and Storage

Summary: Define authored, derived, cooked, and runtime asset-data ownership and transitions.

Modules: Engine, RenderCore, DerivedDataCache, MeshBuilder, TextureBuild, AssetForgeBuiltins

Last reviewed: 2026-09-23

Durin separates asset identity, authoring input, rebuildable derived data, and
deployable runtime data. File suffixes describe those lifecycle contracts, not
merely whether a file contains binary bytes.

## Authored source compatibility

TextureSource accepts only schema 3 block/layer descriptors, canonical payload
hashes, and compression metadata; v1/v2 sources are unsupported. Transparency
comes from the current channel mask. Authored bulk updates use `UpdatePayload`.
Old Cook outputs must be regenerated from current content.

## Import metadata publication

Importers normalize detached `FAssetImportDataState::SourceData` and call the
state's `Validate` before publication. Common source, source-list and base-state
validation return expected void with `FAssetImportDataError`, owning source index,
role, hint, hash, bounds and schema context. `InspectAssetImportInfo` returns an
expected owned `FAssetImportInfo` only after validation. Object and family-specific validation
return the same contract. StaticMesh and VolumeTexture validate the base schema
first, then format axis, source-role or atlas details into the owned diagnostic
text; no polymorphic cause object is retained. Axis validation returns
`std::expected<void, FStaticMeshImportSettingsError>` with the rejected axis combination. Consumers
format with `FormatAssetImportDataError` at presentation boundaries.
`DAssetImportData::SetState` and its family-specific setters require this validated
state, return void, and only install fields and notify compilation changes.
Invalid external input is reported before calling the setter; object `Validate`
continues to check already stored metadata after loading.

Scene imports resolve source material indices to unique mesh slot indices before
assignment and report missing or ambiguous mappings as import diagnostics.
`DStaticMesh::SetMaterialSlotDefaultMaterial` requires an existing slot index on
the owner thread; it updates the default and invalidates dependent render state.

## Texture source storage compression

`FTextureSource` owns lossless storage: Raw (0), byte-run RunLength (1) and
Zstd (2) retain schema 3 and identical semantic source identities. Initialization
prefers pinned Zstd 1.5.7 level 3, a single frame without dictionaries or worker
threads, falling back to Raw only when compression succeeds without a size win.
Older executables cannot read codec 2; update tools before converting assets.
Descriptors and decoded sizes are validated before compression or allocation.
Decoding requires exactly one complete frame, its declared content size, a
window no larger than the 512 MiB source limit, no dictionary, and the canonical
pixel hash. Trailing data, concatenated/skippable frames and unknown codecs fail.

`Recompress` prepares and verifies a detached candidate, preserving the owner,
bulk instance GUID and source identity. Failed operations leave storage and
residency unchanged; identical storage is a no-op. Replaced residency is detached,
while previously acquired byte buffers remain valid. Source mutation requires
exclusive ownership. `DTexture::ReplaceSourceStorage` commits a verified storage
candidate on GameThread without invalidating build or rendering state. Imports
compress detached source work; ordinary loads, saves, reads and audits do not
implicitly recompress. GPU formats, DDC keys and Cook inputs remain unchanged.

## Serialization and production ownership

Engine's DAST adapters classify and format CoreDObject failures at explicit
boundaries. `FAssetReadResult` carries a read classification and owned diagnostic
text. Package capture keeps `std::expected<void, FPackageCaptureError>`; codec encoding and
`SerializeAssetPackageBytes/Closure` return the existing
`ObjectPackage::FPackageWriterResult`. Failed encoding leaves the caller's output
closure unchanged. Live capture and linker mutation share one encode-and-read-back
validation path; relocation consumers do not repeat that validation.

Saves and mutations use `FAssetWriteResult` for error, message and observed write
effect only. Mutation file ledgers and backup roots belong to
`FAssetMutationResultDetails`; editor deletion reports its file ledger through
`FAssetOperationResult`. Physical save diagnostics remain in the package writer,
with retained backup paths included in failure text at the asset boundary.
`AssetWriteResultFromRead` and `AssetWriteResultFromEncoding` are explicit
adapters. Neither read nor encoding failures implicitly convert to a write result,
and preparation failures never claim committed content.

Field application through `LoadAuthoredObject` returns `FAssetReadResult`, with
success derived only from `EAssetReadError::None`. Failures own a complete diagnostic:
object identity, Archive field route and first failure, plus dependency,
bounds or type details captured at the producer. Original serializer failure
messages survive archive and candidate destruction. External resolver failures
contribute only classification and diagnostic text; operation recovery metadata
stays with the resolver's owning operation. Serializers retain their void
interface and in-place writes. The same field application serves ordinary and
private loading; the caller owns candidate cleanup and PostLoad policy.

Persistent values use the common archive protocol rather than paired
direction-named codecs. Runtime `Engine` values own their bidirectional
`Serialize(FArchive&)` field order and validation for DDC and cooked payloads;
Developer `TextureBuild` and `MeshBuilder` own normalized
source-independent recipes. Engine owns all
asset key encoding, editor-only cache lookup/validation/fallback, diagnostics,
and typed application. AssetForgeBuiltins adapts explicit physical imports to
canonical inputs and owns editor transactions.

DerivedDataCache owns only the backend-neutral
`bucket + key -> opaque immutable bytes` storage contract and private local
backend. It has no build-function registry, request framework, or recipe policy.
Builder/translator versions invalidate production identity;
payload schema and stable value identifiers determine runtime readability.

Low-level Get and Put permit concurrency under a logical bucket's shared lock.
Requests never scan or evict entries. Shader compilation remains a direct Cache
API client: RenderCore owns its orchestration and stores complete versioned
SPIR-V-plus-reflection values in `Shaders/CompiledOutput`; machine-local
dependency manifests do not enter portable values.

MeshBuilder and TextureBuild expose explicit build module interfaces.
Build implementations own algorithm metrics and producer versions.
Engine owns keys, runtime serialization, DDC policy, and object application;
recipes retain no cache keys, origin, persistence diagnostics, or live assets.

Texture cache decoding reuses an unpublished platform destination;
on a miss that incomplete destination is discarded before recipe application.
Cooked texture loading retains its input
buffer or BulkData lease until synchronous decoding finishes and publish only
after exact payload completion. StaticMesh cooked products keep joint render
and collision publication at the product boundary, including metadata checks.
The ordinary family serializer does not own rollback or resource updates; see
[Serialization](../Core/Serialization.md) for the in-place payload contract.

Import translates captured physical sources into canonical authored inputs;
build transforms detached inputs into derived products; compilation schedules
and applies those products for live objects. Scheduling, request identity,
completion, and lifetime rules belong to [Asset Compilation](AssetCompilation.md).
Editor commit and recovery belong to
[Async Asset Operations](../../Editor/Architecture/AsyncAssetOperations.md).

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
`MakeSourceHint` returns an expected `FSourceHint` containing Base and Hint;
`ResolveSourceHint` returns an expected physical `FFilePath`. Their shared
`FSourceHintError` owns input/package/project paths, base, operation and filesystem
error context. Failed calls expose no success value. Filesystem conversion stops at its first
failure, retaining the failing path category. Presentation uses
`FormatSourceHintError`.

## Runtime Data Domain

Standalone Game selects the Cooked domain before loading Engine/project assets
and mounts the executable's cooked Engine/Game directories read-only. Material
bytecode validates the saved runtime ABI without requiring a compiler provider;
source-independent sky lighting uses ordinary cooked TextureCube dependencies.

Engine has one immutable `FAssetRuntimeConfiguration` for each initialized
runtime lifetime. `Authored()` selects the authored execution domain with
canonical source/build inputs and disposable DDC available. The validated
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

An editor DAST v9 package and optional raw `.dbulk` segment contain authoritative object and source state:

- reflected properties and cross-package asset references;
- bounded canonical source data, inline or in authored bulk;
- optional source hints and provenance used only by explicit Reimport;
- build settings that contribute to derived-data keys.

Large platform render payloads do not belong in authored storage. Canonical
imported arrays use `FEditorBulkData`, allowing Engine to keep small values
inline and place large values in package-resource ranges of the raw segment.
Field state, placement, validation, and resource lifetime are defined by
[Package Bulk Data](BulkData.md). Optional source-hint resolution is defined by
[Asset Import Framework](../../Editor/Architecture/AssetImportFramework.md#optional-source-hint-contract).

StaticMesh separates persistent `FStaticMeshSource` from detached
`FStaticMeshDecodedGeometry`. `Initialize` validates a complete replacement;
`AcquireGeometry` returns a shared const handle that survives source replacement,
release and asset destruction. `ReleaseGeometry` drops only that source value's
decoded ownership. Asset publication releases decoded residency; detached build
requests release it with their operation lifetime. Explicit readers and source
copies may retain it longer. Canonical unsaved bulk remains owned throughout.
The source and codec contracts are detailed in
[StaticMesh source and building](StaticMeshBuilding.md).

Physical source input is not rebuild authority and is not a runtime asset.
Texture2D, StaticMesh, TextureCube, and VolumeTexture persist the canonical source data required by
their builders. Runtime-required metadata remains on assets, while offline-only
texture build settings are editor-only; no asset also
persists a generic replay graph or mounted-source request.

Texture2D, TextureCube, and VolumeTexture share the sole reflected
`DTexture::Source` and `DTexture::AssetImportData` storage. Checked-in authored
packages and new saves use only these base declaring identities. Package
identity, collision, and strict-load rules remain defined by
[Asset Packages](AssetPackages.md).

Import, Reimport, and Reimport From File are the only paths that read physical
sources, and none mutates them. See
[Source File Workflows](../../Editor/Guides/SourceFileWorkflows.md).

### Import-Time Build Policy

Import creates or updates authored `.dasset` packages and optional authored
`.dbulk` companions, records source provenance, builds data for immediate editor
use, and populates the DDC. Only Cook publishes `Cooked/` packages, cooked bulk,
and `CookManifest.bin`. Factory/reimport acceptance, live-state commit versus
save failure, and atomic Scene publication are defined by
[Asset Import Framework](../../Editor/Architecture/AssetImportFramework.md).

The current import behavior is:

| Asset | Import-time build | Persistent outputs |
| --- | --- | --- |
| StaticMesh | Import canonical geometry and build render/collision payloads | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| Texture2D | Decode canonical pixels, then generate mips and platform format | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| TextureCube, six-face | Decode and validate six canonical faces, then build platform faces | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| TextureCube, panorama | Retain decoded full-precision panorama, then derive projected platform faces | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| VolumeTexture | Decode a canonical voxel volume and build its platform mip chain | Authored `.dasset` plus optional raw `.dbulk`, DDC `.bin` |
| Assets without an external platform payload | Construct and save reflected authoring state | Authored `.dasset` |

StaticMesh and texture import request the
Win64 Game platform/profile variant for editor use. StaticMesh interactive import
and reimport await asynchronous CPU completion before save; object-returning
factories and explicit bool APIs use a selected completion barrier. This is a platform build stored under rebuildable DDC ownership;
it is not cooked publication. Cook may later validate and reuse equivalent
payload bytes, but only an explicit cook places them under `Cooked/` ownership.

### Optional Asset Operation Boundaries

Runtime Engine owns asset state and typed optional operation contracts:
`IMeshBuilderModule` and `ITextureBuildModule`.
Texture consumers call one fixed module implementation. A missing module is an
explicit unavailable result.

StaticMesh consumers acquire a retained module session before worker dispatch, as specified
in [Static mesh building](StaticMeshBuilding.md). Texture2D compilation, TextureCube
PostLoad, and scene import likewise retain `FTextureBuildSession` across worker work.

Build contracts remain owned by Engine, and both build modules publicly depend
on Engine. `Texture2DData.h` carries source/settings values and CPU platform
mips; `Texture2DBuildTypes.h` exposes recipe values, while
`Texture2DBuild.h` owns requests, cache identity, persistence controls and
observations. Texture recipes return `std::expected<Product, Error>` with owned
recipe products; Cube normalization likewise returns its canonical input by value.
Texture2D input validation uses `FTexture2DInputError`, recipe failures use
`FTexture2DBuildError`, and Cube/Volume failures use `FTextureBuildError` with
their recipe stage and diagnostic. Pure recipes and source preparation do not log.
Engine records synchronous failures once and retains asynchronous Texture2D recipe
causes in `FTexture2DCompilationDiagnostic::BuildCause`, separate from completion
errors. Cube/Volume public orchestration and `BuildTexture2DDetached` return
`FTextureBuildOperationError`: failed, invalid input with a concise reason, or
canceled. Object-level Texture2D completion errors likewise carry an actionable
`InputReason` instead of a nested recipe cause. The internal platform-cache worker
passes its diagnostic to the Engine completion owner for reporting.

Synchronous application and submission return expected void; accepted asynchronous
completions retain succeeded, failed, canceled, and superseded states. Cache
read/decode failures fall back to a rebuild, and cache write failures do not turn
usable products into import failures. Persistence diagnostics remain independently
observable. Expected does not imply rollback; publication effects retain their
existing owner and contract.
`StaticMeshData.h` supplies resource-free CPU streams and LOD
metadata. StaticMesh recipes return those owned values; Engine moves the arrays
into `FStaticMeshRenderData`, and owns GPU resource initialization. Recipe and
payload LODs share `FStaticMeshVertexData` without making the recipe product a
disk-schema object. This boundary separates algorithm and runtime resource
responsibilities; it does not promise an Engine-free link target.

`MeshBuilder` owns only detached CPU render construction through `FStaticMeshBuilder`. Engine owns its
PostLoad scheduling, import/Scene build, cache lookup/validation/fallback, and result
application. Authored PostLoad returns after metadata admission; source decoding,
render/ray and collision construction run in the typed worker domain. Cook
constructs a detached projection, even when authored CPU data is absent, without
changing authored bytes, source residency, render revision or dirty state. Only
pending source mutations require a selected wait before cook capture. See
[Asset Compilation](AssetCompilation.md#staticmesh-completion) for bounds,
publication and observational diagnostics. StaticMesh keys are editor-only
Engine-private values; operation
results carry key, origin, descriptor, timings, payload bytes, and persistence
diagnostics without copying them onto assets. Source acquisition, warm-hit reuse,
and detached application follow [Static mesh building](StaticMeshBuilding.md).

Direct `ReplaceSourceRenderData` and `ReplaceRenderData`
operations cancel superseded work and invalidate old render/collision data before
validation. They log CPU replacement failures and expose `GetRenderDataUpdateError`;
CPU residency and GPU readiness remain separate. Valid source settings are retained
even when replacement fails. Collision rebuild failure leaves the new render data
usable with no derived collision. Engine application separately decides dirtying
and material-slot upgrade notification; requested dirtying also applies when a
direct build fails after accepting source settings. The async authored-candidate path retains
its separate preparation/commit contract for import and reimport.
Cook reports existing payload capture rather than inferring an old build origin
from the asset.
`AssetForgeBuiltins` owns only explicit import/reimport providers and editor
save-readiness policy; Engine, Build, and Cook consumers do not acquire an
importer dependency. Module code leases reject unload while build sessions remain;
consumers stop admission and drain work before releasing them.

## Derived Data Cache Objects

Generic content-addressed DDC entries are opaque `.bin` values.
`DerivedDataCache` validates logical buckets and canonical lowercase 128-bit
keys, returns immutable `FSharedByteBuffer` values, and distinguishes hit, miss,
invalid request, excessive value, and storage failure. Its filesystem paths and
backend type remain private. The caller selects the owner-defined decoder;
the cache does not identify a type from the bytes. That owner validates its schema,
producer, bounds, structure, and checksums. Native artifacts such as shader
SPIR-V and reflection sidecars may retain their own strict file grammar beneath
a namespaced subtree. Every DDC entry remains disposable and its authored inputs
remain authoritative.

`FCacheGetResult` and `FCachePutResult` are `std::expected` aliases with a shared
`FCacheError` carrying a backend-neutral code and diagnostic. Successful lookup
returns immutable `FSharedByteBuffer` bytes; successful put returns `void`.
Lookup failures distinguish a normal `Miss` from invalid input, size rejection,
corruption and storage failure. Put reports rejection or storage failure.
Asset cache diagnostics retain only the error object. A `Miss` remains a normal
rebuild trigger, and cache failures can accompany a successful asset build, so
these failures do not become asset operation failures.

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

Family build keys use editor payload identity before requesting bytes, so a
validated DDC hit performs zero source-range reads. A miss captures one owned
immutable payload snapshot before worker execution.

DDC writes use Core's shared atomic byte-publication API: a fixed-length
same-directory temporary file is flushed and closed before replacement. The
temporary name is independent of the destination name, and DDC round trips are
supported beyond the traditional Windows `MAX_PATH` boundary under the
[physical file I/O contract](../Core/FileIO.md).
Owners validate reserved fields, versions, sizes, allocation limits, structure,
and checksums before publication. Texture families use the canonical PlatformData
serializer; other families use their registered functions. Invalid cache bytes are
rebuildable misses. A successful build remains usable when best-effort storage
fails; cache failure is independent of compilation disposition.

Engine's cache adapter retains failed read/write outcomes, logical key, requested
value bound, and separate cache-call timing. Each operation resets its previous
cause and stores no additional message. `FAssetCacheDiagnostic` adds Engine
classification and opaque identity without flattening the underlying result.
Texture codecs retain encode/decode classification and the complete Archive
failure; StaticMesh codecs retain family-owned causes.

StaticMesh synchronous and compilation results carry render/collision cache and
codec causes. All texture families retain read/write causes in
`FAssetCacheDiagnostics`; Texture2D transports these through worker results and
terminal callbacks, including success and application rejection. Bounded
formatters render diagnostics only at presentation.

TextureCube uses Engine-owned bucket `TextureCube/Objects`. Explicit import or
reimport decodes and projects a panorama into six canonical authored RGBA8
faces before the cache lookup. Engine derives the key from those faces and the
builder descriptor; only a miss invokes TextureBuild platform construction.
Ordinary build, PostLoad, DDC recovery, and Cook never recapture a physical source.
Texture producers validate complete platform data before calling the void
`SetPlatformData` ownership-transfer setter; the setter neither validates nor
updates render resources. `PostLoad()` logs initialization failures without
returning a result. Explicit `RebuildPlatformData()` reports success as a Bool
and logs its own errors; lower-level build and Cook APIs retain diagnostics
for operation callers. Failed builds preserve previously accepted data, while
an asset without accepted data remains safely unavailable to resource consumers.

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
provenance, `FTextureSource` bulk, offline build settings, and operation-owned
rebuild keys/diagnostics are omitted.

Payload-bearing packages use the placement contract in
[Package Bulk Data](BulkData.md#cooked-projection). Metadata-only packages,
whose fields stay inline have no companion
and no empty manifest record.

The implemented family projections are:

| Family | Cooked field | First runtime consumer |
| --- | --- | --- |
| Texture2D, TextureCube, VolumeTexture | `PlatformData` | texture resource upload |
| StaticMesh | `RenderData`, `CollisionData` | render and physics publication |
| Material | `ProgramData` | material render-layer publication |

The three texture families store their cooked `FBulkData` in one `DTexture`
slot, but their serializers continue to expose the stable concrete wire fields
`DTexture2D::PlatformData`, `DTextureCube::PlatformData`, and
`DVolumeTexture::PlatformData`. Each family retains its own typed installed data
and codec; common storage does not make those wire identities interchangeable.

Family serializers retain their existing versioned headers, target/profile
facts, bounds, checksums, and semantic validation. Cooked package load validates
the complete package/segment closure and attaches each external `FBulkData`
range before publishing the object graph. It performs no field-range read.
The first explicit family request locks only the required field, decodes into a
detached candidate, validates the family schema, installs typed platform data,
and invokes the common resource update.
Missing, truncated, corrupt, wrong-target, or incompatible data is an
asset-qualified hard failure; there is no source, importer, DDC, or synthetic
fallback.

### Cooked mesh runtime residency

`DStaticMesh` exposes side-effect-free RenderData
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

`EnsureRenderDataLoadedBlocking()` submits CPU-only work or joins an existing
request identity, waits for that owner's manager work, and uses the shared
synchronous fallback when necessary. Its result reports CPU residency only;
callers explicitly call `InitResources()` and query `GetRenderResourceStatus()`
for GPU readiness. Existing combined async requests still queue GPU initialization
when published. Blocking loading is not used by getters, component ticks,
SceneProxy creation, or renderer preparation. Explicit calls to
`EnsureRenderDataLoadedBlocking()` advance a failed/cancelled CPU generation and
repeat CPU loading; `InitResources()` also retries failed GPU initialization.
Ordinary `RequestRenderDataAndResources()` calls retain terminal failures and
cancellations, so polling does not repeatedly submit failed work. See
[Render Resource Lifecycle](../Rendering/RenderResourceLifecycle.md#cooked-mesh-readiness).

Manager shutdown stops admission, cancels reads and decode work, reports a
current cancellation terminal to live assets without publishing CPU or GPU
state, and drains tasks and completions before package, task, render, or Engine
lifetime ends. Reinitialization creates a new manager scope; a cancelled asset
resumes through an explicit `EnsureRenderDataLoadedBlocking()` call.
Package retirement, unload, destruction,
or a newer generation cannot publish stale candidates.

## Cook and Publication Rules

Production Cook runs offline through `DurinAssetTool cook` with stable authored
inputs. It prepares detached save plans and publishes outputs transactionally,
committing the manifest last. Contributor ownership, dependency discovery,
reuse, diagnostics, cancellation, and rollback are defined in
[Asset cooking and publication](Cooking.md).

## Compatibility, targets, and inspection

Package reader policy is defined by [Versioning](Versioning.md#authored-package-policy).

The implemented compatibility identifiers are Win64 platform `1`, Game
profile `1`, and EditorValidation profile `2`. Production family Cook and
runtime qualification currently select Win64/Game. Other target/profile pairs
are unsupported and fail explicitly rather than falling back or guessing.

Inspection and explicit repair ownership are defined
[below](#domain-qualified-inspection-and-repair-ownership).

## Versioning and naming

Package format, family schema, and producer-version policy are defined by
[Versioning](Versioning.md). Cook target/profile also qualifies production identity.

`.bin` is disposable content-addressed DDC storage. A cooked `.dbulk` is a
manifest-owned deployable raw package segment whose layout is authoritative only
through its `.dasset` manifest. The suffix does not promise one
operating-system file per package after future archive/store integration.

## Repository Policy

Git/LFS ownership and generated-directory rules are defined by
[Content Version Control](../../Development/VersionControl/ContentVersionControl.md).
Cooked output remains required for its staged build even when ignored by Git.

## Domain-qualified inspection and repair ownership

Texture payload inspection reports source, derived, cooked, decoded CPU, and
GPU stages without creating a shared authority descriptor. Construct-free
inspection reads package field trees and storage descriptors; live inspection
joins source metadata and placement, installed/cooked platform data, an
available manager-owned Texture2D operation diagnostic, and current render-
resource state. Placement labels are capability descriptions such as `SourceFile`,
`EditorPackageCompanion`, `DerivedDataCache`, and `CookedPackageCompanion`, not
backend paths supplied to domain callers.

StaticMeshEditor owns StaticMesh payload inspection and package-field
interpretation. Engine retains only const source, residency, resource and
BodySetup facts plus bounded manager observations. StaticMesh inspection never
opens bulk storage, acquires source handles, probes cache/build modules or creates
primitive geometry. Metadata presence is not physical validation. Operation
history is request-qualified and never presented as proof of current settings
or source/collision coherence. See the [StaticMesh Inspector](../../Editor/Guides/StaticMeshInspector.md)
for fields and explicit workflows.

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
- [Package Bulk Data](BulkData.md)
- [Asset Compilation](AssetCompilation.md)
- [Versioning](Versioning.md)
- [Texture System](../Rendering/TextureSystem.md)
- [Content Version Control](../../Development/VersionControl/ContentVersionControl.md)
