# Asset Packages

Durin object assets are stored as versioned `.dasset` packages. A package has one public main asset and may contain any number of `DObject` instances arranged through the ordinary Outer hierarchy. Outer defines structural containment and object paths, not a GC strong reference.

## Paths And Mounts

Asset identities use extensionless `FAssetPath` values such as
`/Engine/Materials/Default` or `/Game/Levels/TestLevel`. The first path segment
must match a registered mount. `FSourcePath` uses the same logical mount and
retains the filename extension. Both types resolve relative to the mount's
single `GetContentDir()`; neither virtual path includes `Root` or `ContentPath`.

The immutable Core registry publishes `/Engine/` and `/Game/` plus validated
project-declared extension and external-source mounts. Every mount may contain
`.dasset` packages and ordinary authoring files. `AutoScan` controls recursive
package discovery but never gates typed identity or direct loading. Typed
resolution reports invalid paths, unknown mounts, unavailable content
directories, escapes, missing files, forbidden dependencies, and read-only
authoring policy distinctly.

Existing paths and not-yet-created destinations are checked against canonical
content directories, including junction and symbolic-link targets.

The physical filename is the resolved virtual path plus `.dasset`. Main assets use the package path as their object path; inner objects append a colon and their relative Outer chain, for example `/Game/Objects/Test:Root.Component`.

## Runtime Lifetime

`DPackage` is an Outer-less object graph root. The asset manager roots loaded packages for garbage collection and caches one package instance per exact `FAssetPath`. Public asset loads resolve redirectors first and cache only the final real package; redirector packages are constructed only through AssetCore's internal exact tooling seam. Unload removes the package from the active cache, calls `MarkObjectHierarchyAsGarbage()` for the package tree, and runs GC so the path can be loaded again only after GC-controlled physical removal. Objects that must survive unload must be reparented out of that package first. `DPackage::Asset` is a `TObjectPtr` that strongly retains the main asset; arbitrary descendants remain alive only through actual GC strong references, not merely because their Outer is the package or asset. A package cannot unload while another loaded package declares a hard dependency that resolves to it.

Compiled-in reflection metadata uses a separate `Cpp` package kind. Each reflected module has one rooted `/Cpp/<ModuleName>` package whose structural children are its `DClass`, `DStruct`, and `DEnum` metadata. Those metadata objects are permanent independently of the package's Outer relationship. Cpp packages have no main asset, are not saved as `.dasset`, and remain alive for the process lifetime. CoreDObject intrinsic types are attached to `/Cpp/CoreDObject` after reflection bootstrap completes.

## Reference Model

Choose an object-reference type from the ownership and loading behavior, not
from whether a path happens to be available:

| Type | Use when | Persistence and lifetime |
| --- | --- | --- |
| Reflected `TObjectPtr<T>` | The owner requires the target object/package to be loaded and retained. | Serializes as a hard package dependency, resolves redirectors before eager loading, participates in GC, blocks final target-package unload, and blocks target deletion from outside the deletion set. |
| `TWeakObjectPtr<T>` | Code needs a non-owning handle to an object that is already loaded, such as editor selection or a transient cache. | Stores no durable asset identity, is not a reflected property kind, does not retain the target, and becomes invalid when the object is retired. |
| Reflected `TSoftObjectPtr<T>` | Authored data needs a typed package-main-asset identity without eager loading or retention. | Serializes only the authored path, has a non-owning loaded-object cache, contributes no hard dependency or unload blocker, follows relocation aliases without rewriting its identity, and may remain dangling after deletion. |
| `FAssetPath` or a path string | A service, document, import/source record, thumbnail key, or external setting needs identity but is not itself a reflected object reference. | The owning subsystem defines validation, persistence, move, and load behavior explicitly. Do not load an object merely to recover its path. |

`FSoftObjectPath::TryCreate(...)` validates nullable persistent identity.
`TSoftObjectPtr<T>::SetPath(...)` assigns identity without loading;
`TrySetObject(...)` assigns a package main asset and its path; `Get()` and
`IsLoaded()` inspect only the weak cache. The soft value keeps its authored path
and separately caches the resolved package identity, so an old path can safely
refer to a loaded object in the final real package without changing equality,
hashing, snapshots, or serialized bytes. Use typed
`Asset::ResolveSoftObject(...)` to distinguish `Null`, `NotLoaded`, and
`Loaded` without loading, and `Asset::LoadSoftObject(...)` for the explicit load
boundary. Both APIs enforce `T::StaticClass()`; null handling is selected with
`ESoftObjectNullPolicy`, and missing, incompatible, or corrupt targets return
ordinary `FAssetResult` diagnostics without changing the stored path.

