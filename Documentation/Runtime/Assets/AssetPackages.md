# Asset Packages

Summary: Define asset identity, canonical DAST v9 packages, runtime residency, loading, and inspection.

Modules: AssetRegistry, Engine, CoreDObject, AssetMaintenance

Last reviewed: 2026-08-31

Durin object assets are stored as versioned `.dasset` packages. A package is a
residency and persistence container with zero or more independently addressable
top-level assets. Every direct persistent export is a top-level asset; its
descendants use the ordinary Outer hierarchy. Outer defines structural
containment and object paths, but is not by itself a GC strong reference.

## Public Capability Boundary

Persistent metadata consumers include `AssetRegistry/Catalog.h`,
`AssetRegistry/References.h`, and `AssetRegistry/Scan.h`. Engine exposes narrow
capabilities: `Asset.h` for resolution, residency, loading, and cooked payload
reads; `Asset/AssetOperations.h` for create/save; `Asset/Mutation.h` for exact
mutation; and `Asset/PackageSerialization.h` or `Asset/PackageInspection.h` for
package serialization and construct-free inspection. `AssetCook.h` owns Cook
reachability and publication. Developer `AssetMaintenance` owns project-wide
compatibility and canonical-resave batches.

Ownership is deliberately one-way:

- `CoreDObject` owns format-neutral linker tables, canonical tagged values,
  production DAST v9 read/write, bounded validation, and the construct-free v8
  conversion primitives used only by AssetMaintenance and focused fixtures.
- `AssetRegistry` owns canonical-v9 mounted-file discovery, bounded front-matter
  reads, and immutable package metadata/dependency snapshots.
- `Engine` captures live graphs into linker tables, applies validated linker
  tables to unpublished object graphs, owns residency, and provides transient
  exact inspection and mutation tools. Engine contains no package-table or
  tagged-value wire parser.
- `AssetMaintenance` owns deterministic compatibility/resave plans,
  fingerprints, stale checks, reporting, and publication rollback. It is not
  linked into the game Runtime.

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
`Durin.BinaryFormat.DAST`, and current production version 9. The ordinary codec
policy registers v9 only. Unknown identities, unsupported versions or features,
legacy prefixes, corrupt envelopes, and noncanonical encodings fail before
object construction, mutation, or publication.

The maintained `Engine/Content` and `Sandbox/Content` corpus is canonical v9.
Production discovery, save, load, inspection, mutation, Cook, and canonical
resave reject v7. The completed repository migration removed its converter,
decoder, command route, and fixtures.

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

Every `BulkData` linker value declares element size, power-of-two alignment,
storage kind, extent, and content digest. Inline values consume exact ranges in
Inline Bulk. External values consume exact aligned ranges in the headerless
raw `.dbulk` segment. Bulk Directory binds each value to its owner/field and
placement; zero padding is canonical and every byte of each inline or external
segment is consumed exactly once. Registry binds the complete external segment
by extent and XXH3-128 digest. Empty external closure means zero extent and a
zero digest.

The payload bytes are opaque to the package format. Asset-family code owns
dimensions, pixel/vertex formats, schema versions, rebuild policy, and repair.
The field/resource and Cook rules are defined by
[Package Bulk Data](BulkData.md) and
[Asset Data Lifecycle](AssetDataLifecycle.md).

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

An ordinary save validates the complete new v9 closure, stages main and bulk
bytes, publishes them as one recoverable unit, updates Registry state, then
clears Dirty/NewlyCreated. Existing non-v9 input is unsupported. Failure
restores the prior physical closure, catalog, residency, and dirty state.

Load resolves v9 policy, validates the complete main/bulk closure, and obtains
one detached `FLinkerTables`. Engine then validates registered classes and
fields, creates all package/export skeletons and Outer links unpublished,
resolves hard dependencies, applies detached values through the authored
Archive contract, restores explicit/forced provenance, and invokes
`PostDeserialize`/`PostLoad` only after their prerequisites succeed. The root
transaction publishes residency, dependencies, load reports, and cache state
only after the whole closure succeeds. Any failure destroys the unpublished
graph and releases dependencies admitted by the attempt.

Internal references use export indices. Cross-package hard imports target an
exact top-level asset; cycles work because skeletons exist before values are
applied. Missing fields keep constructor defaults. Unknown classes/fields,
incompatible recursive types, duplicate Map keys, malformed references,
callback rejection, or unavailable operations fail the complete load rather
than partially publishing state.

## Construct-Free Inspection And Mutation

Engine inspection consumes validated v9 linker tables and projects immutable
objects, fields, recursive values, hard/soft references, and BulkData storage
descriptors without constructing the inspected classes. It loads no dependency,
invokes no serializer or `PostLoad`, changes no dirty state, and never publishes
files. Texture and other asset-family inspectors add semantic interpretation
outside this package boundary.

Registry refresh uses only package-level front matter. Relocation, redirector
fix-up, deletion, compatibility maintenance, and Cook first select candidate
packages from those package edges, then open only the candidates that need an
exact occurrence or rewrite. Rewrites operate on detached v9 linkers, preserve
untouched values, rebuild Registry metadata, validate the exact output closure,
and enter the shared atomic transaction. No persistent occurrence route,
display path, or legacy value cache exists.

Package relocation rewrites only the package component and preserves top-level
asset names and subobject suffixes. Asset rename is a separate exact operation.
Deletion uses package-level hard blockers
and exact companion ownership. Cook carries identity plus exact main/bulk bytes
through canonicalization, reachability, pruning, publication, and runtime
admission; it never manufactures a legacy raw-segment metadata grammar.

## Compatibility And Canonical Resave

Ordinary load queries current classes and properties and fails before skeleton
publication on an unknown class/field or incompatible recursive type. It does
not create a maintenance report or retain unknown live payloads.

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

Ordinary load, discovery, save, mutation, Cook, and canonical resave accept v9
only. The bounded `asset migrate` workflow is the sole v8 boundary: it previews
and converts detached closures without constructing objects, stale-checks and
atomically publishes apply results, and verifies canonical v9 re-emission.
User-facing resave steps are in
[Canonical Resave](../../Editor/Guides/CanonicalResave.md).

## Subsystem Boundary

- `CoreDObject`: `DPackage`, `FPackagePath`, reflection, Archive semantics,
  linker tables, canonical values/Map keys, and DAST v9 codec.
- `AssetRegistry`: mounted discovery, bounded v9 front-matter projection,
  package metadata/dependency state, revisions, and rebuildable cache.
- `Engine`: physical closure I/O, live capture/application, residency,
  dependency loading, exact inspection/mutation, DDC integration, and Cook.
- `AssetMaintenance`: project snapshots and compatibility/resave reports.
- Asset-family Runtime/Editor modules: canonical imported data, domain payload
  codecs, build/DDC recipes, save readiness, and Cook contributions.

Asset-level Cook publication is implemented for StaticMesh, Texture2D,
TextureCube, VolumeTexture, TerrainHeightmap, SkeletalMesh, AnimationClip, and
ordinary package-only assets. Engine also owns the fixed built-in Cook-root
list. Async package loading, hot reload, and complete installable-build
orchestration remain separate future work.

## Related Documentation

- [Asset Catalog And Mutation](AssetCatalogAndMutation.md)
- [Asset Data Lifecycle](AssetDataLifecycle.md)
- [Package Bulk Data](BulkData.md)
- [Versioning](Versioning.md)
- [Serialization](../Core/Serialization.md)
- [File I/O](../Core/FileIO.md)
- [Content Version Control](../../Development/VersionControl/ContentVersionControl.md)
