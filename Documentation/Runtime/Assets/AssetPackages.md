# Asset Packages

Summary: Define asset identity, canonical DAST v9 packages, runtime residency, loading, and inspection.

Modules: AssetRegistry, Engine, CoreDObject, AssetMaintenance, AssetTools

Last reviewed: 2026-09-03

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
  production DAST v9 read/write, and bounded validation.
- `AssetRegistry` owns canonical-v9 mounted-file discovery, bounded front-matter
  reads, and immutable package metadata/dependency snapshots.
- `Engine` captures live graphs into linker tables, applies validated linker
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

The mounted `FPackagePath` is part of package validation. DAST v9 includes that
identity in its canonical name table, and every header, complete-read,
inspection, mutation, relocation, Cook, and admission call supplies the exact
identity expected for the physical file. Moving a package therefore requires a
canonical v9 rewrite; a caller cannot validate the same bytes under an
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

The internal v9 codec accepts an optional dependency load policy containing
package resolution, exact-object resolution, and failure cleanup together.
An incomplete policy is rejected before skeleton creation. With a policy,
linker dependencies and serialized external object fields use its retained
callbacks exclusively; failures preserve their asset error codes. Once graph
application starts, failure discards that graph and invokes policy cleanup
instead of releasing packages selected by a global load snapshot. The caller
owns dependency lifetimes and scopes cleanup to that invocation. Without a
policy, ordinary loading keeps its existing behavior.

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

## Canonical DAST v9 Format