Public typed `LoadAsset(...)`, cross-package hard-reference loading, and typed
soft resolve/load all use `ResolveAssetPath(...)` before constructing a package.
Expected-class validation applies to the final real metadata, and normal callers
never receive `DAssetRedirector` in place of the requested type. Registry-missing
direct loads remain available for ordinary packages after bounded header
validation; an unregistered redirector is not constructed by that fallback.

The registry exposes `FindAssetExact(...)` for physical entry identity,
`ResolveAssetPath(...)` for final real identity, deterministic direct reverse
redirect lookup, unified hard/soft/redirect reference-index queries, and
`BuildCookReachability(...)`. Redirect resolution follows at most 32 aliases and reports missing
requests, missing targets, cycles, depth overflow, unknown final classes, type
mismatch, and corrupt redirect metadata without changing runtime residency.
Relocation uses only `AnalyzeAssetRelocationBatch`,
`RevalidateAssetRelocationBatch`, `ApplyAssetRelocationBatch`, and
`RestoreAssetRelocationBatch`. Persistent settings and import records keep
their authored paths and resolve aliases at use sites; relocation never reads
or saves an arbitrary external store.

## File Format

Every authored or cooked `.dasset`, regardless of its main asset class, uses the
same DAST object-package envelope. AssetCore reads v2 and v3, and authorized
package saves write v3. Relocation preserves the source package version while
changing only the main-object name when a rename requires it.
Both headers record the `DAST` magic, format version, main asset class,
dependencies, and object count. V3 additionally stores a bounded registry-entry
kind and redirect destination immediately after the class name. An ordinary
asset must have an empty destination. A redirector must name
`Durin::Asset::DAssetRedirector`, contain exactly one object and one dependency,
and make that dependency equal its canonical destination. The asset path is
derived from the mounted package filename, so moving a package within a content
mount does not rewrite its payload. Package format versions describe this wire
contract only; reflected property evolution does not require a package-format
increment.

The bounded header reader classifies redirectors without constructing their
object graph or destination. Complete validation also requires exactly one
external `DestinationObject` field matching the header and dependency table.
V2 packages project as ordinary assets and are not dirtied or migrated by scan
or load; an authorized save emits v3. A v2 package claiming the redirector class
is corrupt because it has no authoritative redirect summary.

Asset-specific magic values belong to external derived or cooked payloads, not
to alternative `.dasset` envelopes. StaticMesh payloads use DMSH and texture
payloads use TXPL. A cooked `.dbulk` uses the DBLK container format and may
contain one of those asset-specific payloads; the cooked `.dasset` that
references it still begins with DAST.

Object records store object id, outer id, qualified class name, object name, and
a field table. Fields are identified by declaring qualified class plus property
name and include a recursive serialized-type signature and payload size.
Signatures describe the wire encoding, not C++ object size or another
process-local ABI property. Missing fields retain constructor defaults. Unknown
classes, invalid Outer hierarchies, malformed references, truncation, and
unsupported versions fail the complete load.

## Structure Compatibility

AssetCore retains every unknown or removed serialized object field
in an `FAssetLoadReport` instead of reducing compatibility to a log message.
Each object-level issue records the package and object identity, declaring
class, original field signatures and payloads, classification, risk, summary,
and optional handler identifier. Related legacy fields may be grouped into one
issue.

AssetCore owns report construction, payload retention, object-reference
resolution helpers, and optional class-specific upgrader registration. The
current repository baseline ships no production asset-specific structure
upgrader; unrecognized fields therefore remain explicit incompatibilities.
The registration API remains a low-level integration point rather than a
supported editor migration or resave workflow. Unhandled fields are
`UnknownIncompatible` with `UnknownNewerSchema` risk and do not themselves mark
the package Dirty. Rules that cannot preserve the represented data use
`DataLossRisk`.

