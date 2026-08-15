# Asset Packages

Summary: Define asset identity, registry, package serialization, loading, migration, derived data, and cooking.

Modules: AssetCore, CoreDObject

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
package discovery and therefore ordinary load visibility. A package under a
manual-scan mount is still a valid authored identity, but it must be admitted
explicitly before `LoadAsset` can see it. Typed
resolution reports invalid paths, unknown mounts, unavailable content
directories, escapes, missing files, forbidden dependencies, and read-only
authoring policy distinctly.

Existing paths and not-yet-created destinations are checked against canonical
content directories, including junction and symbolic-link targets.

The physical filename is the resolved virtual path plus `.dasset`. Main assets use the package path as their object path; inner objects append a colon and their relative Outer chain, for example `/Game/Objects/Test:Root.Component`.

## Runtime Lifetime

`DPackage` is an Outer-less object graph root. AssetCore's private runtime state
roots resident packages for garbage collection and caches one package instance
per `FAssetPath`. Newly created and persistent packages share that store. Each
resident entry is explicitly `NewlyCreated` or `Published`, while
`DPackage::IsDirty()` independently records unsaved contents. Authoring code
uses `FindResidentPackage` for either state; save promotes the same entry to
`Published` after catalog publication.

Public asset load first accepts an already resident non-redirector package,
including a newly created package, without file I/O. Otherwise it resolves the
persistent catalog and caches only the final real package; redirector packages
are constructed only through AssetCore's internal exact tooling seam. A catalog
miss never guesses a physical filename or discovers an unindexed file. Unload
rejects newly created or dirty packages by default. A caller that intentionally
abandons unsaved work passes `EAssetPackageUnloadPolicy::DiscardUnsaved` to the
same `UnloadPackage` operation. Successful unload removes residency, calls
`MarkObjectHierarchyAsGarbage()` for the package tree, and runs GC so the path
can be loaded again only after GC-controlled physical removal. Objects that
must survive unload must be reparented out of that package first.
`DPackage::Asset` is a `TObjectPtr` that strongly retains the main asset;
arbitrary descendants remain alive only through actual GC strong references,
not merely because their Outer is the package or asset. A package cannot unload
while another loaded package declares a hard dependency that resolves to it.

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
soft resolve/load accept an exact resident real package first, then use
`ResolveAssetPath(...)` before constructing a persistent package.
Expected-class validation applies to the final real metadata, and normal callers
never receive `DAssetRedirector` in place of the requested type. A catalog miss
with no exact resident package returns `NotFound` without deriving a filename,
probing a package header, or publishing metadata. Editor recovery may explicitly call
`AdmitAssetPackageToCatalog`; startup and Cooked-runtime fixtures refresh the
catalog after publishing their mounts.

The registry exposes `FindAssetExact(...)` for physical entry identity,
`ResolveAssetPath(...)` for final real identity, deterministic direct reverse
redirect lookup, unified hard/soft/redirect reference-index queries, and
`BuildCookReachability(...)`. Redirect resolution follows at most 32 aliases and reports missing
requests, missing targets, cycles, depth overflow, unknown final classes, type
mismatch, and corrupt redirect metadata without changing runtime residency.
Production relocation uses `PrepareAssetRelocationTransaction` to obtain an
immutable summary and opaque transaction. `Commit`, `Undo`, and `Redo` own
final revalidation, journal advancement, compensation, and recovery
classification. Persistent settings and import records keep their authored
paths and resolve aliases at use sites; relocation never reads or saves an
arbitrary external store.

## File Format

Every authored or cooked `.dasset`, regardless of its main asset class, uses the
same DAST object-package envelope. DAST v4 is the sole authored package reader,
ordinary writer, and repository baseline. Unsupported versions fail before
header-specific interpretation, object construction, mutation, or publication.
Relocation preserves the package format while changing only the main-object
name when a rename requires it. The header records the `DAST` magic, format
version, main asset class, bounded registry-entry kind, redirect destination,
dependencies, and object count. An ordinary
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
Every format outside the supported-reader set is rejected before
header-specific metadata or the object graph is interpreted. Registry and
reference caches include the package format in their fingerprints, discard
unsupported entries, and cannot reuse a projection after package bytes or the
declared format changes.