DAST has permanent format identity
`3c59d1a9-6ceb-4e4c-b059-452db0a5af56`, diagnostic name
`Durin.BinaryFormat.DAST`, and current production version 9. Supported readers
and corpus-transition policy are defined by
[Versioning](Versioning.md#authored-package-policy).

`CoreDObject`'s `DObject/PackageFormat.h` is the sole code authority for the
DAST magic, identity, format and object-stream versions, supported-reader set,
and persisted-projection reader-policy fingerprint. AssetRegistry and Engine
consume those constants directly and publish no independent package-format
copy.

### Envelope And Sections

A v9 main image contains the 64-byte DURF v1 preamble, a 32-byte DAST format
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
exists in v9.

### Linker Tables And Canonical Values

`DObject/PackageLinker.h` is the format-neutral contract. `FLinkerTables` owns
the package summary, canonical names, recursive serialized types, schemas,
custom versions, imports, exports, property tags, provenance, values, and BulkData
placement facts. `FPackageIndex` represents null/import/export identity without
exposing wire arithmetic.

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
emission. Late discovery, duplicate identity, invalid topology, unsupported
value kinds, malformed UTF-8, noncanonical order, arithmetic overflow, or a
limit failure aborts without replacing either output. Identical logical input
and identity produce byte-identical main/bulk output.

`ReadPackageV9Registry(...)` validates only the declared front matter and
physical main/bulk extents before publishing package-level metadata.
`ReadPackageV9(...)` validates the complete main image and exact external
segment into a detached linker model. It checks tables, indices, topology,
types, values, section and payload digests, range placement, complete
consumption, and canonical re-emission before replacing its output. Neither
entry constructs a `DObject`, resolves dependencies, invokes callbacks, or
writes files.

### BulkData Closure

Bulk Directory binds each `BulkData` value to an Inline Bulk or external raw
`.dbulk` range; Registry binds the complete external segment. Placement,
alignment, padding, and digest rules are defined by
[Package Bulk Data](BulkData.md#dast-v9-authored-placement).
Payload bytes remain opaque to the package format; asset families own their
schemas, interpretation, and [data lifecycle](AssetDataLifecycle.md).

## Production Save And Load

Engine's save boundary walks each live object's ordinary
`DObject::Serialize(...)` Archive to discover and then capture the complete
effective graph. It resolves defaults and authored provenance, freezes object
and field manifests, converts every value into detached CoreDObject linker
tables, and calls `WritePackageV9`. Encoding never dereferences the live graph.
A graph, field, reference, version, or value first seen after discovery is a
save failure.

Save options select `Authored` or `Cooked`. Cooked capture carries the exact
target profile and omits `EditorOnly` fields unless diagnostic retention is
explicit. Owned per-save overrides may omit objects/properties or supply copied
replacement values without mutating live state; validation rejects foreign or
conflicting entries and hard references to omitted objects.

An ordinary save is a one-package invocation of `SavePackagesAtomically`. It
validates the complete new v9 closure, publishes owned payloads before the main
image that binds them, verifies the stable closure, clears Dirty/NewlyCreated,
then publishes a revisioned Registry delta. Failure before authored commit
restores the prior closure and leaves the package retryably Dirty. Registry
failure after authored commit keeps valid bytes, fences the affected path, and
returns `ContentCommittedProjectionPending` for reconciliation.

Load resolves v9 policy, validates the complete main/bulk closure, and obtains
one detached `FLinkerTables`. Engine then validates registered classes and
fields, creates all package/export skeletons and Outer links unpublished,
resolves hard dependencies, applies detached values through the authored
Archive contract, restores explicit/forced provenance, and invokes
`PostDeserialize`/`PostLoad` only after their prerequisites succeed. The root
transaction publishes residency, dependencies, load reports, and cache state
only after the whole closure succeeds. Archive, dependency, and load-policy failures
destroy the unpublished graph and release dependencies admitted by the attempt.
`PostLoad()` is a void lifecycle notification: recoverable initialization failures
are logged by the object and do not reject package publication or duplication.
Callbacks must preserve safe state, repair invalid relationships, or leave derived
resources unavailable; resource consumers and explicit Cook/build operations own
their readiness checks. Data that must reject a load belongs in archive validation,
not a PostLoad return value.

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
PostLoad policy. Serializer and struct migration callbacks still execute and
require their own admission before use in isolated preparation.

`PreparePackageGraphs` consumes saved immutable closures on GameThread and uses
those same canonicalization, schema, skeleton, field and authored-ledger phases.
It creates all private skeletons before applying any values, including default
inners, and resolves batch imports against their private package identities.
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
across GC. Active load transactions and projection fences return Busy; preparation
rechecks fences after disk validation and never clears a fence.

The load scope records only exact package generations admitted by its calls,
including transitive dependencies. On failure the caller retains that scope, drops
candidate graphs, then calls `Release`; InUse retains the records for retry. Scope
destruction transfers residency and is not abort cleanup. Reload abort may pass
exact weak target identities to ignore those targets' saved dependency edges, which
may differ from their edited live references. Real live references and dirty/new
state still protect owned packages. Other packages' saved edges retain their normal
protection, and neither scope ownership nor the exception follows a same-path
replacement. Failed ordinary load transactions retire resources of successfully
loaded nested dependencies along with their object rollback.

The caller must explicitly admit each export class's construction and serialization
callbacks and hold path admission/edit/save leases. The result is `ValuesPrepared`:
ordinary PostLoad and runtime publication have not run, and loaded migration/version
metadata remains available for the owning resource preparation step. Preparation
revalidates saved closure digests before returning; failure preserves the output
and marks only newly created private hierarchies as garbage without running global
GC. Candidate package/object limits include default inners; additional dependency
load budgets and decoded/scratch/runtime byte accounting remain the caller's
responsibility. Graph ownership and destruction
stay on GameThread. This is an internal deserialization boundary, not a public
reload admission or success result.

## Construct-Free Inspection And Mutation

Engine inspection consumes validated v9 linker tables and projects immutable
objects, fields, recursive values, hard/soft references, and BulkData storage
descriptors without constructing the inspected classes. It loads no dependency,
invokes no serializer or `PostLoad`, changes no dirty state, and never publishes
files. Texture and other asset-family inspectors add semantic interpretation
outside this package boundary.

Mutation tools use Registry package edges to select candidates, then open only
those requiring exact inspection. Rewrites operate on detached v9 linkers, preserve
untouched values, rebuild Registry metadata, validate the exact output closure,
and enter bounded artifact publication. No persistent occurrence route,
display path, or legacy value cache exists.

Relocation, asset rename, Fix Up, and deletion policy belong to
[Asset Catalog And Mutation](AssetCatalogAndMutation.md). Cook reachability and
publication belong to [Asset Data Lifecycle](AssetDataLifecycle.md#cook-and-publication-rules).

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
identity, invokes Runtime's construct-free v9 schema inspection, and reports
canonical identity/deprecated-route evidence with stable physical offsets and
fingerprints. It constructs no `DObject`, loads no dependency, invokes no
callback, and writes no authored file. The Editor compatibility window and
`DevTool asset check` consume the same deterministic records.

Canonical resave is current-format v9 maintenance, not reimport or format
conversion. Planning captures exact package identity, main/bulk fingerprint,
format, entry kind, residency, Dirty conflicts, compatibility, and evidence.
Apply revalidates the fingerprint, loads through the ordinary v9 reader when
required, waits for family-owned save-readiness recovery, and publishes through
`SavePackagesAtomically`. Verification rereads the exact v9 closure and
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