A package whose load report contains compatibility-risk payloads rejects an
ordinary save. Persisting the in-memory representation requires a low-level
caller to pass explicit data-loss consent; editor workspaces expose no such
action. Level, Material, and Texture document opens reject any load report with
compatibility issues before activation, retain the previous active document or
world, and release only packages introduced by the rejected request. The error
directs the user to the read-only Asset Compatibility Audit for complete
details. This prevents unknown newer fields from being silently discarded by
normal editor saves while keeping the retained payloads as AssetCore's final
safety boundary.

The read-only compatibility probe is a separate, compact inspection path. The
game thread freezes registered class and property identities into a value-only
`FReflectionCompatibilityCatalog`; a worker can then stream object and field
descriptors from one package, validate ids, outers, lengths, and payload bounds,
and seek across payload bytes without copying them. It constructs no `DObject`,
loads no dependency, invokes no `PostLoad()`, changes no dirty state, and writes
no authored file. Package size and stable last-write ticks bind each result to
the registry snapshot and mark a changed input stale.

Each terminal record keeps inspection, compatibility, and freshness as
orthogonal states and reports stable codes for unknown fields, incompatible
signatures, unavailable classes, unsupported formats, invalid object graphs,
corrupt bytes, and I/O failures. Report schema v1 serializes stable string names
and deterministic virtual-path order; it never includes field payload bytes.
The frozen fixture corpus under
`Engine/Tests/Native/AssetCoreTests/Data/Compatibility` covers the current
format and every terminal classification without defining those incompatible
inputs as supported migration sources.

The editor exposes this probe only through `Tools > Asset Compatibility Audit`.
Opening or drawing the non-modal window compares already-published registry
metadata but performs no registry scan and reads no package bytes. `Run Audit`
is the sole start action: DurinEd snapshots the registry and reflection catalog,
launches one cancelable worker, and returns compact records through a
synchronized request-serial mailbox. The game thread owns the path-keyed live
index, deterministic sorting, filters, counts, finding details, diagnostic
copying, fingerprint reconciliation, and Content Browser navigation. A changed
fingerprint marks only that row stale; additions are not checked, removals
disappear, and project changes or shutdown cancel and drain the worker before
editor-owned state is released. The window offers no save, rewrite, discard, or
other data-loss action.

DurinDevTool exposes the same AssetCore probe as the explicit read-only
`asset audit --project <project.dproject>` command. Its native host enumerates
auto-scan mount contents without publishing or persisting an asset-registry
snapshot, captures the same value-only reflection catalog, and serializes the
same schema-v1 package records and finding codes in virtual-path order. Human
output groups the orthogonal states; `--format json` preserves the shared model
for CI. Independently repeatable `--fail-on incompatible`, `--fail-on
unsupported`, and `--fail-on error` policies affect only process status and
never authorize a content write. The command does not initialize an editor
workspace, renderer, GPU, source/import service, or DDC service.

Internal references use object ids. Cross-package strong references target the other package's main asset by `FAssetPath` and synchronously load that dependency. Circular dependencies work because object skeletons are constructed before dependency fields are applied.

Reflected `TSoftObjectPtr<T>` fields persist only their logical identity. Their
recursive DAST v2/v3 signature is
`SoftObject:<ExpectedQualifiedClass>:v1`; Array and Map signatures wrap it in
the ordinary container grammar. One value is `uint8 0` for null, or `uint8 1`,
a `uint64` UTF-8 byte count, and exactly one canonical `FAssetPath`. Paths are
bounded to 1 MiB. Invalid tags, truncation, overlong strings, trailing bytes,
non-canonical paths, and kind/signature mismatches fail deterministically.
Missing fields keep the constructor's null/default value.

Soft-object serialization never writes the weak loaded-object cache and never
adds its target to the package header dependency table. Loading stores the path
with an empty cache and succeeds when the target is unloaded or missing;
resolution and loading remain explicit typed AssetCore operations. Therefore a
soft path neither eagerly loads its target nor prevents target-package unload.
Archive property serialization and snapshots use the same bounded null/path
identity rule and likewise ignore loaded state.

