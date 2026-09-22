# Static Mesh Source and Building

Summary: Define StaticMesh source ownership, detached builds, payload validation, and compatibility.

Modules: Engine

Last reviewed: 2026-09-22

## Source ownership and publication

Source, recipe, application, payload, key, and derived-build APIs return
`std::expected<T, E>` directly, with operation-owned failure context and no
redundant success flag. Commands and conversions retaining caller-owned outputs
return `std::expected<void, E>` and preserve their documented output behavior.
Source acquisition and key factories return their successful values by value.
Read `error()` only on failure; cancellation remains a distinct error code.
Low-level operations retain their own structured context. The authored build boundary
keeps codes needed by compilation control flow. The synchronous object-facing
boundary exposes only owned diagnostic strings, so callers
need not understand build stages or the lower-level error tree.

`StaticMeshSource.h/.cpp` owns canonical source storage and its codec;
`StaticMeshGeometry.h` defines detached decoded sections and material mappings.
The reflected `FStaticMeshSource` retains `Geometry: FEditorBulkData`,
`MaterialSlotCount: uint32` and `MeshCount: uint32`.
The owning `DStaticMesh::Source` remains EditorOnly. Authored packages use
`FStaticMeshSourceVersion`; see [source versioning](Versioning.md#static-mesh-source-versions).
The independent geometry bulk codec uses `StaticMeshSourceGeometryPayloadVersion`.
Version 1 canonical bytes,
XXH3-128 content hashing, source identity and DDC keys are unchanged.
Reflection legacy names accept the former source type and owner field when
loading authored packages; new saves use FStaticMeshSource and Source.

`Initialize` validates complete geometry before installing canonical bytes and
seeding one immutable `FStaticMeshGeometryReadHandle`. It checks source mapping,
indices, finite positions, optional channel lengths, bounded names/counts and
aggregate canonical size before byte allocation. `AcquireGeometry` first uses
resident geometry; otherwise it reads canonical bulk, validates bounds before
array allocation, checks archive end and reflected counts, and publishes only a
complete decoded value. Read/archive failures retain their diagnostics and are
not cached. `IsValid`, `GetIdentity` and metadata access do no payload I/O.

A source has its own residency mutex and shared const geometry reference.
Concurrent acquire, release and copying a stable source are supported; copies
share existing immutable allocations but release independently. A copy made
before acquisition has its own lazy cache. `ReleaseGeometry` drops source-owned
decoded retention, while outstanding handles remain readable. Complete source
mutation, reflection loading and asset application require exclusive owner
access; shared decoded values must never be edited in place. A reflected change
in identity prevents reuse of the previous cache. Existing cooked-load, GPU and
collision revisions continue to qualify runtime state independently.

Fresh standalone/Scene import initializes source once before Engine DDC lookup.
Initialization returns `std::expected<void, FStaticMeshSourceError>`, retaining owned validation,
Archive encoding and Bulk-update errors.
Rejection preserves the source identity, canonical bytes and existing readers.
Recipes receive only an owning decoded handle and recipe settings. Provider feature
version 9 returns render products as `std::expected<Product, FStaticMeshRecipeError>`.
Errors own mesh/section identity, rejected indices/values, budget facts and
cancellation. Physics cooking has no registered recipe provider. Failed or canceled recipes return no product. Derived-data orchestration translates recipe failures once into a bounded pipeline failure, preserving cancellation.
A warm hit
uses source identity even with unreadable canonical bulk; a miss acquires geometry.
`FStaticMeshBuilder::Build` constructs owned, validated render data, including
bounds and optional ray acceleration. `CommitStaticMeshBuild` consumes that prepared
render data on the owner thread after checking the captured source, normalization,
material bindings and cancellation state. It does no bounds, ray or collision
construction. Resource initialization precedes live mutation. Source, slots,
render and prevalidated import provenance become current within one consumer
refresh boundary. A changed accepted source or normalization invalidates derived
collision there; rebuilding the same accepted geometry preserves physics resources.
`PublishStaticMeshRenderData` is a separate render-only entry point for a finalized
projection of the current accepted geometry. It does not change source, BodySetup,
collision readiness, or collision scheduling.

Build consumes fixed material-slot definitions and never reconciles, appends,
renames or retires asset slots. Import/reimport owns that policy through
`ReconcileStaticMeshMaterialSlots`, preserving matched bindings and stable positions.
`PreparedMaterialSlots` remains operation-owned until successful render publication.
Ordinary rebuilds retain existing slots. Render failure preserves previous state;
the private destructive test replacement retains its explicit destructive behavior.

Authored PostLoad, ordinary Build/AsyncBuild, standalone import/reimport and Scene
import complete at asset commit. The transaction schedules missing collision as an
independent operation, with its own readiness and error. Collision failure never
rolls back accepted source, slots, provenance or render data and never changes a
successful render completion into a failure. Cook independently builds detached
render and required collision projections; missing required collision fails cook.
Cooked decoding validates both existing payloads and prepares bounds/ray data
before installing them within one consumer refresh boundary, without editor recipes.
Bounds/getters/scene preparation never finish authored work.

Preview/components retain the previous accepted mesh until publication. An
initially empty preview waits for CPU data; thumbnail readiness returns pending
while authored compilation is active, then requests resources through the
independent GPU path. Inspector and material preview report pending work without
a build barrier. Closing an Inspector does not cancel background compilation.
The manager's bounded read-only observations are documented in
[Asset Compilation](AssetCompilation.md#staticmesh-completion).

Assets release decoded residency at
publication; operation-owned copies expire at operation completion. Explicit
source readers may cache until `ReleaseGeometry`. Neither release nor successful
publication removes unsaved authoritative bulk bytes. Cooked projection strips
source, and cooked loading uses neither source acquisition nor a build provider.

## Payload results and cancellation

Source acquisition returns `std::expected<FStaticMeshGeometryReadHandle, FStaticMeshSourceError>`, retaining resource-read causes, Archive code/path and owned validation
counts, mesh/field identity and rejected values. It supports a borrowed cancellation
predicate under its residency lock; the predicate must not reenter that source. A canceled decode never publishes
partial residency. Ray construction supports borrowed cancellation through its
triangle/bounds loops and sort/partition work. Null optional ray acceleration
retains exact reference traversal. Render/collision payload conversion, encoding,
decoding and validation also accept borrowed predicates and check at most every
256 scalar/record work units. Collision payload extraction and reconstruction
return `std::expected<void, FPhysicsCollisionPayloadError>`, owning geometry/mode, counts, rejected
indices/ordinals and vertex context. Construction latches cancellation across
the physics builder so it cannot become a topology rejection. Both APIs
preserve output on failure. Archive/provider adapters format explicitly; CookedMesh product errors retain
`CollisionCause`. Render conversion returns `std::expected<void, FStaticMeshPayloadError>` with owned
LOD/stream/section indices, rejected attribute values, bounds and actual/expected
counts or ranges. It preserves outputs on rejection/cancellation; CookedMesh product errors retain `RenderCause`.
Cooked product decoding returns `FCookedMeshProductResult`, preserving Archive
code/path/byte position, metadata differences and both construction causes; it
publishes only a complete candidate. Async workers retain `ProductCause`.
Ordinary payload Archive loading instead fills an unpublished
destination in place: cancellation reports an error and the caller discards the
incomplete value. Successful loading clears obsolete optional UV/color streams
and collision leaf data. The authored wrapper latches cancellation, so an interrupted cache decode
cannot fall through to a recipe or become a successful cache hit. The cancellable
bounds overload is for detached construction only: false can leave partial bounds,
and the caller must discard that candidate. No callback survives its synchronous
call. Contiguous allocation/copy, container hashing/packing, archive I/O and
compression remain indivisible library calls; cooperative cancellation has no
hard wall-clock deadline and includes scratch destruction before return.

## Error-handling boundaries

An interface returns failure when its caller must stop, recover, or report an
unsatisfied operation. Intermediate layers preserve that failure instead of
inventing another wrapper. Diagnostic detail alone does not require a new type.

| Boundary | Caller responsibility |
| --- | --- |
| Source acquisition, collision input preparation, recipe/build/cook | Stop on invalid or unavailable input; preserve the original reason and cancellation. |
| Payload validation and cooked loading | Reject malformed data; keep structured validation details available to non-cache callers. |
| Cache read/decode/write | Rebuild rejected cache entries; report recoverable warnings without failing a valid product on cache write failure. |
| Async admission | Report whether work was accepted; accepted work has a separate completion. |
| Async completion | Distinguish success, failure, cancellation and supersession; ordinary failure carries its cause. |
| Physics installation | Reject stale requests as Superseded without mutation; retain an Installation failure for incompatible current geometry. |
| Queries, capture, invalidation and cancellation | Use values or lifecycle operations; do not add nested failure objects. |

Render finalization and public publication still validate externally supplied data.
Their failures are necessary even when the normal builder already validates its own
product. Waiting for a task is a barrier; inspect its completion or owner state for
the build outcome.

## Provenance and compatibility

StaticMesh source provenance stores one normalized project-relative or external
absolute filename plus the exact source hash, Assimp importer version, and
import axes. Source organization is independent of the StaticMesh package
path. Reimport reads the persisted file without copying, replacing, relocating,
or deleting it. Legacy package-relative source fields are rejected. The
canonical DDC key also includes builder version 4, render-payload schema 5, and target
platform. Render/collision key factories return typed key or byte results,
retaining rejected target and Archive code/path. Failed results contain no key
or partial bytes; provider
adapters format explicitly. Private cache codecs return a rejection message to the
cache boundary, where decode rejection triggers rebuilding. Public payload validators
retain typed failures for cooked loading and other non-cache callers. Public derived-data builds return
`std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>` or
`std::expected<FPhysicsCookResult, FPhysicsCookFailure>`.
Render data is returned with unique ownership; failure and cancellation return no product.
Render construction, application and resource publication use
`FStaticMeshBuildFailure`: a diagnostic stage, owned text capped at 4096 bytes,
and `IsCancelled()` for internal control flow. Lower-level source, recipe,
codec and validation errors are formatted at the pipeline boundary; the failure
then propagates unchanged through candidate construction and completion diagnostics.
There is no per-layer error enumeration or nested completion cause tree.
Physics preparation, cooking and installation use `FPhysicsCookFailure` independently.
Both pipelines report recoverable cache problems through `FAssetBuildCacheWarning`,
with operation and bounded text. Clean hits and misses produce no warning records.
Cache read/decode failures rebuild from source; write failures do not invalidate an
in-memory product. Invalid source, recipe or payload data still fails validation.
Queries, cancellation, invalidation and snapshot capture do not introduce another
error domain. Async admission and eventual completion are separate contracts;
cancellation and supersession are completion states, not ordinary build failures.
`StaticMeshBuilder.h` is the advanced detached building API. `FStaticMeshBuilder::Build`
returns validated render data with bounds and ray-query acceleration;
`FPhysicsCookHelper::Cook` returns collision geometry from owned LOD0
positions/indices copied into a detached value snapshot, with per-request
settings captured independently of render ownership. Workers read the snapshot
without mutating it; moving a request transfers its arrays without copying.
`Capture` records owner facts for render publication. Collision capture copies
LOD0 streams only for source-less procedural/debug meshes. Authored collision uses
`IInterface_CollisionDataProvider::CreatePhysicsMeshInputTask`: its typed task acquires canonical source
and creates normalized positions/indices on the worker without RenderData or RHI.
Both projections use `GetStaticMeshPositionNormalization` to preserve identical
coordinates. No public combined render/collision build product exists.
Render output owns CPU geometry and the section-to-slot mapping. Asset slot
definitions are inputs, not build outputs. Provider registration remains an
internal guard for render operations only. Physics Cook calls the linked PhysicsCore
geometry builders directly, without a registered StaticMesh provider. Cache warnings are collected
separately; render results carry no cache-origin, key or timing observation.
`DStaticMesh::Build` returns `std::expected<void, std::vector<std::string>>`
after synchronous construction and application. Its render work does not submit, join, wait for or create a render diagnostic
record in the compiling manager; the asset commit schedules missing collision. Valid source input
cancels older asynchronous requests for the same mesh without waiting or pumping
their callbacks. Failed construction/application preserves live mesh data;
success marks the package dirty. Nonfatal cache failures are logged here.
Caller-facing errors are plain string arrays; `FormatStaticMeshBuildMessages`
joins them for display within a 4096-byte budget. The decoded-geometry overload
first validates and captures canonical source.
`DStaticMesh::AsyncBuild` returns the same expected/string-array shape for
admission only. Accepted requests deliver `FStaticMeshCompilationResult` with
request ID, terminal `Status` and an `Errors` string array. Cancellation and
supersession are completion states, not public error codes. Detailed diagnostics
remain a separate query. Import saving updates the import completion result,
not compilation history. Compilation, cooked residency and Level mutation
reports retain independent lifecycle and partial-effect states.
These observations do not change cache fallback or publication. A valid warm DDC object can load from persisted identity while source
and Assimp are unavailable.

The schema-5 payload is little-endian and checksummed, with bounded chunks for
bounds, material-slot count, per-LOD geometry and screen-size policy, sections,
vertex streams, and indices. Readers validate counts, ranges, numeric data, and
indices, and skip only optional unknown chunks. Cook strips source/import
metadata and uses the independent lazy bulk fields described above.

Each render/collision value owns one bidirectional Archive schema. Input regions
are borrowed from the owning DDC buffer or BulkData lease for synchronous decode;
the caller checks complete consumption before publication. Render chunk sizes
and cumulative native vector storage are bounded before stream allocation.
DCOL validates disjoint ranges, checked element counts and native storage before
resizing. Offset tables and body hashes retain bounded output staging; collision
save additionally orders indices by leaf ordinal in local scratch without
changing the source. None of these scratch values owns live publication.

StaticMesh render schema 5 stores each LOD's `ScreenSize` beside its geometry and retains
the schema-3 bounded material-slot count rather than slot GUIDs.
Every decoded section index is validated against that count; package metadata
then restores editor/runtime slot names and imported source indices by stable
position. Schema 4 and older payloads are incompatible, and builder version 4
invalidates prior derived data. Current render key schema 4 and collision key
schema 3 encode the applicable builder and payload version values. Source-backed assets and stale
DDC entries rebuild; cooked/runtime-only schema-4-or-older content must be recooked and
is never silently reinterpreted. Encode reads semantic data back from the named buffer resources;
decode constructs them from the payload's position, normal, tangent, UV,
color, index, and LOD-policy data. Decode and render-data reconstruction publish
only after the complete policy and geometry validate.

CPU storage is retained while editor and test consumers inspect LOD data.
`NeedsCPUAccess` is the explicit policy for a future discard path; upload
currently retains these arrays.

## Related documentation

- [Static mesh rendering](../Rendering/StaticMeshRendering.md)
- [Asset data lifecycle](AssetDataLifecycle.md)
- [Asset compilation](AssetCompilation.md#staticmesh-completion)