Asset-specific magic values belong to external derived or cooked payloads, not
to alternative `.dasset` envelopes. StaticMesh payloads use DMSH and texture
payloads use TXPL. A cooked `.dbulk` uses the DBLK container format and may
contain one of those asset-specific payloads; the cooked `.dasset` that
references it still begins with DAST.

### Frozen DAST v4 Wire Contract

DAST v4 is the qualified authored format. AssetCore exposes production-owned
low-level writer and reader boundaries, and package policy routes header,
inspection, compatibility, reference, registry/cache, and live-load operations
only to v4. Ordinary and bundle saves use the v4 live writer in no-delta mode.
The layout below is frozen: later format changes must use a new version and an
explicit migration rather than altering these bytes or semantics.

A v4 package starts with bytes `44 41 53 54`, then `04 00 00 00` (`uint32`
little-endian version 4), a little-endian `uint32` public-summary byte length,
and `uint8(5)` section count. The summary is followed by exactly five 9-byte
directory entries: `uint8 kind`, little-endian absolute `uint32 offset`, and
little-endian `uint32 length`. Directory kinds and order are Name `0x01`, Type
`0x02`, Schema `0x03`, Object `0x04`, and Value `0x05`. The first section begins
immediately after the directory, each following offset equals the preceding
offset plus length, and the last extent equals file size. There is no alignment
padding. Duplicate, missing, unknown, out-of-order, overlapping, overflowing,
gapped, or trailing extents are invalid. The summary is bounded to 65,535 bytes
and the complete package to 256 MiB.

The public summary contains, in order, an asset-class wire string, `uint8` entry
kind (`0` asset or `1` redirector), redirect-destination wire string (empty only
for an asset), VarUInt dependency count, canonical bytewise-sorted unique
dependency strings, and VarUInt object count. It must be consumed completely.
Header-only validation reads this payload and the fixed directory without
parsing or allocating body tables.

Fixed-width `uint16`, `uint32`, `uint64`, IEEE-754 binary32/binary64, and GUID
components are little-endian independent of host layout. VarUInt is minimal
unsigned LEB128 over `uint64`, at most 10 bytes, and rejects overflow, unused
high bits, and overlong forms. Signed integers use ZigZag followed by VarUInt.
Counts and ids apply their narrower semantic bounds before allocation. A wire
string is a VarUInt byte length followed by valid shortest-form UTF-8;
surrogates, invalid sequences, embedded NUL, and lengths over 1 MiB are invalid.
Writers preserve code points without Unicode normalization, and identity/order
uses exact UTF-8 bytes. Empty strings are allowed only where this contract says
so.

All table ids are one-based VarUInt; zero means absent only where explicitly
permitted. Discovery closes before emission, and a late name, type, schema,
object, dependency, or custom version is an internal save failure. The canonical
tables are:

- Name entries are nonempty strings deduplicated and sorted by unsigned bytewise
  lexical order. The section is `count` followed by wire strings.
- Type entries deduplicate and sort by their complete self-contained structural
  descriptor bytes. Sorting keys recursively contain the opcode, directly
  encoded qualified name where applicable, scalar parameters, and child keys;
  they never contain package-local ids. Emitted length-delimited records replace
  names and children with frozen ids. Opcodes are Bool `01`, I8 `02`, I16 `03`,
  I32 `04`, I64 `05`, U8 `06`, U16 `07`, U32 `08`, U64 `09`, F32 `0A`, F64
  `0B`, String `0C`, Name `0D`, Guid `0E`, Enum `0F`, Intrinsic `10`, Struct
  `11`, FixedArray `12`, Array `13`, Map `14`, HardRef `15`, SoftRef `16`, and
  Bytes `17`. Enum stores qualified-name id and integer storage opcode;
  Intrinsic stores layout id; Struct stores qualified-name id; FixedArray stores
  element-type id and nonzero dimension; Array stores element-type id; Map stores
  key- and value-type ids; references store expected-class name id, with zero
  meaning `DObject`. Resolved type cycles are invalid.