Supported reflected payloads are numeric values, bool, strings, enums,
`DStruct` values, `TObjectPtr`, `TSoftObjectPtr`, vectors, maps, and nesting of
those containers.
Struct payloads contain a qualified-name field table. Missing fields retain
constructor defaults and unknown names are skipped, but a serialized field
whose name matches the current struct with a different kind or recursive type
signature fails loading with `TypeMismatch`; it is never reinterpreted.
Authored struct persistence is allowed only when its immutable `FDStructOps`
table advertises `AuthoredFieldsComplete`. Otherwise save, package load, and
typed inspection fail closed: save/load report `CustomStructCodecRequired`, and
the boolean inspection API returns false. AssetCore never silently omits
durable unreflected state. Runtime Archive `Serialize` callbacks
do not affect DAST: authored packages always retain the tagged reflected-field
representation so compatibility inspection remains possible. No current
production struct requires an authored custom codec.

DAST v2/v3 retain the logical `Array<...>` and `Map<...,...>` signatures, count
fields, and entry payload grammar. New Map saves order entries by the canonical
logical key token from the reflection contract, so equivalent supported maps
produce identical package bytes regardless of insertion, reserve, rehash, or
bucket history. Readers do not require that order and continue accepting valid
historical v2 packages written in unordered iteration order. V3 changes only
the public header summary; field payload grammar is unchanged.

Container package loading is bounded and transactional. Array elements and Map
keys/values decode into detached managed storage, duplicate decoded Map keys
are corrupt input, and the live destination is committed only after every
nested payload and post-deserialize callback succeeds. Construction,
allocation, truncation, duplicate, type, or repair failures unwind temporary
values and leave the original destination unchanged. Diagnostics identify the
array element or Map entry key/value path that failed.

AssetCore decodes each struct into default-constructed managed temporary
storage and requires copy assignment before reading the payload. After every
known nested field has loaded successfully, an optional `PostDeserialize`
callback receives `AuthoredAsset` plus the DAST format version. Only successful
repair commits the temporary value to the live destination. A callback
rejection reports `PostDeserializeRejected`; malformed fields, unavailable
operations, or rejected invariants leave the previous struct valid and
unchanged. This same transaction applies to `FAssetPackageField::TryReadStruct`.
Import-record output and detached-tombstone structs use this hook to rebuild
their parsed `FAssetPath` caches from authored path text.

String, Name, and Guid payloads use explicit logical encodings, so their current
signatures carry an encoding version rather than `ElementSize`. The reader also
accepts their v2-era `<kind>:<ABI-size>` signatures, including recursively in
arrays, maps, and struct fields, because those sizes never controlled their
payload bytes. Raw scalar and enum payloads are copied using `ElementSize`, so
their serialized signatures continue to require an exact width. Raw object
pointers and property kinds without runtime helpers fail package saving instead
of being silently omitted.

## Subsystem Boundary

- `CoreDObject` owns `DPackage`, `FAssetPath`, object paths, qualified reflected class identities, and type-erased container access.
- `AssetCore` owns `.dasset` I/O, the synchronous asset registry, package caching, dependency loading, structure-compatibility reports and upgrader registration, DDC storage, and cooked container/publication primitives.
- `Engine` owns asset-specific source provenance, import/build policy, derived-data keys and codecs, and cook contributions.
- Editor modules invoke the provider-neutral import and reimport framework
  documented in [Asset Import Framework](../../Editor/Architecture/AssetImportFramework.md).
- `DLevel` objects are main assets inside packages; a `DWorld` remains a runtime/editor session container and activates one level at a time.

Asset-level cooking and deterministic cooked publication are implemented for
StaticMesh, Texture2D, TextureCube, and ordinary package-only assets. Engine
owns a fixed built-in Cook-root list; it currently contains
`/Engine/Materials/DefaultMaterial`, whose package is published without an
empty bulk companion. Complete project discovery, editor or DurinDevTool
packaging commands, and installable-build orchestration are not yet connected.
Other deferred package-system work includes async loading and hot reload.

Package format versions are independent of the Durin engine release version.
Adding, removing, or reordering tagged reflected fields does not change the
package format. An engine release rewrites a package only when the package byte
contract changes or a recognized legacy-field upgrader produces canonical
authored state.

## Rebuildable Asset Caches

Durin stores asset discovery metadata and source-image thumbnails beneath `FPaths::DerivedDataCacheDir()`. The active project owns this directory; without a project, the engine directory is the fallback. It is ignored generated data, is never a content mount, and may be deleted in full while the editor is stopped. Mounted `Content` remains authoritative and repopulates both caches.

