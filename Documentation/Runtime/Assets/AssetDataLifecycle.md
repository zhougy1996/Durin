# Asset Data Lifecycle and Storage

Summary: Define authored, derived, cooked, and runtime asset-data ownership and transitions.

Modules: Engine, RenderCore, DerivedDataCache, StaticMeshBuild, TextureBuild, AssetForgeBuiltins

Last reviewed: 2026-09-07

Durin separates asset identity, authoring input, rebuildable derived data, and
deployable runtime data. File suffixes describe those lifecycle contracts, not
merely whether a file contains binary bytes.

## Serialization and production ownership

Persistent values use the common archive protocol rather than paired
direction-named codecs. Runtime `Engine` values own their bidirectional
`Serialize(FArchive&)` field order and validation for DDC and cooked payloads;
Developer `TextureBuild` and `StaticMeshBuild` own normalized
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

StaticMeshBuild registers one render/collision provider and TextureBuild three
providers. These providers use bounded typed modular-feature invocation.
Providers own recipe metrics and producer versions.
Engine owns keys, runtime serialization, DDC policy, and object application;
providers retain no cache keys, origin, persistence diagnostics, or live assets.

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

## Runtime Data Domain

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

StaticMesh separates persistent `FStaticMeshImportedData` from detached
`FStaticMeshDecodedGeometry`. `Initialize` validates a complete replacement;
`AcquireGeometry` returns a shared const handle that survives source replacement,
release and asset destruction. `ReleaseGeometry` drops only that source value's
decoded ownership. Asset publication releases decoded residency; detached build
requests release it with their operation lifetime. Explicit readers and source
copies may retain it longer. Canonical unsaved bulk remains owned throughout.
The source and codec contracts are detailed in
[StaticMesh rendering](../Rendering/StaticMeshRendering.md#source-and-payload-compatibility).

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
`IStaticMeshBuildProvider`,
`ITexture2DBuildProvider`, `IVolumeTextureBuildProvider`,
and `ITextureCubeBuildProvider`.
Runtime consumers invoke exactly one provider through a bounded modular-feature
visitor. No provider reference or provider-authored callable escapes that
visitor; zero providers is an explicit unavailable result and multiple
providers is an explicit ambiguity rather than registration-order selection.

`StaticMeshBuild` owns only detached render/collision recipes. Engine owns its
PostLoad scheduling, import/Scene build, cache lookup/validation/fallback, and result
application. Authored PostLoad returns after metadata admission; source decoding,
render/ray and collision construction run in the typed worker domain. Cook
constructs a detached projection, even when authored CPU data is absent, without
changing authored bytes, source residency, render revision or dirty state. Only
pending source mutations require a selected wait before cook capture. See
[Asset Compilation](AssetCompilation.md#staticmesh-completion) for bounds,
publication and observational diagnostics. StaticMesh keys are editor-only Engine-private values; operation
results carry key, origin, descriptor, timings, payload bytes, and bounded
persistence diagnostics without copying them onto `DStaticMesh` or `DBodySetup`.
Metadata-only warm loads do not read authored geometry; a miss acquires an
immutable decoded geometry handle before calling the recipe. Fresh source
initialization encodes once and seeds the same handle without a decode round trip.
The authored replacement is passed separately to application; build results do
not own source storage. `SetImportedRenderData` and `SetRenderData`
validate candidates before rollback-safe resource replacement; Engine
application separately decides dirtying and material-slot upgrade notification.
Cook reports existing payload capture rather than inferring an old build origin
from the asset.
`AssetForgeBuiltins` owns only explicit import/reimport providers and editor
save-readiness policy; Engine, Build, and Cook consumers do not acquire an
importer dependency. Each build module instance owns its provider objects and
generation-bound registration tokens, so owner retirement rejects new calls
and waits for admitted visitors before provider state is destroyed.

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
Owners validate reserved fields, versions, declared sizes, allocation limits,
structural invariants, and checksums before publishing data. A cache write failure does not
invalidate a complete in-memory build result.

For Texture2D/TextureCube/VolumeTexture, Engine validates cached PlatformData
through the canonical serializer; other build families validate through their
registered family functions. Invalid bytes become rebuildable misses from
the persistent common texture source. Every family retains a complete local result
after a successful build even when best-effort DDC storage fails, and surfaces
the bounded store diagnostic separately.

TextureCube uses Engine-owned bucket `TextureCube/Objects`. Explicit import or
reimport decodes and projects a panorama into six canonical authored RGBA8
faces before the cache lookup. Engine derives the key from those faces and the
provider descriptor; only a miss invokes TextureBuild platform construction.
Ordinary build, PostLoad, DDC recovery, and Cook never recapture a physical source.

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
| EnvironmentLighting | `PlatformData` | lighting resource upload |

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

`FCookContext::AddPackage` accepts canonical cooked package bytes; package
serialization captures any `FBulkData` ranges and produces the optional raw
segment. `AddRawPackage` admits an already laid-out opaque segment. Asset families
contribute through class-keyed registrations owned by `RegisterEngineCookContributors`; individual family
Cook methods are private.

CMNF entries are sorted by normalized cook-relative path and record kind, required
flag, byte extent, and XXH3-128 digest. Raw companions use `PackageBulk`;
package-only output has no companion entry.

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
loaded and captured serially in canonical order. Class-keyed
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
order. Unchanged validated plans preserve bytes and timestamps.
Only after manifest publication may paths owned solely by the previous
manifest be removed; unowned files are never cleanup candidates.

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
opens bulk storage, acquires source handles, probes cache/providers or creates
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
