# Static Mesh Source and Building

Summary: Define StaticMesh source ownership, detached builds, payload validation, and compatibility.

Modules: Engine

Last reviewed: 2026-10-10

## Source ownership and publication

Source, application, payload, key, and derived-build APIs return
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
`MeshDescription/MeshDescription.h` defines detached triangle topology and material
groups. `FStaticMeshAttributes` and `FStaticMeshConstAttributes` are borrowed
views of the fixed static-mesh attribute schema. Sections own geometric positions,
optional vertex-instance-to-vertex mappings and triangle instance indices. Normals,
tangents, UVs and colors are per instance, allowing seams at shared positions.
An empty mapping is the compact one-instance-per-vertex representation. This
section-local triangle model does not implement UE edge or arbitrary polygon APIs.
The reflected `FStaticMeshSource` retains `Geometry: FEditorBulkData`,
`MaterialSlotCount: uint32` and `MeshCount: uint32`.
The owning `DStaticMesh::Source` remains EditorOnly. Authored packages use
`FStaticMeshSourceVersion`; see [source versioning](Versioning.md#static-mesh-source-versions).
The independent geometry bulk codec uses `StaticMeshSourceGeometryPayloadVersion`.
Compact descriptions retain version-1 canonical bytes, XXH3-128 hashing and source
identity. Version 2 appends the explicit instance mapping to each section; readers
accept both formats. The identity envelope remains version 1, with the bulk hash
distinguishing the extended topology. Render expands instances into render vertices;
collision maps triangle instances back to geometric vertices. Render borrows
section attributes and indices, allocating only expanded positions before
constructing output streams. Both use all source
positions for identical normalization. Derived keys use the
[shared build-action schema](DerivedDataBuild.md); render and collision have
independent identities and separate registered shared-output sessions. Collision
capture owns prepared arrays and computes identity before metadata-only lookup;
PhysicsCore cooking completes immutable arrays before geometry publication.
Authored packages use the reflected identities `FStaticMeshSource` and `Source`.
The former source type and owner-field aliases have been retired after corpus
canonicalization.

`Initialize` validates complete geometry before installing canonical bytes and
seeding one immutable `FMeshDescriptionReadHandle`. It checks source mapping,
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
The module-private Developer `FStaticMeshBuilder::Build` receives
`FStaticMeshBuildParameters` with an owning decoded handle, fixed material slots,
normalization and borrowed cancellation/memory controls.
`IMeshBuilderModule::BuildRender(FStaticMeshRenderData& OutRenderData,
const FStaticMeshBuildParameters& Parameters)` synchronously writes detached CPU
render data and returns `bool`. The output must have no initialized GPU resources
or vertex factories; failure or cancellation preserves its previous CPU data.
The builder moves CPU streams into uninitialized vertex/index buffers. Engine
freezes those allocations into shared DDC outputs without copying; GPU resource
initialization and material object binding remain with Engine.
MeshBuilder logs construction failures with mesh/section identity, rejected
indices/values and budget facts directly at the failure site, without an error
object crossing the module contract. Cooperative cancellation checks stop
construction without an error log. Physics cooking is independent of the render build module. Derived-data
orchestration checks its latched cancellation state before interpreting `false`
as a generic construction failure and never publishes canceled output.
A warm hit
uses source identity even with unreadable canonical bulk; a miss resolves the
captured canonical bytes and decodes them inside the registered function.
`BuildStaticMeshRenderData` constructs owned, validated render data, including
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

Authored PostLoad, ordinary Build/BuildFromSource, standalone import/reimport and Scene
import complete at asset commit. The transaction schedules missing collision as an
independent operation, with its own readiness and error. Collision failure never
rolls back accepted source, slots, provenance or render data and never changes a
successful render completion into a failure. Cook independently builds detached
render and required collision projections; missing required collision fails cook.
Cooked decoding validates both existing payloads and prepares bounds/ray data
before installing them within one consumer refresh boundary, without editor construction.
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
source, and cooked loading uses neither source acquisition nor a build module.

Worker requests own `FStaticMeshBuildSettings`: material name/source/index values
and normalization size. `MakeStaticMeshBuildSettings` captures these once from
owner-side material slots. Publication reconciliation snapshots retain live
material bindings and source identity separately; neither enters the worker
settings. The DDC resolver copies already captured slot values without converting
owner descriptors again. Shared-output construction has one always-validating
entrypoint; assembly and finalization retain their existing validation boundaries.

## Payload results and cancellation

Source acquisition returns `std::expected<FMeshDescriptionReadHandle, FStaticMeshSourceError>`, retaining resource-read causes, Archive code/path and owned validation
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
preserve output on failure. Archive/build adapters format explicitly; CookedMesh product errors retain
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
cannot fall through to construction or become a successful cache hit. The cancellable
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
| Source acquisition, collision input preparation, build/cook | Stop on invalid or unavailable input; preserve the original reason and cancellation. |
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
render DDC action uses canonical schema 2, adapter function version 1, constants schema
2, and shared-output schema 2. Its named `BuilderVersion` constant is the nonzero
opaque `uint64` returned by `IMeshBuilderModule::GetBuildVersion()`. The built-in
module hashes a stable implementation identifier, `StaticMeshBuilderVersion`, and GLM version
using canonical binary encoding and XXH3-64; future output-affecting library
versions belong in that composition. The identity is immutable for the editor
lifetime and is compared for equality, never ordered. Adapter logic changes bump
`StaticMeshRenderBuildFunctionVersion` independently. Material-slot count and
target are constants, with separate source/reconciliation input identities.
Before invoking the builder, the adapter verifies its identity against the action.
Package/Cook render payloads retain schema 5. Format compatibility depends only on
`StaticMeshPayloadSchemaVersion`; the historical producer word is ignored when
reading and written as zero. Algorithm changes therefore invalidate DDC entries
without rejecting compatible cooked payloads. StaticMesh cook recipe dependencies
encode the render builder identity, adapter function version, shared-output schema,
Payload schema, and `PhysicsCookBuilderVersion`.
The algorithm revision header lives in MeshBuilder/Private/StaticMesh; Engine
consumers use only the module identity contract. StaticMesh production submits
definitions through the shared DDC session; standalone render key factories and
their input/error structures live only in native test support. Collision key
factories return typed results. Failures retain rejected targets and codec
diagnostics. Failed results contain no key
or partial bytes; execution boundaries format explicitly. Private cache codecs return a rejection message to the
cache boundary, where decode rejection triggers rebuilding. Public payload validators
retain typed failures for cooked loading and other non-cache callers. Public derived-data builds return
`std::expected<std::unique_ptr<FStaticMeshRenderData>, FStaticMeshBuildFailure>` or
`std::expected<FPhysicsCookResult, FPhysicsCookFailure>`.
Render data is returned with unique ownership; failure and cancellation return no product.
Render construction, application and resource publication use
`FStaticMeshBuildFailure`: a diagnostic stage, owned text capped at 4096 bytes,
and `IsCancelled()` for internal control flow. Lower-level source, construction,
codec and validation errors are formatted at the pipeline boundary; the failure
then propagates unchanged through candidate construction and completion diagnostics.
There is no per-layer error enumeration or nested completion cause tree.
Physics preparation, cooking and installation use `FPhysicsCookFailure` independently.
Both pipelines log recoverable cache problems once at execution, with function,
key, operation and bounded original cause. Results and completion records carry no
cache warning lists. Clean hits and misses produce no warning records.
Cache read/decode failures rebuild from source; write failures do not invalidate an
in-memory product. Invalid source, construction or payload data still fails validation.
Queries, cancellation, invalidation and snapshot capture do not introduce another
error domain. Async admission and eventual completion are separate contracts;
cancellation and supersession are completion states, not ordinary build failures.
`StaticMeshBuild.h` is the advanced detached Engine API. `BuildStaticMeshRenderData`
returns validated render data with bounds and ray-query acceleration;
`FPhysicsCookHelper::Cook` returns collision geometry from owned LOD0
positions/indices copied into a detached value snapshot, with per-request
settings captured independently of render ownership. Workers read the snapshot
without mutating it; moving a request transfers its arrays without copying.
`CaptureStaticMeshReconciliation` records owner facts for render publication. Collision capture copies
LOD0 streams only for source-less procedural/debug meshes. Authored collision uses
`IInterface_CollisionDataProvider::CreatePhysicsMeshInputTask`: its typed task acquires canonical source
and creates normalized positions/indices on the worker without RenderData or RHI.
Both projections use `GetStaticMeshPositionNormalization` to preserve identical
coordinates. No public combined render/collision build product exists.
Shared DDC render output owns CPU geometry and the section-to-slot mapping.
Asset slot definitions remain inputs and are not persisted in that output. `IMeshBuilderModule` is an Engine-declared
module interface, implemented by Developer/MeshBuilder without feature registration.
The interface and source-geometry recipe requests require
`DURIN_WITH_EDITORONLY_DATA`. Render-data finalization, publication, and their
failure types remain available for cooked runtime loading.
`MeshBuilder` remains loaded throughout the editor lifetime and does not support
runtime unloading or reloading. `IMeshBuilderModule::Get` borrows the active module;
`BuildStaticMeshRenderData` supports both synchronous and worker callers without a
session or code lease. A missing module rejects construction explicitly; cooked
loading does not require the builder. Consumers stop admission and drain work
before normal editor shutdown. Builder versions remain immutable for the editor
lifetime and contribute to DDC identity. Engine does not link
back to the Developer implementation. Physics Cook calls the linked PhysicsCore
geometry builders directly, independently of the render build module. Cache issues
are logged internally; render results carry no cache-origin, key or timing observation.
`DStaticMesh::Build(Mode, Options, Completion)` rebuilds the current accepted Source.
`EStaticMeshBuildMode` explicitly selects Synchronous or Asynchronous execution;
there is no implicit scheduling default. Both modes support cache persistence and
package-dirty options. Source and settings are captured on the owner thread.
The return type is `std::expected<void, std::vector<std::string>>`: synchronous
success means constructed and applied; asynchronous success means admitted.
Completion runs on the owner thread, before return for synchronous calls and
through the mailbox for asynchronous calls. Synchronous completion has request ID 0.
Synchronous render work neither submits nor pumps compilation records; commit
still schedules independent missing collision. Valid source input cancels older
requests without waiting for their callbacks. Failed construction/application
preserves live data. Errors are bounded owned strings.
`BuildFromSource(Mode, Request, Completion)` is the separate authoring transaction
for a candidate Source, prepared slots and provenance, installed only on success.
Geometry must first pass `FStaticMeshSource::Initialize(FMeshDescription)`;
`DStaticMesh` has no decoded-geometry Build overload.
Asynchronous Accepted requests deliver `FStaticMeshCompilationResult` with
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

Cooked render/collision payloads each own one bidirectional Archive schema.
Their input regions are borrowed from BulkData leases; the caller checks
complete consumption before publication. Render and collision DDC sessions use
their family-owned shared-output schemas and typed assembly. Render chunk sizes
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
invalidates prior derived data. The render and collision session actions encode shared-output schema 2
independently of their cooked payload schemas, together with their applicable
builder versions. Source-backed assets and stale
DDC entries rebuild; cooked/runtime-only schema-4-or-older content must be recooked and
is never silently reinterpreted. Encode reads semantic data back from the named buffer resources;
decode constructs them from the payload's position, normal, tangent, UV,
color, index, and LOD-policy data. Decode and render-data reconstruction publish
only after the complete policy and geometry validate.

CPU storage is retained while editor and test consumers inspect LOD data.
`FPositionVertexBuffer` exposes UE-style `Init(vertex count)`, `Init(const positions&)`,
`VertexPosition`, `GetVertexData`, and `GetAllowCPUAccess` interfaces. Borrowed
initialization copies; Durin also retains a moving `Init(positions&&)` overload
and shared-storage helpers for allocation-preserving build/DDC handoff. Mutable
position access detaches shared storage and requires an uninitialized resource.
Position buffers default to retaining CPU data. With `bNeedsCPUAccess=false`,
successful RHI creation releases both mutable and shared CPU storage after the
upload bytes have been snapshotted; failed creation retains them for retry.
Vertex count and stride remain available after discard. Shared initialization
accepts the same CPU-access policy, and releasing one owner does not invalidate
other holders. Recreating a discarded buffer requires supplying positions again;
CPU queries, deformation, bounds recalculation and payload encoding require retained data.
Position, normal, tangent, UV, color and index getters return borrowed read-only
spans. Render resources can retain validated native `FSharedByteBuffer` arrays;
these arrays preserve the recipe allocation rather than copying its contents.
Shared setters reject serialized bytes or a different native element type before
replacing storage. Mutable access explicitly detaches into an owned vector,
leaving other owners of the original block unchanged. Callers that need a
snapshot across rebuild or unload must copy the span or retain an owning block.

Finalization only materializes missing UV/color defaults and does not detach
already populated streams. RHI upload reads the active immutable or mutable
view. Working-set admission accounts for the full retained backing capacity,
including capacity outside a subview, rather than just its visible element count.
Finalization validates borrowed stream views through the same semantic checks as
the archive model, without copying a complete payload. Package payload conversion
remains an explicit copy into the archive model.
Tangent, UV, color and index buffers still retain CPU storage after upload;
position buffers apply the policy described above. Existing mesh construction
retains CPU positions by default for CPU consumers and resource recreation.

## Related documentation

- [Static mesh rendering](../Rendering/StaticMeshRendering.md)
- [Asset data lifecycle](AssetDataLifecycle.md)
- [Asset compilation](AssetCompilation.md#staticmesh-completion)