Compiled shader manifests, SPIR-V, and reflection sidecars also live beneath `DerivedDataCache/Shaders/`. Their virtual shader mount namespace prevents engine, project, and extension shaders from colliding. Authored Slang sources remain authoritative and rebuild the shader cache after deletion.

`StaticMesh/Objects/<key-prefix>/<key>.bin` stores DMSH render payloads built
from exact source bytes, importer identity and version, semantic import
settings, payload/builder versions, target platform, and target profile.
StaticMesh import writes this object for immediate editor reuse. A safe miss can
be rebuilt from source, and a cook can reuse the resulting render data without
making runtime depend on the DDC path.

`AssetRegistry/Registry.bin` is a versioned, deterministic snapshot keyed by virtual mount root and normalized mount-relative package path. Startup still enumerates mounted `.dasset` files once. Exact file-size and stable last-write-time matches reuse cached class, entry kind, redirect destination, format-version, and dependency metadata without reading package headers; new or changed files use the bounded header reader, and missing files disappear from the freshly published live map. Full validation bypasses fingerprint reuse and verifies redirector bodies. Schema, package-format, serialization, or mount-manifest incompatibility and corrupt or missing snapshots cause a non-fatal rebuild. Successful mutations update the live registry and dirty the snapshot, which is atomically replaced after explicit reconciliation and during orderly asset-manager shutdown. The live registry exposes a monotonic process-local revision that advances only when its published asset metadata changes, allowing editor queries to cache derived views safely. Scan diagnostics expose elapsed milliseconds, enumeration/reuse/reparse/removal/failure/redirector counts, and package-header read attempts and bytes.

`AssetRegistry/References.bin` is the single rebuildable hard, soft, and
redirect occurrence projection. Every source entry is keyed by its full package
size, stable last-write time, 128-bit content hash, DAST version, and extractor
schema. Each occurrence records source package and object identity, declaring
type, top-level field, kind, expected class, target path, a typed
fixed-array/Array/Map/struct route, and a deterministic display path. Results
sort deterministically and support target-to-referencer and deduplicated
source-to-target queries without changing package-header dependency semantics.

Extraction reads package fields and reflection metadata without constructing
owner objects, invoking `PostLoad`, resolving targets, or changing residency.
It accepts at most four container levels, 100,000 occurrences per package,
1,000,000 occurrences per snapshot, 1 MiB paths and Map-key tokens, and 4 KiB
display paths. Cache miss, fingerprint change, full validation, schema change,
or corrupt cache re-extracts the authoritative package; save, source relocation,
source deletion, and registry reconciliation update or invalidate the affected
source projection. A failed source extraction publishes no partial source
entry.

The registry's cook-reachability query resolves explicit roots and registered
external runtime roots to final real assets, then traverses canonical hard and
soft targets. It validates expected classes at the final metadata, rejects
missing/cyclic/corrupt redirects and incomplete source indexing, excludes alias
packages from the result, and terminates ordinary reference cycles through its
visited set. External providers contribute values without modifying their
authored stores. This Cook graph does not alter runtime loading or unload guards,
which continue to use only package-header hard dependencies.

`CanonicalizeAssetPackageForCook(...)` losslessly rewrites hard and soft paths
in produced bytes to final real paths and verifies that no dependency or field
still names a redirector. `FCookContext` runs that pass before staging, rejects
redirector packages, canonicalizes registered output identities, detects aliases
that collapse onto one output, and publishes only real packages and their bulk
companions. Cook never edits the authored `.dasset` or external root store.

Asset relocation is atomic and batched even for one mapping. Analysis captures
the registry revision, exact participant fingerprints, loaded-package state,
generated redirectors, and exclusively owned payload moves in a getter-only
token. Apply prepares and journals every output, publishes real destinations,
owned payloads, source and upstream redirectors, loaded package/object names,
and the complete registry projection under one revision. Restore and Redo use
the same retained token rather than computing reverse moves.