- Schema begins with custom-version count and entries sorted by numeric GUID
  tuple `(A,B,C,D)`, each four little-endian `uint32` components plus an unsigned
  `uint32`-domain VarUInt value. Schemas then sort by qualified-type UTF-8 bytes.
  Each length-delimited schema is qualified-name id, field count, then fields
  sorted by field-name bytes, type descriptor bytes, and authored flags. A field
  is field-name id, type id, and authored-flags VarUInt. The only v4 authored
  flags value is zero; field ids are one-based positions in the canonical
  schema.
- Object is object count followed by length-delimited
  `(outer-object-id, class-name-id, object-name-id)` records. Id 1 is the package
  root with outer id zero. Remaining objects sort by canonical outer path,
  class-name bytes, then object-name bytes; duplicate sibling identities are
  invalid.

Every table, record, and section is completely consumed. Limits are 1,048,575
names, types, schemas, and objects; 65,535 fields per schema; 4,096 dependencies;
256 custom versions; 1,048,575 container elements; and nesting depth 64, all
further constrained by the package-size bound.

The Value section is object count followed, in object-id order, by one
length-delimited block per object. A block is override count followed by records
sorted by `(schema id, field id)`. A known record is schema id, field id,
provenance (`00` explicit or `01` forced), value byte length, and value. Duplicate
fields and noncanonical order are invalid. The schema type selects exactly one
payload, so values do not repeat opcodes:

- Bool is byte `00` or `01`; unsigned integers are VarUInt and signed integers
  ZigZag VarUInt. F32/F64 use little-endian IEEE bits, preserve signed zero and
  infinities, and canonicalize NaNs to quiet `0x7FC00000` or
  `0x7FF8000000000000`.
- String is a wire string; Name is a Name id; Guid is four little-endian
  components; Enum follows its declared integer storage; Bytes is VarUInt length
  plus exact bytes.
- Intrinsic layouts are `01` FVector2 `(x,y)` F64, `02` FVector3 `(x,y,z)` F64,
  `03` FVector4 `(x,y,z,w)` F64, `04` FQuat `(w,x,y,z)` F64, `05` FTransform
  `(rotation FQuat, translation FVector3, scale FVector3)`, and `06`
  FLinearColor `(r,g,b,a)` F32. Raw GLM or C++ memory is never serialized.
- Struct is changed-field count followed by canonical `(field id, provenance,
  value length, value)` records under its schema. FixedArray emits exactly its
  dimension elements without a count; Array emits count then elements. Map
  rejects duplicate logical keys and sorts entries by complete canonical encoded
  key bytes; keys are limited to Bool, integer, String, Name, Guid, Enum, and
  fixed-size Intrinsic.
- HardRef is tag `00` null, `01` plus internal object id, or `02` plus public
  dependency id. SoftRef is tag `00` null or `01` plus a Name id containing the
  canonical soft-object path. Unknown tags and zero or out-of-range ids are
  invalid.

An omitted object field means the current immutable class default. A struct
field may be omitted only when registered operations provide deterministic
construction and logical equality. Missing operations or an authored custom
serializer without a proven v4 codec fail closed. A loaded explicit field keeps
provenance `00` even when equal to today's default; provenance `01` records a
serializer-forced override and equality never removes it.

Custom-version discovery freezes with the other tables. Duplicate GUIDs,
unsupported known values, out-of-range values, and discovery/emission mismatch
fail before publication. Unknown GUID/value pairs are retained exactly and
re-emitted in canonical GUID order; a live load may proceed only when no known
codec declares that GUID required for interpretation.

