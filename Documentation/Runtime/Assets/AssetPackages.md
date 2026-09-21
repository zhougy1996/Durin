# Asset Packages

Summary: Define asset identity, canonical DAST v10 packages, runtime residency, loading, and inspection.

Modules: AssetRegistry, Engine, CoreDObject, AssetMaintenance, AssetTools

Last reviewed: 2026-09-21

Durin object assets are stored as versioned `.dasset` packages. A package is a
residency and persistence container with zero or more independently addressable
top-level assets. Every direct persistent export is a top-level asset; its
descendants use the ordinary Outer hierarchy. Outer defines structural
containment and object paths, but is not by itself a GC strong reference.

## Public Capability Boundary

Persistent metadata consumers include `AssetRegistry/Catalog.h`,
`AssetRegistry/References.h`, and `AssetRegistry/Scan.h`. Engine exposes narrow
capabilities: `Asset.h` for resolution, residency, loading, and cooked payload
reads; `Asset/Mutation.h` for exact mutation; and
`Asset/PackageSerialization.h` or `Asset/PackageInspection.h` for package
serialization, saving, and construct-free inspection. Asset creation,
duplication, import, deletion operations, and editor persistence policy belong to `IAssetTools`;
Engine exposes no generic asset-construction operation. `AssetCook.h` owns Cook
reachability and publication. Developer `AssetMaintenance` owns project-wide
compatibility and canonical-resave batches.
`Asset/PackageRemoval.h` exposes batch residency release and checked catalog
removal; selection, confirmation, companions, and deletion callbacks belong to
`AssetTools/AssetDeletion.h` as defined by
[Asset Catalog And Mutation](AssetCatalogAndMutation.md#deletion-and-fix-up).

Ownership is deliberately one-way:

- `CoreDObject` owns format-neutral linker tables, canonical tagged values,
  production DAST v10 read/write, generic live graph capture, package persistence,
  and bounded validation. See [Package Persistence](../Core/PackagePersistence.md).
- `AssetRegistry` owns canonical mounted-file discovery, bounded front-matter
  reads, and immutable package metadata/dependency snapshots.
- `Engine` prepares asset-specific capture inputs, applies validated linker
  tables to unpublished object graphs, owns residency, and provides transient
  exact inspection and mutation tools. Engine contains no package-table or
  tagged-value wire parser.
- `AssetMaintenance` owns deterministic compatibility/resave plans,
  fingerprints, stale checks, reporting, and publication rollback. It is not
  linked into the game Runtime.
- Asset-family Runtime/Editor modules own canonical imported data, domain
  payload codecs, build/DDC recipes, save readiness, and Cook contributions.

## Paths And Mounts

Package identities are extensionless `FPackagePath` values such as
`/Engine/Materials/Default` or `/Game/Levels/TestLevel`. A top-level object adds
an explicit asset name, for example
`FTopLevelAssetPath("/Game/Levels/TestLevel.TestLevel")`; a complete object path
may append a relative Outer chain such as
`FObjectPath("/Game/Levels/TestLevel.TestLevel:Root.Component")`. The first
package-path segment must match a registered mount and resolves relative to
that mount's single content directory. Optional importer source hints use a
separate explicitly based physical-path contract and never resolve through
package mounts.

Ordinary `FPackagePath::TryCreate` remains mount-bound. Cook staging may use the
explicit `TryCreateProjectContent` factory for a canonical `/Game/...` target
before the fixed output mount exists; that exception admits no other namespace.

Path factories `TryCreate` and `TryCreateProjectContent` return `bool`.
Use `TryCreateWithDiagnostic` at input boundaries that need to report failure
details. Deferred project-content and subobject-composition factories expose only
success; they have no diagnostic consumer. `FPackagePath::IsValid(text)` is a
boolean predicate. Both factory forms share validation and preserve
the output on failure. Diagnostic factories and soft-reference
assignment return `FObjectOperationResult`.
Its `FObjectError` discriminates path and soft-object error codes; success is an
absent code. Failures own their subject and expected/actual identities, retain
component indices and byte limits where relevant, and preserve `EMountPathError`
for mount lookup failures. Failed factories do not publish output paths; failed
soft-reference assignment leaves authored identity and loaded-cache state intact.
`TrySetObject(nullptr)` remains a successful reset, while a null loaded-cache
assignment fails. `FormatObjectError` generates prose at UI, logging, command,
and adapters to framework interfaces that still require strings. Semantic tests
assert codes and context, not formatted text.

Package, asset, and subobject identities compare case-sensitively by canonical
UTF-8 spelling. Factories reject invalid separators, empty components, and
inputs beyond the frozen component or complete-path bounds.
The complete path stores only those two interned names and one optional dotted
subobject string, and subobject traversal uses non-owning component views.
Discovery rejects case-only duplicate logical package identities.

The immutable Core mount registry publishes `/Engine/` and `/Game/` plus
validated project-declared extension and external-source mounts. Typed
resolution distinguishes invalid paths, unknown mounts, unavailable content
directories, escapes, missing files, forbidden dependencies, and read-only
write policy. Existing paths and not-yet-created destinations are checked
against canonical content directories, including symbolic-link targets.

The physical package filename is the resolved virtual package path plus
`.dasset`. A nonempty external BulkData closure uses the stable sibling
`.dbulk`. The file does not select an asset: top-level names are serialized
explicitly, and inner objects append a colon plus their relative Outer chain,
for example `/Game/Objects/Test.Mesh:Root.Component`.

The mounted `FPackagePath` is part of package validation. DAST v10 includes that
identity in its canonical name table, and every header, complete-read,
inspection, mutation, relocation, Cook, and admission call supplies the exact
identity expected for the physical file. Moving a package therefore requires a
canonical rewrite preserving the source version; a caller cannot validate the same bytes under an
arbitrary path.

## Runtime Lifetime

`DPackage` is an Outer-less object-graph root. CoreDObject's global object and
package registration is the sole live residency lookup; Engine does not keep a
second resident-package map. `CreatePackage` detects path collisions through
that registration. New asset packages begin `NewlyCreated`; a successful first
save clears that state only after file and catalog publication.
`DPackage::IsDirty()` independently records unsaved contents.

Every ordinary asset package is `Standalone`, so GC retains registered
residency without a manual root. `DPackage` registers and strongly retains all
of its direct persistent exports; no distinguished `Asset` pointer exists.
Descendants remain alive through their top-level graph's actual GC strong
references, not merely through Outer. Compiled-in reflection metadata instead
uses rooted `/Cpp/<ModuleName>` packages and is never serialized as `.dasset`.

`LoadPackage(FPackagePath)` owns closure admission and residency.
`LoadObject(FObjectPath)` resolves an exact top-level asset or descendant,
follows asset-level redirects, loads the owning package, and selects the exact
object. No load API derives an asset name from a package leaf, and a catalog
miss never guesses a filename.

Synchronous package and object loads return `std::expected<DPackage*,
FAssetReadError>` and `std::expected<T*, FAssetReadError>` respectively. Typed
object calls specify `LoadObject<T>(Path)`. The returned pointer observes existing
package residency; it does not transfer ownership or promise rollback on failure.
`FAssetPackageLoadScope` uses the same value contract and records completed
dependencies even when the requested root fails. Its explicit `Release()` remains
the residency cleanup boundary.

The optional caller-owned `FAssetLoadReport*` remains available on either branch,
including mutation and canonicalization evidence emitted before a root failure.
Existing resident-hit and early-rejection report behavior is unchanged. Read
errors retain their category, diagnostic text, and available resource/storage or
soft-object validation causes. Inspect `error()` only after testing failure.
Codec status and pending-component dependency bindings remain separate from these
completed-load values because bindings may expose incomplete package skeletons.

`Asset/AsyncLoad.h` provides `LoadPackageAsync(FPackagePath)` and
`RequestAsyncLoad(FObjectPath/FTopLevelAssetPath)`. Both return a shared
`FAsyncLoadHandle`; submission, observation, cancellation, callbacks and handle
destruction belong to GameThread. Successful handles strongly retain their
package and selected object. Object readiness does not imply compilation or
render-resource readiness. Releasing all caller handles does not cancel a
queued request; use `Cancel()` to withdraw that consumer explicitly.
`IsPackageLoading` continues to describe incomplete live object construction;
use the handle state to observe a queued or reading async request. A synchronous
load may complete the same package while its async read is pending; publication
then reuses that resident package.

`GetResult()` requires `IsComplete()` and returns an expected `FAsyncLoadedAssets`
containing the selected package/object pointers, or a typed read error. Pending
and loading states have no terminal result. Cancellation supplies a terminal
`Cancelled` error while preserving the existing callback-suppression policy.

Requests are deferred, including resident hits and ordinary errors. Guarded or
shutdown admission instead returns a failed handle immediately without invoking
the callback. The engine loop pumps `ProcessAsyncLoading`; standalone tools and
tests must pump it explicitly. Completion callbacks run on a later pump after
input file leases have been released, may enqueue more requests, and never run
inside a recursive pump. `Cancel()` suppresses undelivered callbacks, including
for an already completed request, without cancelling other consumers of the
same package. Dropping a successful handle releases its strong references;
ordinary Standalone package residency still follows the unload contract below.

The initial implementation coalesces active requests for the same root package,
starts higher-priority pending requests first, and limits detached reads to two
closures with at most 256 MiB of main-package bytes each. BlockingIO tasks read
the catalog dependency closure under package file read leases. Read errors in
unused dependencies do not fail a root whose live serializer discards those
fields. Publication rejects a changed catalog revision with `StaleData`; callers
may retry. An actual dependency outside the captured closure also fails instead
of falling back to synchronous main-package file reading.

GameThread still performs codec decoding, object construction, value restoration,
bulk-resource registration/validation, and component validation/PostLoad through
the ordinary loader. These stages preserve cycle and rollback semantics but are
not yet interruptible. The default pump budget is 2 ms and one completion unit;
it is checked between units, not inside a package component or callback. This
API therefore removes main-package file reads from GameThread, but does not
promise a hard frame-time bound or eliminate synchronous work inside PostLoad.

`CancelAsyncLoading()` cancels pending requests and undelivered callbacks and
drains accepted reads; it is a blocking teardown/tool boundary, not a UI wait.
Launch calls it after consumer detachment and before UI/module and task teardown.
`ShutdownAssetManager()` also drains async loading before retiring package
resources; shutdown invalidates previously loaded objects as usual.

The internal DAST codec accepts dependency bindings for package and exact-object
resolution. Ordinary loading supplies loader-only bindings and transfers
completion to a scoped owner of incomplete packages. Explicit private policies
retain their own cleanup callback and operation guarantees; the codec requires
that callback when completion is not owned by the ordinary loader. Both bindings
use the supplied source exclusively. Direct ordinary linker application discards
its candidate on failure but retains independently completed dependencies.

A policy can enable `bRejectImplicitLiveLoads`. During synchronous graph
application on the calling thread, live package/object loads and non-null soft
resolution/loading then fail with `InUse` before resident lookup or file access.
This includes calls made by constructors, `PostLoad`, and policy callbacks.
The rejection remains recorded in enclosing guarded loads, so ignoring its
return value cannot publish a successful graph. Graph application checks the
record after construction and before returning success and uses policy rollback
on failure. Scope exit restores normal loading. This guard does not intercept
raw object lookup, direct file I/O, or work dispatched to another thread; it does
not pin provider code or establish a complete Cook input session.

Unload rejects newly created or dirty packages unless the caller explicitly
selects `DiscardUnsaved`. It retires `Standalone`, runs GC, and succeeds only
when no authored hard dependency or transient GC strong reference still owns
the graph. When a transient reference keeps the graph alive, Engine restores
`Standalone` and reports `InUse`. Failed load, deletion, and shutdown use
separate complete-graph retirement paths.

## Reference Model

Choose a reference by ownership and loading behavior:

| Type | Persistent meaning |
| --- | --- |
| Reflected `TObjectPtr<T>` | Hard package dependency. It loads and retains the target, participates in GC, blocks unload, and blocks deletion outside the deletion set. |
| Reflected `TWeakObjectPtr<T>` | Transient, non-owning handle to an already loaded object. It is omitted from authored and cooked packages. |
| Reflected `TSoftObjectPtr<T>` | Typed exact-object identity without eager loading or retention. It persists an authored `FObjectPath` and keeps only an invalidatable weak runtime cache. |
| `FPackagePath` or path string | Service-defined package identity whose owner specifies persistence, move, and load behavior explicitly. |

An invalid default `FObjectPath` is the null soft-reference value; non-null
values pass the ordinary exact-object path factory.
`ResolveSoftObject(...)` distinguishes `Null`, `NotLoaded`, and `Loaded`
without loading; `LoadSoftObject(...)` is the explicit typed load boundary.
Resolution returns an expected `TSoftObjectResolution<T>` with state, pointer,
resolved identity, and redirection flag. Allowed null and valid not-loaded
references are successful resolutions. Loading an allowed null reference returns
a successful null pointer. Failures carry identity/redirection diagnostics when
resolution established them; they do not contain a success-state payload.
Both follow catalog redirect resolution while preserving the authored soft path
for equality, hashing, ordering, and serialization. Cache population verifies
the exact resolved object path and expected class. `Get()` performs no lookup,
redirect resolution, or load, and returns only a live weak object from the
current global cache epoch.

Hard imports identify an exact top-level asset in another package. Soft
references serialize complete canonical object paths. AssetRegistry persists
package-level hard, soft, redirect, and searchable-name facts; exact
object/property/container occurrences are computed transiently by Engine only
for tools that need a concrete edit. See
[Asset Catalog And Mutation](AssetCatalogAndMutation.md).

## Canonical DAST Format

DAST has permanent format identity
`3c59d1a9-6ceb-4e4c-b059-452db0a5af56`, diagnostic name
`Durin.BinaryFormat.DAST`. The current wire version, supported readers, and
corpus-transition policy are defined by
[Versioning](Versioning.md#authored-package-policy).

`CoreDObject`'s `DObject/PackageFormat.h` is the sole code authority for the
DAST magic, identity, format and object-stream versions, supported-reader set,
and persisted-projection reader-policy fingerprint. AssetRegistry and Engine
consume those constants directly and publish no independent package-format
copy.

### Envelope And Sections

A v10 main image contains the 64-byte DURF v1 preamble, a 32-byte DAST format
header, and nine canonical 48-byte directory entries. The required contiguous
sections, in order, are:

1. Registry
2. Names
3. Imports
4. Exports
5. Types
6. Schemas
7. Values
8. Bulk Directory
9. Inline Bulk

Each directory entry declares one section kind, version, absolute offset,
extent, and XXH3-128 digest. Sections are contiguous, nonoverlapping, and
completely consume the declared file. Duplicate, missing, reordered, gapped,
overlapping, overflowing, trailing, or hash-mismatched sections are invalid.
The first section begins at byte 528.

The declared discovery header ends after Imports. Registry, Names, and Imports
are therefore wholly front-matter resident; ordinary discovery never reads
Exports, Types, Schemas, Values, or payload bytes. Default bounds are 16 MiB
for front matter, 1 GiB each for the main and external bulk images, 1,048,575
table entries or container elements, 1 MiB strings, and value depth 64.

Registry records the package identity and a canonical list of top-level asset
records. Each record binds an export id to `FTopLevelAssetPath`, class, and an
optional exact `FObjectPath` redirect destination. Shared sorted
hard/soft/searchable package facts, export count, and exact external-bulk extent
and digest remain package-level. No package class, redirect, or main-export id
exists at package level.

### Defaults And Version Policy

Ordinary `SavePackage(Package)` writes v10 deltas against paired class/default
subobject values. `SavePackage(Package, EAssetPackageSaveMode::Complete)` and
`FAssetPackageSerializationOptions::Mode` explicitly select complete snapshots.
Bundle saving propagates the same selection; cooked saves always emit complete
values. A failed delta plan reports its reason without silently changing modes.

Baseline tags, inherited Struct defaults, whole-container replacement, Forced
intent, and explicit hard references follow
[Default-relative logical planning](../Core/Serialization.md#default-relative-logical-planning).
The loader initializes unpublished delta objects from reflected defaults and
remaps template references; explicit complete exports skip that initialization.
Missing or incompatible constructor-created default children reject delta saves,
and `NoClassDefaultObject` classes require complete snapshots. Reference binding,
validation, and PostLoad still precede publication.

Native fields remain complete without a reflected copy contract. The intrinsic
DObject identity node has no authored values; material compatibility uses package
custom versions. Ordinary loading allocates no authored-override ledger; only
Forced boundaries restore replacement intent. Detached relocation and reference
rewrites use the same validated current-format closure.

### Linker Tables And Canonical Values

`DObject/PackageLinker.h` is the format-neutral contract. `FLinkerTables` owns
the package summary, canonical names, recursive serialized types, schemas,
custom versions, imports, exports, property tags, provenance, values, and BulkData
placement facts. `FPackageIndex` represents null/import/export identity without
exposing wire arithmetic.

`FLinkerTables::TryResolvePath` returns `FLinkerResult` with a typed cause,
requested/failed package indices, and the relevant table size. Detached
`ObjectPackage::BuildCanonicalMapKeyToken` returns `FCanonicalMapKeyResult`
with the failing kind, type parameter, value/count context, and an owned
outer-to-inner field/array route. Both derive success from their error and
publish output only on success. `FormatLinkerError` and
`FormatCanonicalMapKeyError` generate text at presentation or legacy adapters;
neither migrated API accepts an error output parameter.

The closed value domain covers scalar integers and floats, Bool, String, Name,
Guid, Enum, intrinsic math values, Struct, fixed and dynamic arrays, Map, hard
and soft references, bytes, and BulkData. Every property tag carries declaring
type, field name, recursive structural type, implicit/explicit/forced
provenance, and a detached value. Map entries are ordered by CoreDObject's
canonical logical key token; insertion order and hash-table history cannot
change package bytes. Floating NaNs use canonical bit patterns while signed
zero and infinities retain their exact bits.

The writer freezes all names, types, schemas, versions, package indices, Outer
topology, field identities, reference closure, and bulk placements before
emission, assigning stable one-based ids. Late discovery, duplicate identity, invalid topology, unsupported
value kinds, malformed UTF-8, noncanonical order, arithmetic overflow, or a
limit failure aborts without replacing either output. Identical logical input
and identity produce byte-identical main/bulk output.

`ReadPackageRegistry(...)` validates only the declared front matter and
physical main/bulk extents before publishing package-level metadata.
`ReadPackage(...)` validates the complete main image and exact external
segment into a detached linker model. It checks tables, indices, topology,
types, values, section and payload digests, range placement, complete
consumption, and byte-identical canonical re-emission before replacing its output.
Registry reads additionally validate the caller's mounted package identity. Both
readers publish owned results without retaining input spans. Neither constructs a
`DObject`, resolves dependencies, invokes callbacks, or writes files.

All six public freeze/write/read entry points return `FPackageWriterResult` or
`FPackageReaderResult`, with success derived from the typed failure category.
Each failure retains a specific reason and an owned logical path or subject. Nested envelope, Linker, canonical Map-key and
writer failures are formatted where they are produced, preserving their details
without retaining another operation result. A recorded inner value failure survives the outer
property decoder. `FormatPackageError` owns codec prose; Engine and Registry
adapters format explicitly while their outer result contracts remain separate.

### BulkData Closure

Bulk Directory binds each `BulkData` value to an Inline Bulk or external raw
`.dbulk` range; Registry binds the complete external segment. Placement,
alignment, padding, and digest rules are defined by
[Package Bulk Data](BulkData.md#dast-v10-authored-placement).
Payload bytes remain opaque to the package format; asset families own their
schemas, interpretation, and [data lifecycle](AssetDataLifecycle.md).

## Production Save And Load

Engine's save boundary walks each live object's ordinary
`DObject::Serialize(...)` Archive to discover and then capture the complete
effective graph. It resolves defaults and authored provenance, freezes object
and field manifests, converts every value into detached CoreDObject linker
tables, and calls `WritePackage`. Encoding never dereferences the live graph.
A graph, field, reference, version, or value first seen after discovery is a
save failure.

Save options select `Authored` or `Cooked`. Cooked capture carries the exact
target profile and omits `EditorOnly` fields unless diagnostic retention is
explicit. Owned per-save overrides may omit objects/properties or supply copied
replacement values without mutating live state; validation rejects foreign or
conflicting entries and hard references to omitted objects.

Synchronous, asynchronous, and batch saves share destination admission,
version validation, destination stamp capture, closure encoding,
and writer preparation. Synchronous, protected task, and batch saves use the
same per-package commit path. Batches stop at the first failure and never roll
back earlier successful packages. Saving validates the complete new
v10 closure, publishes owned payloads before the main image that binds them,
verifies the stable closure, publishes a revisioned Registry delta, then clears
Dirty/NewlyCreated for the saved revision. Failure before authored commit
restores the prior closure and leaves the package retryably Dirty. Registry
failure after authored commit keeps valid bytes, fences the affected path, and
returns `ContentCommittedProjectionPending` for reconciliation.

Admission-only `SavePackage(..., SAVE_Async)` instead writes final files directly,
without rollback. Its return value does not confirm persistence; partial writes
fence the projection and retain Dirty. See the authoritative
[asynchronous save contracts](AssetCatalogAndMutation.md#asynchronous-save-staging)
for typed protected tasks, resource retirement, completion and retries.

Each protected package writer assigns one unique save ID to its main/bulk staging and
backup siblings beside the destination files. Backups serve only that writer's
in-process rollback and are released by finalization. Save preparation does not
recover or delete files abandoned by earlier saves; those files do not reserve
paths for a new save. This is not persistent crash recovery across files.

A save owning a private package candidate can opt into
`bRollbackOnRegistryFailure`, restoring prior package/bulk bytes instead of
committing a projection-pending result. A `PreparedPublication` token admits only
one private package owned by that active object-graph operation; multi-package
saves with this token are rejected before writing. Scene import publishes one
package at a time in dependency order. Each package uses both options inside
its final persistence callback: successful disk and Registry publication is
immediately followed by the non-failing memory commit. Later failures preserve
previously published disk content and live objects. Other save callers retain
the ordinary policy above.

Load resolves the source-version policy, validates the main/bulk closure and
obtains detached linker tables. It constructs package/export skeletons, follows
hard dependencies, applies values through Archive and restores authored ledgers.
A single in-flight record owns each candidate and its resource registration;
strong pins retain objects even before restored fields reference them. The DFS
completion stack supplies ordering, not a second residency map.

Only internal dependency bindings can return skeletons. `FindResidentPackage`
and ordinary residency enumeration exclude in-flight records; public package and
object loads return `InUse` for incomplete packages. Raw Core object existence is
not an asset readiness guarantee. Phases are Constructing, Skeleton (dependency
and value restoration), ValuesRestored, Validated, PostLoading, then Ready or
Failed. Terminal records are removed; completed packages remain Standalone.

Hard-reference cycles form a strongly connected completion group. Every member
restores values before any group graph validation, and every member validates
before the first PostLoad. Dependencies complete before dependents; cyclic
members use DFS completion order, with reverse object order within each package.
All members become publicly ready together. Failure retires incomplete members
and their resources; independent completed dependencies and pre-existing residents
remain valid. Explicit load scopes also record successful dependencies of a
failed root and can release those exact generations normally. Forced GC remains
part of failed candidate retirement to support immediate same-path retry.

Constructors, serializers and graph validators must not publish candidate
references or perform external mutations requiring compensation. Serializers
modify their target and use the supplied reference bindings. Ordinary construction
and restoration guard public live operations; ignored guard failures still reject
the load. `ValidateLoadedObjectGraph` is read-only and runs on fully restored group
values, with cooked/private context. Family source, slot, material, ownership and
RoadNet data rejection occurs at this boundary before initialization notifications.

`PostLoad()` is a void, non-transactional initialization notification. Recoverable
load and publication-preparation gates precede it. Resource compilation/readiness
remains separate from object readiness. A cyclic peer's values are available, but
its PostLoad may not have run. Reentrant public loads may consume ready packages or
complete an independent component; a new component cannot refer back into one
already running PostLoad. Requests for that incomplete component fail `InUse`.
Callback exceptions produce a classified load failure, discard the incomplete
component and restore loader/report state. This cleanup does not compensate
notifications, compilation requests or other external effects already executed.

Internal references use export indices. Cross-package hard imports target an
exact top-level asset; cycles work because skeletons exist before values are
applied. Missing fields keep constructor defaults. Authored fields absent from
a known declaring type are discarded, including fields in nested structs and
containers; dependencies referenced only by discarded fields are not loaded.
A current property or explicit historical route is never treated as removed
when its type is incompatible. Unknown classes or declaring types,
incompatible recursive types, duplicate Map keys, malformed references,
serializer callback rejection, or unavailable loading operations fail the complete load rather
than partially publishing state.

The Engine-private load Archive receives an explicit
`FPackageLoadBindings`: a retained bulk resource and an external-object
resolver. The loader creates these bindings once per package and shares them
across all object reads, for both authored and cooked data. The Archive never
looks up either binding in the live registries itself.
Missing bindings and external resolvers returning no object fail the read;
internal references still use the supplied export table. The ordinary linker
supplies its registered resource and normal load resolver. A private caller can
supply an immutable resource snapshot and private object resolver without
temporarily publishing either. `LoadAuthoredObject` applies serializer fields
only; the caller owns graph construction, dependency lifetime, rollback and
PostLoad policy. It returns a classified, owned diagnostic in `FAssetReadResult`;
resolver operation results are not retained inside load failures. Serializer
and struct migration callbacks still execute and
require their own admission before use in isolated preparation.

`PreparePackageGraphs` consumes saved immutable closures on GameThread and uses
those same canonicalization, schema, skeleton, field and authored-ledger phases.
It creates all private skeletons before applying any values, including default
inners, and resolves batch imports against their private package identities.
Only after every package in the batch has restored values does it invoke
`ValidateLoadedObjectGraph` on the candidate objects. Thus a validator can inspect
already-restored referenced objects from later packages in the selected batch.
Failure preserves the caller's previous output and never runs candidate PostLoad.
External imports bind to captured package identities. Missing dependencies require
an explicitly supplied, caller-owned `FAssetPackageLoadScope`; without it the call
fails without loading. Scoped loading requires resident replacement targets so a
recursive dependency load cannot publish one of those targets. The caller admits
ordinary external loading and PostLoad separately from candidate deserialization.
After these explicit loads finish, a live-operation guard covers candidate
construction, field/ledger restoration and final validation. Implicit live loads,
saves, unloads and mutation entry points reject the call; even an ignored rejection
invalidates the entire batch. Callback exceptions return InvalidClosure (allocation
failures return BudgetExceeded) after candidate ownership unwinds.
Graph owners retain exact strong references to their objects and external dependencies
across GC. Active load components and projection fences return Busy; preparation
rechecks fences after disk validation and never clears a fence.

The load scope records only exact package generations admitted by its calls,
including transitive dependencies. On failure the caller retains that scope, drops
candidate graphs, then calls `Release`; InUse retains the records for retry. Scope
destruction transfers residency and is not abort cleanup. Reload abort may pass
exact weak target identities to ignore those targets' saved dependency edges, which
may differ from their edited live references. Real live references and dirty/new
state still protect owned packages. Other packages' saved edges retain their normal
protection, and neither scope ownership nor the exception follows a same-path
replacement. Independent successful dependencies of a failed ordinary request
remain resident with their resources until normal unload or explicit scope release.

The caller must explicitly admit each export class's construction, serialization,
and graph-validation callbacks and hold path admission/edit/save leases. The result is `ValuesPrepared`:
ordinary PostLoad and runtime publication have not run, and loaded migration/version
metadata remains available for the owning resource preparation step. Preparation
revalidates saved closure digests before returning; failure preserves the output
and marks only newly created private hierarchies as garbage without running global
GC. Candidate package/object limits include default inners. The entire batch's
retained main/bulk bytes are checked with overflow-safe subtraction before parsing
or constructing any candidate; per-package storage limits cannot replace this
aggregate check. Additional dependency
load budgets and decoded/scratch/runtime byte accounting remain the caller's
responsibility. Graph ownership and destruction
stay on GameThread. This is an internal deserialization boundary, not a public
reload admission or success result.
`FPackageGraphPrepareResult` owns a typed reason, the affected package identity,
admission/budget context, and diagnostic text materialized at reader, resource,
graph-validation or asset boundaries. Failed preparation does not replace the
caller's prior candidate set. `FormatPackageGraphPrepareError` presents the owned
diagnostic without retaining candidate objects or complete asset operations.

## Saved Package Reload

`ReloadPackages` is the public GameThread recovery boundary for authored
Texture2D, VolumeTexture, Material, MaterialInstance, and material-function
interface implementations. Function candidates validate their bounded detached
closure before replacement; committed function replacements invalidate loaded
callers through the existing material edit scheduler. Requests are
deduplicated by resident package identity and bounded by package, object,
reference-slot, retained-CPU, and candidate-GPU limits. Newly created packages,
cooked runtimes, missing saved files, unregistered top-level classes, projection
fences, active loads, and non-idle native participants reject without clearing
Dirty or changing residency.

The coordinator snapshots and validates every saved main/bulk closure, admits
external dependencies through one exact-identity load scope, creates all private
graphs, runs family PostLoad/runtime preparation on those private objects, and
revalidates the complete disk digests. It then prepares one
`FObjectGraphReplacement` across the batch. Its final referencer scan and
registration/reference commit occur in one GameThread call, so a failure in a
later package cannot publish an earlier package. Strong references are rewritten,
weak handles retain generation safety, soft paths remain unchanged, and unrelated
referencer packages are not dirtied. `FObjectReplacementMap::Build` returns
`FObjectReplacementMapResult` with typed reasons, the failing package index,
budget counts, and owned object/type identities. Failed construction preserves
the previous mapping. The graph coordinator keeps its own failure code/reason
and formats lower-level failures before their temporary state is destroyed.

The returned operation remains `Pending` only while the committed old graph waits
for participant retirement. Callers keep it alive and use `Poll` or `Wait`; only a
successful retirement reports `Succeeded`. Prior to commit, cancellation and all
structured failures preserve the old graph. After commit, dependency-release
diagnostics do not masquerade as rollback because the new graph is already the
registered generation.

Reload diagnostics own typed reasons, stage/package/object context and text
captured at graph preparation, storage, path, replacement and asset boundaries. Function preparation retains all material diagnostics; failed material
compilation retains its status and complete diagnostic set. No candidate object
is retained merely to explain a failure. Editor consumers use
`FormatPackageReloadDiagnostic` at display time. The coordinator handles
candidate cleanup and post-commit dependency retention internally; callers do
not replay recovery based on diagnostic text. Resource receipts accept an owned
diagnostic only while pending, reject an empty failure, preserve the first
terminal transition, and return a locked copy through `GetFailure`.

## Construct-Free Inspection And Mutation

Engine inspection consumes validated linker tables and projects immutable
objects, fields, recursive values, hard/soft references, and BulkData storage
descriptors without constructing the inspected classes. It loads no dependency,
invokes no serializer or `PostLoad`, changes no dirty state, and never publishes
files. Texture and other asset-family inspectors add semantic interpretation
outside this package boundary.

Mutation tools use Registry package edges to select candidates, then open only
those requiring exact inspection. Rewrites operate on detached linkers, preserve
untouched values, rebuild Registry metadata, validate the exact output closure,
and enter bounded artifact publication. No persistent occurrence route,
display path, or legacy value cache exists.

Relocation, asset rename, Fix Up, and deletion policy belong to
[Asset Catalog And Mutation](AssetCatalogAndMutation.md). Cook reachability and
publication belong to [Asset Cooking and Publication](Cooking.md).

## Compatibility And Canonical Resave

Schema evolution follows the [Archive version policy](Versioning.md#archive-version-context)
and the load rules above. Wire validation checks the entire package before
applying that policy; malformed bytes are not excused by an unknown field name.

Discarded fields are omitted from live values and authored override ledgers.
`FAssetLoadReport::DiscardedFieldCount` reports the number of skipped field
occurrences, and the package recommends canonical resave without becoming
Dirty. Loading does not rewrite source bytes; the next explicit save emits only
the current schema. Construct-free inspection retains an informational
`UnknownField` finding for removed authored fields without marking the package
incompatible. It does not instantiate objects or discard bytes on disk.

The read-only `AssetMaintenance` compatibility probe freezes registered schema
identity, invokes Runtime's construct-free schema inspection, and reports
canonical identity/deprecated-route evidence with stable physical offsets and
fingerprints. It constructs no `DObject`, loads no dependency, invokes no
callback, and writes no authored file. The Editor compatibility window and
`DevTool asset check` consume the same deterministic records.

Canonical resave writes v10 through ordinary saving and accepts only current-format
inputs. Planning captures exact package identity, main/bulk fingerprint,
format, entry kind, residency, Dirty conflicts, compatibility, and evidence.
Apply revalidates the fingerprint, loads through the ordinary version-dispatched reader when
required, waits for family-owned save-readiness recovery, and publishes through
`SavePackages`. Verification rereads the exact v10 closure and
requires compatible current-format output with no remaining selected evidence;
failure restores the prior closure and Registry state. Project batches stop at
cancellation but do not claim project-wide atomicity.

User-facing resave steps are in
[Canonical Resave](../../Editor/Guides/CanonicalResave.md).

## Related Documentation

- [Asset Catalog And Mutation](AssetCatalogAndMutation.md)
- [Asset Data Lifecycle](AssetDataLifecycle.md)
- [Package Bulk Data](BulkData.md)
- [Versioning](Versioning.md)
- [Serialization](../Core/Serialization.md)
- [File I/O](../Core/FileIO.md)
- [Content Version Control](../../Development/VersionControl/ContentVersionControl.md)