A successful `A -> B` keeps a direct `A -> B` redirector. Moving `B -> C`
retargets upstream aliases directly to `C`; moving back to an alias path may
reclaim it only when exact resolution proves that it denotes the same real
asset. Unrelated aliases and real assets remain hard collisions. Relocation
does not consult reference-index completeness, load or save referencers,
rewrite hard or soft authored paths, or modify project settings/import records.
Stale tokens, read-only participants, collisions, staging failures, and
publication failures either make no authoritative change or run reverse-order
compensation; failed compensation retains an explicit recovery-required
journal beneath every affected content mount. Its versioned entries record
physical/staged paths, pre/post fingerprints, publication order, and
completed/compensated state. An extensionless locator beneath
`Saved/AssetMutationRecovery` names those roots for recovery tooling but is not
authoritative data.

Deletion never rewrites persistent paths, but its immutable batch token uses the
unified graph and registered stores for safety diagnostics. Alias-only deletion,
broken aliases, and target selections missing any direct/upstream alias are
blocked. Deleting a target together with its complete alias closure requires an
explicit warning; soft and external-store occurrences warn that authored paths
will dangle. Registry/store revisions, warning snapshots, exact entries, and
files are revalidated and retained so Undo/Redo restores redirector metadata
exactly.

Fix Up is the only path-canonicalizing authoring transaction. It computes
upstream alias closure, rewrites tagged hard/soft package fields plus registered
external stores, verifies zero remaining incoming persistent occurrences, and
then optionally deletes the aliases. Dirty/incompatible/read-only inputs,
incomplete indexes, unavailable providers, changed fingerprints, publication
failure, or verification failure retain valid redirectors and restore every
participant.

Registry dependencies are package-level strong-reference edges collected from
the package header. They support loading, unload guards, move/delete checks, and
dependency-closure queries, but do not record which reflected property produced
an edge. In particular, they are not material Parent tags and cannot answer
which unloaded material instances are direct children of a material. Loaded
material hierarchy queries instead inspect canonical Parent chains; an unloaded
child query requires a future searchable-property metadata contract.

`Thumbnails/Index.bin` maps stable provider-neutral keys to PNG objects under
`Thumbnails/Objects/`. Source-image keys include normalized source identity,
source size and last-write time, maximum dimensions, generator schema,
color-space policy, and output encoding version. Rendered Material,
MaterialInstance, and TextureCube keys instead include virtual asset identity,
exact class, package fingerprint, provider and visual-contract schemas, fixed
output settings, and preview-fixture identity; material keys also include the
sorted transitive package-dependency closure. In-flight generation separately
revalidates asset and render-resource revisions before publication. A warm hit
decodes the generated PNG and follows the asynchronous, serial-validated RHI
upload path without reopening source input or loading and rendering the authored
asset. Missing or modified inputs, changed dependencies, settings, revisions or
versions, corrupt indexes or PNGs, and missing objects become safe misses.
Object and index writes are atomic; persistence failure does not fail valid
in-memory pixels. Encoded objects use an independent disk LRU budget, while RHI
textures retain their process-local GPU budget. Cleanup resolves and validates
every target beneath the exact thumbnail cache root before deletion. Editor
provider, scheduling, lifetime, and presentation ownership is documented in
[Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md).

`Textures/Objects/<key-prefix>/<key>.bin` and
`TextureCube/Objects/<key-prefix>/<key>.bin` store Texture2D and TextureCube
platform mip chains. Their 128-bit keys cover exact source-content identities,
every platform build setting, builder/payload/projection versions where
applicable, target platform, and target profile. Texture2D source fingerprints
avoid reopening unchanged source, while either texture class can use a valid
persisted identity and warm DDC object when source files are unavailable. Each
cache object is TXPL schema 1 with bounded mip/slice records and a payload
checksum.
Missing, incompatible, truncated, corrupt, or structurally invalid objects are
safe misses; a successful source build atomically replaces the object, while a
cache write failure does not invalidate usable in-memory platform data.

DDC objects are content-addressed generated files, so `.dasset` packages do not
store cache paths or byte offsets. Cooked packages instead serialize logical
payload descriptors and resolve their required DMSH or TXPL bytes from validated
DBLK companions beneath the configured cook root.

The shared authored/DDC/cooked storage classes, `.bin` versus `.dbulk`
semantics, loose companion naming, logical bulk descriptors, and runtime failure
policy are defined in [Asset Data Lifecycle and Storage](AssetDataLifecycle.md).

Repository storage rules for packages, source assets, and generated data are documented in [Content Version Control](../../Development/VersionControl/ContentVersionControl.md).