Unknown explicit values use provenance `02`. Their body is retained-closure
length and bytes followed by payload length and exact payload bytes. The closure
is a self-contained mini Name/Type/Schema table using the same canonical
encodings, followed by root schema and field ids. Its ids never refer to or get
remapped through package tables. Closure and payload bytes are copied exactly on
canonical resave, and the closure must parse completely and resolve its root
descriptor before acceptance. Canonical rebuilding therefore cannot change an
opaque value's descriptor meaning.

Validation uses checked arithmetic and temporary immutable models. Invalid
UTF-8, nonminimal VarUInt, bounds or id violations, descriptor cycles,
unordered or duplicate records, overlap or gaps, truncation, unsupported
opcodes or flags, impossible counts, excess depth, trailing bytes, and
incomplete length-delimited consumption fail before destination mutation or
output publication. V4 has no optional section or opcode; extensions requiring
new kinds require a later format version.

The executable reference goldens consume the production logical delta plan and
qualify a complete current Default Material at 6,275 bytes: envelope/directory
79, Name 1,803, Type 62, Schema/custom versions 107, Object 5, and Value 4,219.
It contains 105 names, 21 structural types, 6 schemas, 1 object, 1 top-level
override, and maximum depth 5; the plan contains 785 fields, emits 554, omits
231, performs 1,275 logical comparisons, and requires no authored-intent
ledger in no-delta mode. XXH64 is `C4111B7609C78D4F`. This is 10,109
bytes below the 16,384-byte controlling gate and 14,384 bytes below the
20,659-byte same-content-v2-relative gate. Modeled parse operations/allocation
inputs fall from v3's 5,020/3,948 to 134/133 without compression. Repeated and
reverse-discovery packages are byte-identical. Loaded-explicit and forced ledger
fixtures exercise provenance `00` and `01`, while unknown-retention fixtures
keep `02`; clearing the ledger restores the same baseline bytes. These values
also qualify the production writer byte-for-byte against the independent
reference codec. The same production codec now owns ordinary v4 package saves
and bounded v4 reads.

### Explicit DAST v4 Writer Boundary

`Durin::Asset::DastV4::WritePackage` consumes only owned logical package input:
public summary values, structural type and schema descriptors, custom versions,
object topology, completed known overrides, and exact retained unknown closure
and payload bytes. It closes and canonicalizes the package-local tables before
emission, rejects malformed descriptors, topology, values, versions, retained
closures, limits, and discovery mismatches with a stable typed diagnostic, and
replaces the caller's destination only after the complete bounded package has
been assembled.

`Durin::Asset::DastV4::WriteAssetPackage` is the sole live-object integration
entry. It performs Archive discovery/emission manifest checks, consumes
`BuildDefaultDeltaPlan` in enabled or no-delta mode, and then delegates to the
low-level writer. Ordinary and atomic-bundle saves select no-delta mode so a
loaded canonical v4 package resaves byte-identically. Relocation, redirector
Fix Up, and cook canonicalization decode the existing v4 model, apply their
bounded rewrite, and canonically re-encode it. Inspection and loading remain
read-only.

Retained provenance `02` inputs keep their descriptor closure and payload as
separate exact byte spans. Before publication, the writer parses all closure
framing, canonical Name/Type/Schema tables, descriptor references and cycles,
root schema/field resolution, bounds, and trailing-byte state. Package table
reordering never remaps or rebuilds those bytes.

### Explicit DAST v4 Reader Boundary

`Durin::Asset::DastV4::ReadHeader` validates the bounded public summary and
five-entry directory without parsing or allocating body tables.
`DecodePackage` owns a temporary pointer-free logical model containing immutable
one-based ids, decoded values, raw payload locations, and separate exact
retained closure/payload spans. It validates every primitive, extent, table,
record, descriptor, topology, value, and closure, then re-emits through the
production writer and requires byte equality before replacing the caller's
destination. `ReencodePackage` exposes that same canonical round trip.

`LoadAssetPackage` is the sole explicit live-reader entry. It creates every
object skeleton and Outer before resolving dependencies, applies only known
overrides through the shared authored Archive relative to constructor/class and
Struct defaults, restores loaded-explicit and forced authored intent, and calls
PostLoad in reverse object order. The move-only `FLoadedAssetPackage` handle is
the publication and lifetime owner. Decode, class, dependency, Archive, ledger,
PostLoad, or publication failure destroys the complete new graph, releases
dependencies loaded by the failed attempt, and preserves the caller's previous
handle and report.

`InspectPackage`, `ExtractReferences`, and `ProbeCompatibility` consume the
same decoded logical model without constructing objects or invoking callbacks.
Known values project to existing field/reference semantics, including intrinsic
math Struct payloads and canonical Map routes. Provenance `02` never remaps
through package tables: compatibility and live reports carry its exact
`DescriptorClosure` and `RetainedPayload` spans and retain data-loss risk.

The package policy routes bounded header, validation, inspection, reference,
compatibility, and ordinary live-load reads through these entries when the
preamble declares v4. Reads never write or dirty a package. Unsupported versions
fail before version-specific parsing. Saves invoke the v4 writer only after
compatibility and stale-input checks succeed.

### Production Save and Load

Object records store object id, outer id, qualified class name, object name, and
a field table. Fields are identified by declaring qualified class plus property
name and include a recursive serialized-type signature and payload size.
Signatures describe the wire encoding, not C++ object size or another
process-local ABI property. Missing fields retain constructor defaults. Unknown
classes, invalid Outer hierarchies, malformed references, truncation, and
unsupported versions fail the complete load.

DAST v4 save and load are purpose-specific `FArchive` adapters. Saving runs a
discovery pass through each live object's virtual `DObject::Serialize(...)`,
freezes object ids, fields, dependencies, logical types, and version use, then
calls the same entry for emission. A derived serializer must call its base once
and can persist additional durable state only as stable named fields. A field,
dependency, type, object, or version first seen during emission fails before
file or registry publication. Repeated saves preserve the existing canonical
tables, overrides, dependencies, objects, and Map ordering.

Loading validates records and creates all object skeletons and Outer links
before applying fields. Each live object receives exactly one DAST load Archive
call; unavailable fields leave constructor defaults, while known incompatible
fields fail and unknown fields enter the compatibility pipeline. Dependencies
are resolved before their references are applied. Successful objects receive
`PostLoad` in reverse object order. A bounds, schema, Archive, dependency,
callback, or `PostLoad` failure rolls back the complete new package and leaves
the active cache, registry, files, and prior dirty state unchanged.

Live loading uses one root transaction boundary. The root package owns dependency
residency, registry publication, load reports, and cache transfer; nested
dependencies queue their entries until the root commits. Any decode, dependency,
field, ledger, callback, or `PostLoad` failure destroys the complete new graph
and restores prior residency, registry, cache, report, and dirty state.

Each nonresident package is read from its physical file once. The package policy
resolves the codec from that byte buffer, reads and validates the neutral header
through the same codec and bytes, then supplies the unchanged bytes to live
loading. Header validation still precedes skeleton publication. The root
`FAssetLoadReport::PackageFileReadCount` records successful package-file reads
across the complete dependency closure; it is structured bounded telemetry, not
a cache or a per-package log.

An ordinary single-package or atomic-bundle save may update an existing package
only when its registered format equals the ordinary v4 writer. A stale or
unsupported registry version is rejected before serialization, staging, file
publication, registry publication, or dirty-state clearing. New packages use
the same ordinary writer. Format transitions belong only to an explicit
migration transaction.

Current-format byte mutations resolve the source codec before decoding and
require its declared mutation capability plus an exact match with the ordinary
writer format. Reference fixup, relocation, redirector creation, and cook
canonicalization therefore reject a supported non-ordinary format before
changing output bytes. Version-specific decoded packages remain inside their
codec adapter; shared transactions consume neutral headers, inspections,
reference edges, load handles, and byte results.

### Canonical Reflected-Identity Resave

Canonical resave is current-format package maintenance, not package-format
migration or reimport. The v4 metadata probe records every registered legacy
class, struct, or enum identity in the package header, object records, schemas,
and recursive type descriptors. Each finding carries the package, stored and
current identity, reflected kind, stable location, and logical path. The same
evidence is attached to a live `FAssetLoadReport`; a successfully loaded package
exposes `IsCanonicalResaveRecommended()` without becoming Dirty.

`PlanAssetCanonicalResaves` deterministically selects packages, folders,
mounts, or an explicit project scope and captures the physical fingerprint,
format, entry kind, residency, Dirty conflict, compatibility state, and
evidence. It blocks stale inputs, non-current formats, redirectors, incompatible
payloads, dirty loaded packages, and non-authoring mounts. A package with no
evidence is skipped unless an interactive caller explicitly requests a plain
package resave.

Apply revalidates each fingerprint, loads through the ordinary current-format
reader when necessary, and publishes one bounded atomic package unit through
`SavePackagesAtomically`. It never invokes an import provider or source-data
workflow. The published bytes are reread through the compatibility probe;
success requires the current writer, compatible inspection, and zero remaining
legacy identities. Verification or registry-reconciliation failure restores
the prior authored bytes. Batch admission stops at cancellation and retains
terminal results for completed units; project maintenance does not claim
project-wide atomicity.

`DurinAssetTool --operation=canonical-resave` is dry-run by default. Selection
uses `--package`, `--folder`, `--mount`, or explicit `--project-scope`; `--apply`
writes, `--format=human` selects a compact human report, and the default is a
deterministic JSON report. `--ci` is read-only, cannot be combined with apply,
and fails when selected compatible content still has registered legacy
identities. This operation is separate from the exact-edge migration command
below.

### Explicit Package Migration

`DevTool asset migrate` is the only package-format migration boundary. The
current v4-only baseline registers no built-in migration edge, so it reports no
path for older packages. A future format transition must explicitly register
its exact source-to-target edge before this workflow can select it.
Planning is read-only, requires explicit package or mount selection (or an
explicit whole-corpus invocation), closes dependencies that also require
migration, and records content hashes, sizes, stable timestamps, source and
target codec identities, the exact edge identity and risk, and the reader-policy
identity. Major-zero migration intentionally supports one exact source-to-
migration-target edge rather than a multi-hop graph. The edge executes a
load-transform-write strategy: source-aware load and schema upgrade, an optional
edge-owned transform callback, then the selected target codec writer. The unused
identity-free asset-schema migration kind is not part of this contract.

Apply acquires the migration writer lock, rereads the source preamble and
fingerprint, resolves the exact edge again from the live registry, and compares
all recorded authorization fields before staging. Missing, changed, fabricated,
lossy, or codec-mismatched authorization is blocked. Successful serialization
is validated by the target codec and repeated byte-for-byte before publication.
Ordinary saves and the repository baseline remain independent of migration-
writer policy.

The existing package-bundle sidecar journal stages every selected destination
before publication. The tool validates bounded target-format decode,
byte-identical re-emission, fresh compatibility probes, and registry projections
before removing sidecars. Any decode, dependency, upgrade, serialization, staging,
publication, post-audit, cache, or registry failure compensates the complete
bundle and restores authored bytes plus runtime state. Registry entries are
published only after every package has passed post-audit; discovery, audit,
inspection, ordinary loading, and dry-run planning never rewrite content.

A permanent scoped test codec exposes the qualified v4 logical package through
an independent format identity and writer. It keeps shared dispatch, non-
ordinary mutation rejection, exact-edge execution, stale/tampered authorization,
cancellation, every apply failure phase, rollback failure, recovery, post-audit,
and registry publication covered without retaining a production legacy codec.

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
recursive DAST signature is
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
table advertises `AuthoredFieldsComplete`, unless the struct declares one
universal Archive serializer for its complete durable representation. Otherwise
save, package load, and typed inspection fail closed with
`CustomStructCodecRequired`. AssetCore never silently omits durable unreflected
state. A custom serializer still enters stable named nested fields and uses the
same logical types, so tagged DAST compatibility inspection remains possible.
No current production struct requires a separate AssetCore-only custom codec.

DAST retains the logical `Array<...>` and `Map<...,...>` signatures, count
fields, and entry payload grammar. New Map saves order entries by the canonical
logical key token from the reflection contract, so equivalent supported maps
produce identical package bytes regardless of insertion, reserve, rehash, or
bucket history. Readers do not require map entries to arrive in canonical order,
but reject duplicate decoded keys.

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

### Construct-Free Package Tooling

Complete live save/load and byte-only tooling deliberately meet at the DAST
logical value grammar rather than at object construction. AssetCore's package
Archives and tooling share the bounded scalar, container, struct, hard/soft
reference, type-signature, native-field, and canonical Map-key rules. There is
no second live-object serializer outside the Archive adapters.

Inspection, compatibility probes, registry and reference-index extraction,
redirector fixup, relocation analysis, deletion analysis, and cook
canonicalization operate on immutable package records plus a frozen reflection
catalog. They construct no inspected class, invoke no virtual serializer or
`PostLoad`, resolve no dependency merely to inspect it, publish no package, and
change no dirty state. Route-specific walkers remain where a tool must report a
stable occurrence path or preserve unknown struct payload bytes exactly; they
validate through the common grammar and never reinterpret values as C++ object
memory.

Tool readers enforce field payload bounds, overflow-safe remaining-byte checks,
canonical paths and type signatures, maximum allocation/count limits, Map-key
uniqueness, and complete consumption where required. Rewriters copy untouched
or unknown payload bytes exactly and publish atomically only after complete
validation. A corrupt input or failed rewrite produces no partial registry,
reference-index, cook, relocation, deletion, or authored-package result.

String, Name, and Guid payloads use explicit logical encodings, so their current
signatures carry an encoding version rather than `ElementSize`. Readers require
the exact current recursive signature, including inside arrays, maps, and struct
fields. Raw scalar and enum payloads are copied using `ElementSize`, so
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

`AssetRegistry/Registry.bin` is a versioned, deterministic snapshot keyed by virtual mount root and normalized mount-relative package path. Startup still enumerates mounted `.dasset` files once. Exact file-size and stable last-write-time matches reuse cached class, entry kind, redirect destination, format-version, and dependency metadata without reading package headers; new or changed files use the bounded header reader, and missing files disappear from the freshly published live map. Full validation bypasses fingerprint reuse and verifies redirector bodies. Schema, package-format, serialization, or mount-manifest incompatibility and corrupt or missing snapshots cause a non-fatal rebuild. Successful mutations update the private catalog store and dirty the snapshot, which is atomically replaced after explicit reconciliation and during orderly runtime shutdown. Public callers receive owned `FAssetCatalogEntry` values and immutable `FAssetCatalogSnapshot` values, never pointers into the store. `RefreshAssetCatalog` returns requested mode, completeness, prior/resulting revisions, catalog and reference counters, warnings, and structured errors in one value. The monotonic process-local revision advances only when published asset metadata changes, allowing editor queries to cache derived views safely. Scan diagnostics expose elapsed milliseconds, enumeration/reuse/reparse/removal/failure/redirector counts, and package-header read attempts and physical/logical bytes.

Public ownership is split by purpose: `AssetPackage.h` owns package-format and
inspection values, `AssetCatalog.h` owns immutable discovery values,
`AssetLoad.h` owns runtime resolution/residency, `AssetMutation.h` owns current
authoring operations, and `AssetTestSupport.h` owns deterministic failure seams.
No public manager or mutable catalog container is part of the contract.

`AssetRegistry/References.bin` is the single rebuildable hard, soft, and
redirect occurrence projection. Every source entry is keyed by its full package
size, stable last-write time, 128-bit content hash, DAST version, and extractor
schema. Each occurrence records source package and object identity, declaring
type, top-level field, kind, expected class, target path, a typed
fixed-array/Array/Map/struct route, and a deterministic display path. Results
sort deterministically and support target-to-referencer and deduplicated
source-to-target queries without changing package-header dependency semantics.

Incremental reconciliation treats an exact package path, file-size, and stable
last-write-time match as a trusted cheap cache hit before reading any package
payload. It carries forward the cached content hash and occurrences without
recomputing them. A writer that changes bytes while restoring both size and
timestamp can therefore remain invisible until `FullValidation`; that explicit
mode bypasses both registry and reference reuse, reads and hashes every package,
and performs complete package and reference validation. Reference-index scan
diagnostics report payload-read attempts and bytes separately from logical
reuse and extraction counts, and reset for every scan.

With a missing or invalid reference cache, reconciliation attempts one payload
read for each discovered source; an unchanged warm incremental scan attempts
none, a single metadata-visible source change attempts one, and
`FullValidation` attempts one per source. Failed opens count as attempts but add
no bytes. Explicit Content Browser Refresh still reconciles every registered
auto-scan mount through this incremental path; its current-folder scope applies
only to the visible item snapshot, not to registry discovery.

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

Asset relocation is atomic and batched even for one mapping. Preparation
captures the registry revision, exact participant fingerprints, resident-package
state, generated redirectors, and exclusively owned payload moves behind an
opaque transaction. Its immutable summary exposes the operation kind, captured
revision, and ordered source/destination scope. `Commit` prepares and journals
every output, publishes real destinations, owned payloads, source and upstream
redirectors, resident package/object names, and the complete registry projection
under one revision. `Undo` and `Redo` use the same retained transaction rather
than computing reverse moves. Invalid state transitions fail without advancing
the journal; result details distinguish restored failure from retained
recovery-required state.

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

Deletion never rewrites persistent paths. Preparation returns one opaque
deletion transaction whose immutable scope uses the unified graph and
registered stores for safety diagnostics. Alias-only deletion,
broken aliases, and target selections missing any direct/upstream alias are
blocked. Deleting a target together with its complete alias closure requires an
explicit warning; soft and external-store occurrences warn that authored paths
will dangle. Registry/store revisions, warning snapshots, exact entries, and
files are retained so `Commit`, `Undo`, and `Redo` can revalidate and restore
redirector metadata exactly. AssetCore owns final safety validation, resident
eviction, catalog removal/restoration, and compensation order around a
caller-supplied reversible physical stage/restore transition. Callers cannot
invoke unload or registry-projection phases separately.

Owned-payload relocators, deletion-companion contributors, persistent external
reference stores, and committed-only move observers register through explicit
handles. Registrations retain the module-owned resource lease, gate every
invocation against owner retirement, reject duplicate class providers, and are
removed by their exact handle. No callback may silently replace another
module's durable-state owner.

Fix Up is the only path-canonicalizing authoring transaction. It computes
upstream alias closure, rewrites tagged hard/soft package fields plus registered
external stores, verifies zero remaining incoming persistent occurrences, and
then optionally deletes the aliases. Dirty/incompatible/read-only inputs,
incomplete indexes, unavailable providers, changed fingerprints, publication
failure, or verification failure retain valid redirectors and restore every
participant.

`PrepareRedirectorFixupTransaction` returns an immutable summary of selected
aliases, final mappings, package/store occurrences, and aliases proven
deletable, plus the same opaque mutation value used by relocation. `Commit`
owns the final registry/store/fingerprint checks, journal publication,
verification, and compensation. Result details separately report rewritten,
retained, deleted, skipped, and failed paths. Fix Up is an explicit maintenance
command rather than an editor-history entry, so its transaction deliberately
does not advertise `Undo` or `Redo`; calling either is rejected without changing
the committed state.

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
