# Asset Packages

Summary: Define asset identity, package serialization, runtime residency, loading, and compatibility inspection.

Modules: AssetCore, CoreDObject

Last reviewed: 2026-08-25

Durin object assets are stored as versioned `.dasset` packages. A package has one public main asset and may contain any number of `DObject` instances arranged through the ordinary Outer hierarchy. Outer defines structural containment and object paths, not a GC strong reference.

## Public Capability Boundary

AssetCore exposes capability-named entry points rather than one complete public
surface. `Asset.h` is the ordinary runtime entry point for catalog lookup,
redirect resolution, package residency, asset loading, and cooked-payload
reading. `AssetAuthoring.h` adds package creation and persistence, mounted-source
mutation, relocation, deletion, and redirector fix-up. `AssetCook.h` adds Cook
reachability, cooked-container construction, package serialization for Cook, and
manifest publication. `AssetTools.h` adds offline inspection, compatibility, and
canonical-resave workflows.

Public Engine asset headers include narrow leaves such as
`Asset/CookedAsset.h`, `Asset/Cook.h`, and `Asset/SourcePath.h` when their type
layout or method signatures require those declarations. They do not include an
umbrella solely to obtain one value type. Callers select `PackageTypes.h`,
`PackageInspection.h`, or `PackageAuthoring.h` by capability; there is no
package compatibility aggregate.
The V4 reader, writer, archive adapter, and version policy remain AssetCore
implementation details.

## Paths And Mounts

Asset identities use extensionless `FAssetPath` values such as
`/Engine/Materials/Default` or `/Game/Levels/TestLevel`. The first path segment
must match a registered mount. `FSourcePath` uses the same logical mount and
retains the filename extension. Both types resolve relative to the mount's
single `GetContentDir()`; neither virtual path includes `Root` or `ContentPath`.

The immutable Core mount registry publishes `/Engine/` and `/Game/` plus
validated project-declared extension and external-source mounts. Every mount
may contain `.dasset` packages and ordinary authoring files. Typed resolution
reports invalid paths, unknown mounts, unavailable content directories,
escapes, missing files, forbidden dependencies, and read-only authoring policy
distinctly. `AutoScan`, explicit admission, and load-visibility projection are
defined by [Asset Catalog And Mutation](AssetCatalogAndMutation.md).

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
| Reflected `TWeakObjectPtr<T>` | Code needs a non-owning handle to an object that is already loaded, such as editor selection or a transient cache. | Must be `Transient`; stores no durable asset identity, is omitted from authored/cooked packages, does not retain the target, and becomes invalid when the object is retired. Weak Map values are supported; weak Map keys are not. |
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

Catalog queries, redirect diagnostics, reference projections, Cook reachability,
and transactional relocation are defined by
[Asset Catalog And Mutation](AssetCatalogAndMutation.md).

## File Format

Every authored or cooked `.dasset` is a DURF/DAST v6 object package, regardless
of its main asset class. It is the repository baseline, ordinary writer, and
only supported package format. DAST has the permanent format identity
`3c59d1a9-6ceb-4e4c-b059-452db0a5af56` and canonical diagnostic name
`Durin.BinaryFormat.DAST`. DURF carries `(FormatId, FormatVersion)` and common
integrity facts; AssetCore resolves that pair through an immutable codec
registry before DAST parses package sections. Unknown identities, unsupported
features or versions, legacy prefixes, and corrupt envelopes fail before object
construction, mutation, or publication.

Relocation preserves the package format while changing only the main-object
name when a rename requires it. DAST's Public Summary and Import sections record
the main asset class, package kind, redirect destination, dependencies, and
object count. An ordinary asset has no destination. A redirector must name
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

Domain payloads are not file formats or nested DURF envelopes. The reflected
asset class and payload slot select one codec and schema before bytes are read.
DDC, DABK, and DBLK remain opaque storage at that boundary: they do not probe
payload bytes or carry a codec tag. A cooked `.dbulk` is DURF/DBLK v2; the
`.dasset` that describes its slots remains DURF/DAST v6.

### DAST v6 Envelope Route

AssetCore implements DAST v6 under `DURF` v1 as its capability-complete
production codec. Its 32-byte format header
records package kind, zero flags, absolute directory offset, section count,
48-byte entry size, and zero reserved word. The canonical required sections are
Public Summary, Import, Name, Type, Schema, Export, Value, and Payload
Directory. Directory entries are strictly kind-ordered and contain required
flags, 64-bit contiguous extents, XXH3-128 section hashes, and zero reserved
words. Public Summary and Import end exactly at `HeaderBytes`; all sections are
contiguous through physical EOF, with no DTRL/DTRF footer authority.

Public Summary selects one-based `MainExportIndex` 1 and freezes Import,
Export, and payload counts plus class/redirect metadata. Import paths are
canonical sorted unique dependencies. Existing Name, Type, Schema, and Value
bytes are reused exactly; v5 Object bytes become Export after topology
validation, dependency IDs become one-based Import indexes, and payload
descriptors become the front-directory-owned Payload Directory. Soft paths
remain paths. The codec reconstructs an internal canonical logical object
stream to reuse the semantic engine, but that internal form is never package
wire authority or published output.

Header-only inspection validates the complete common front matter, directory
extents, and Public Summary/Import section hashes without reading later
sections. Full validation checks every section hash, exact contiguity, semantic
topology, payload descriptor equality, and canonical reconstruction before any
codec capability exposes data. Unknown required sections fail. Unknown
skippable sections are extent/hash validated for read-only validation, but save
or mutation rejects before output unless byte-exact canonical retention is
available. V6 is the supported-reader route and ordinary writer.

### DAST v6 Ordinary Route

AssetCore registers one reader-, writer-, and mutation-complete DAST v6 codec
and selects it for ordinary single-package and bundle saves. Existing file,
asset type, payload size, or environment values never change that policy.
Legacy DAST prefixes have no production reader, writer, migration, or rollback
route. External payload authority is the required front-directory Payload
Directory; v6 has no EOF trailer or footer.

### DAST Logical Object-Stream Wire Contract

The logical object stream is an internal canonical grammar carried by DAST v6
sections. AssetCore exposes production-owned low-level writer and reader
boundaries, while package policy routes header, inspection, compatibility,
reference, registry/cache, and live-load operations only through v6. Its
internal grammar version is 5 and does not identify a supported standalone
package format.
The layout below is frozen: later format changes must use a new version rather
than altering these bytes or semantics. Any required corpus conversion is a
separately planned, temporary offline tool, not a permanent runtime facility.

A object-stream package starts with bytes `44 41 53 54`, then `05 00 00 00` (`uint32`
little-endian version 5), a little-endian `uint32` public-summary byte length,
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
  is field-name id, type id, and authored-flags VarUInt. The only current authored
  flags value is zero; field ids are one-based positions in the canonical
  schema.
- Object is object count followed by length-delimited
  `(outer-object-id, class-name-id, object-name-id)` records. Id 1 is the package
  root with outer id zero. Remaining objects sort by canonical outer path,
  class-name bytes, then object-name bytes; duplicate sibling identities are
  invalid.

Every table, record, and section is completely consumed. Limits are 1,048,575
names, types, schemas, and objects; 65,535 fields per schema; 4,096 dependencies;
256 custom versions; 1,048,575 Array/Map elements; 256 MiB per Bytes value; and
nesting depth 64, all further constrained by the package-size bound. Bytes is
not charged against the element-count limit.

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
serializer without a proven object-stream codec fail closed. A loaded explicit field keeps
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
reference codec. The same production codec owns ordinary object-stream package
saves and bounded v5 reads.

### DAST Logical Object-Stream Writer Boundary

`Durin::Asset::PackageObjectStream::WritePackage` consumes only owned logical package input:
public summary values, structural type and schema descriptors, custom versions,
object topology, completed known overrides, and exact retained unknown closure
and payload bytes. It closes and canonicalizes the package-local tables before
emission, rejects malformed descriptors, topology, values, versions, retained
closures, limits, and discovery mismatches with a stable typed diagnostic, and
replaces the caller's destination only after the complete bounded package has
been assembled.

`Durin::Asset::PackageObjectStream::WriteAssetPackage` is the sole live-object integration
entry. It performs Archive discovery/emission manifest checks, consumes
`BuildDefaultDeltaPlan` in enabled or no-delta mode, and then delegates to the
low-level writer. Ordinary and atomic-bundle saves select no-delta mode so a
loaded canonical object-stream package resaves byte-identically. Relocation, redirector
Fix Up, and cook canonicalization decode the existing object-stream model, apply their
bounded rewrite, and canonically re-encode it. Inspection and loading remain
read-only.

Package serialization options select an explicit `Authored` or `Cooked` save
domain. Authored is the compatibility-preserving default. Cooked capture sets
the Archive purpose to `CookedPackage`, marks it persistent and cooking, carries
the exact target platform/profile through both discovery and value capture, and
filters `EditorOnly` fields unless the Cook context requests diagnostic
retention. The old top-level property filter remains available to audited
authored clients; Cook paths do not use it to duplicate reflected field policy.

An optional owned per-save override table changes the effective save view
without changing a live object. Entries use exact object and reflected-property
identity and may omit an object, omit one property, or provide a copied
replacement value. Registration rejects conflicting entries, foreign
properties, incompatible value type/size/alignment, and—after the package graph
is frozen—objects outside that graph. Owned reflected snapshots retain hard
reference roots for the complete operation. Effective precedence is object
omission, property omission and archive selection (including `EditorOnly`),
then replacement value, then the live value. Discovery and capture therefore
observe the same references, dependencies, fields, and logical codecs. An
omitted object must not remain reachable through a non-omitted hard reference,
and the package root cannot be omitted.

Live graph discovery, override validation, and logical value capture end at an
AssetCore-private captured-package boundary. That value owns the object
topology, effective field nodes, strings, descriptors, references,
dependencies, retained unknown values, custom versions, and default-delta plan
needed by DAST encoding. Encoding receives only that captured state plus frozen
identity metadata; it does not dereference the source package graph. The graph
and field manifests are checked across discovery and capture, and late object,
field, reference, or version changes fail before destination bytes are
published. Capture remains on the asset-owning thread under the existing
no-concurrent-edit rule. Encoding may move to a worker only after a future owner
provides task lifetime and publication scheduling for the captured value.

Retained provenance `02` inputs keep their descriptor closure and payload as
separate exact byte spans. Before publication, the writer parses all closure
framing, canonical Name/Type/Schema tables, descriptor references and cycles,
root schema/field resolution, bounds, and trailing-byte state. Package table
reordering never remaps or rebuilds those bytes.

### DAST Logical Object-Stream Reader Boundary

`Durin::Asset::PackageObjectStream::ReadHeader` validates the bounded public summary and
five-entry directory without parsing or allocating body tables.
`DecodePackageStructure` owns a temporary pointer-free logical model containing immutable
one-based ids, decoded values, raw payload locations, and separate exact
retained closure/payload spans. It validates every primitive, extent, table,
record, descriptor, topology, value, and closure without re-encoding.
`DecodePackage` is the explicit canonical-audit boundary: it performs that
decode, re-emits through the production writer, and requires byte equality.
`ReencodePackage` exposes the canonical round trip for offline maintenance and
bounded byte mutations.

`LoadAssetPackage` is the sole explicit live-reader entry. After one structure
decode, it compares every serialized class and field signature with one frozen
reflection catalog. Only a fully compatible package may create object
skeletons and Outers, resolve dependencies, and apply known
overrides through the shared authored Archive relative to constructor/class and
Struct defaults, restores loaded-explicit and forced authored intent, and calls
PostLoad in reverse object order. The move-only `FLoadedAssetPackage` handle is
the publication and lifetime owner. Decode, class, dependency, Archive, ledger,
PostLoad, or publication failure destroys the complete new graph, releases
dependencies loaded by the failed attempt, and preserves the caller's previous
handle and report.

At this bytes-to-runtime boundary, registered type and owner-scoped property
aliases are resolved before schema preflight. A recognized property alias keeps
the stored type descriptor and payload but replaces the field identity with the
current reflected name for value application and authored-intent restoration.
Compatibility inspection reports the alias as canonicalization evidence, and a
subsequent save emits only the current name. Aliases do not relax kind or type
signature checks. A schema whose distinct stored names canonicalize to the same
field is invalid and fails before object publication.

Versioned `_DEPRECATED` fields are the bounded exception for an intentional
signature or semantic change. Preflight resolves them before current-name
matching by declaring type, historical stored name, exact logical signature,
and the package's GUID-keyed Custom Version. Missing tags use `-1`; a route's
upper bound is exclusive. Consumed data loads only into the deprecated member,
then class `PostLoad` or detached-struct `PostDeserialize` converts it before
publication. Current saves automatically emit the route domain's latest value
and omit all deprecated members from schemas and deltas.

The route's `MigratesTo` list projects loaded-explicit or forced provenance to
current authored paths. Splits project to every target; merges keep forced over
explicit over absent. Compatibility records expose `DeprecatedRouteUsed`
findings and structured route evidence without marking an otherwise supported
package incompatible. That evidence makes the package eligible for canonical
resave, and successful verification requires both alias and deprecated-route
evidence to be empty afterward. Unclaimed unknown fields retain their existing
opaque behavior.

The machine-readable compatibility report is Schema v3. Each package record
always carries `canonicalizationEvidence` and `deprecatedRouteEvidence` arrays;
the latter records the object path, declaring type, historical and deprecated
field names, Custom Version GUID and bounds, and migration targets. DevTool
validates this exact contract with `asset-audit-v3.schema.json` before applying
audit or baseline policy. The prior v2 schema remains checked in as the frozen
shape that predates deprecated-route evidence.

`InspectPackage`, `ExtractReferences`, and `ProbeCompatibility` consume the
same decoded logical model without constructing objects or invoking callbacks.
Known values project to existing field/reference semantics, including intrinsic
math Struct payloads and canonical Map routes. Provenance `02` never remaps
through package tables: construct-free inspection can report its exact
`DescriptorClosure` and `RetainedPayload` spans, while ordinary live load
rejects it before residency.

The package policy routes bounded header, validation, inspection, reference,
compatibility, and ordinary live-load reads through these entries when the
envelope declares DAST v6. Reads never write or dirty a package. Unsupported versions
fail before version-specific parsing. Saves invoke the object-stream writer only after
compatibility and stale-input checks succeed.

### Production Save and Load

Object records store object id, outer id, qualified class name, object name, and
a field table. Fields are identified by declaring qualified class plus property
name and include a recursive serialized-type signature and payload size.
Signatures describe the wire encoding, not C++ object size or another
process-local ABI property. Missing fields retain constructor defaults. Unknown
classes, invalid Outer hierarchies, malformed references, truncation, and
unsupported versions fail the complete load.

DAST logical object-stream save and load are purpose-specific `FArchive` adapters. Saving runs a
discovery pass through each live object's virtual `DObject::Serialize(...)`,
freezes object ids, fields, dependencies, logical types, and version use, then
calls the same entry for emission. A derived serializer must call its base once
and can persist additional durable state only as stable named fields. A field,
dependency, type, object, or version first seen during emission fails before
file or registry publication. Repeated saves preserve the existing canonical
tables, overrides, dependencies, objects, and Map ordering.

Loading validates records and creates all object skeletons and Outer links
before applying fields. Each live object receives exactly one DAST load Archive
call; unavailable fields leave constructor defaults, while incompatible or
unknown serialized fields fail during schema preflight. Dependencies
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
only when its registered format equals the ordinary v6 writer. A stale or
unsupported registry version is rejected before serialization, staging, file
publication, registry publication, or dirty-state clearing. New packages use
the same ordinary writer. A non-current format is not an ordinary save input.

Current-format byte mutations resolve the source codec before decoding and
require its declared mutation capability. Reference fixup and relocation
accept and preserve only source v6; new redirectors and ordinary authored saves
use v6. Unsupported formats are rejected before output bytes change.
Version-specific decoded packages remain inside their
codec adapter; shared transactions consume neutral headers, inspections,
reference edges, load handles, and byte results.

### Canonical Reflected-Identity Resave

Canonical resave is planned current-format package maintenance, not reimport or
format conversion. The metadata probe records every registered legacy
class, struct, or enum identity in the package header, object records, schemas,
and recursive type descriptors. Each finding carries the package, stored and
current identity, reflected kind, stable location, and logical path. The same
evidence is attached to a live `FAssetLoadReport`; a successfully loaded package
exposes `IsCanonicalResaveRecommended()` without becoming Dirty.

`PlanAssetCanonicalResaves` deterministically selects packages, folders,
mounts, or an explicit project scope and captures the physical fingerprint,
format, entry kind, residency, Dirty conflict, compatibility state, and
evidence. It blocks stale inputs, non-current formats, incompatible payloads,
dirty loaded packages, and non-authoring mounts; non-asset entries such as
redirectors are skipped. A package with no evidence is skipped unless an
interactive caller explicitly requests a plain package resave.

Apply revalidates each fingerprint and loads through the ordinary current-format
reader when necessary. In the uncooked tool host, PostLoad may schedule
family-owned derived-data recovery; apply alone loads the required build and
built-in authoring modules, drains the asset's active recovery claim, and asks
the registered `IAssetAuthoringReadinessFeature` provider to validate transient
domain state. The generic tool does not enumerate concrete asset classes.
Recovery may read a mounted source or DDC entry, but recovery-mode AssetForge
does not publish an authored package; an authored mutation or Dirty package
blocks the subsequent canonical save.

The ready package is published as one bounded atomic unit through
`SavePackagesAtomically`. Published bytes are reread through the compatibility
probe; success requires the current writer, compatible inspection, and zero
remaining legacy identities. Verification or registry-reconciliation failure
restores the prior authored bytes. Batch admission stops at cancellation and
retains terminal results for completed units; project maintenance does not
claim project-wide atomicity.

`DurinAssetTool --operation=canonical-resave` is dry-run by default. Selection
uses `--package`, `--folder`, `--mount`, or explicit `--project-scope`; `--apply`
writes, `--format=human` selects a compact human report, and the default is a
deterministic JSON report. Canonical resave always targets v6; no format-selection
or legacy rollback option exists.
The plan records the target format and rejects stale fingerprints before each
atomic unit. `--ci` is read-only, cannot be combined with apply,
and fails when selected compatible content still has registered legacy
identities.

There is no general package migration command or registered migration graph.
If real non-current content requires conversion, its owning plan must introduce
a narrowly scoped offline converter, qualify the complete corpus and restart
boundary, and remove the converter after the baseline becomes current.

## Structure Compatibility

Ordinary load captures the current reflection catalog and rejects an unknown
class, unknown field, or mismatched field signature before object skeleton
construction. No partially compatible package becomes resident, no retained
legacy payload is attached to live state, and save has no data-loss escape
hatch. Editor document opens receive the same structured load failure and keep
their prior active document or world unchanged.

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
- `AssetCore` owns `.dasset` I/O, the synchronous asset registry, package caching, dependency loading, construct-free compatibility reports, strict schema preflight, DDC storage, and cooked container/publication primitives.
- `AssetBuildCore` owns only family-neutral cache values/policy, opaque DDC access, and the authoring build-host lifecycle; it does not own typed recipes or a generic executor.
- `Engine` owns asset-specific source provenance, import/build policy, derived-data keys and codecs, and cook contributions.
- Editor modules invoke the descriptor-based `FImportService` for initial
  import, single-asset reimport/repair, and record-backed multi-output actions,
  as documented in [Asset Import Framework](../../Editor/Architecture/AssetImportFramework.md).
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
package format. An engine release rewrites a package only through an explicit
current-format canonical resave or a separately planned temporary corpus
converter.

## Authored bulk companions

### Construct-free payload inspection

`FAssetPackageInspection` records the inspected physical package path and can
decode the tagged fields of a reflected struct through
`FAssetPackageField::TryInspectStructFields` without constructing its C++ value
or resolving nested storage. Editor bulk descriptors use the explicit
`TryReadEditorBulkDataStorageDescriptor` name and validate inline size/hash as
well as external identity. `EditorBulkDataStorage` can enumerate descriptors,
referenced companions, and unreferenced same-package companions read-only.

The texture domain composes these physical facts through
`InspectTexturePayloadPackage`. AssetCore does not interpret dimensions, pixel
formats, voxel counts, domain schema versions, or repair policy. Inspection may read and
validate a referenced companion, but it never publishes, restores, removes, or
rewrites a file; those remain explicit package/source/Cook workflows.

The DAST logical object stream gives authored bulk values their own `BulkData` opcode. The Value
section contains payload id, 16+4 compatibility-reserved bytes, logical and
stored byte counts, XXH3-128 content hash, placement, and container hash.
Readers accept historical nonzero reserved values from the superseded semantic
descriptor experiment and ignore them; current writers emit zero. Canonical
resave is the supported migration without changing the opcode layout.
Values below 256 KiB carry a normal bounded inline Blob after the descriptor;
values at or above 256 KiB carry no payload bytes in DAST.

The reflected UE-style `FEditorBulkData` value composes AssetCore's
storage-neutral `FBulkData` with editor-side atomic replacement. The value
contains only payload id, logical byte count, content hash, and immutable
resident bytes; it has no semantic format, authority, provider, residency,
placement, or container state. Package Archive capture creates the authored
descriptor that adds stored size, placement, and container hash for the
DAST/DABK transaction. Payload meaning and schema versions belong to the
reflected owning domain.

External authored bytes live beside the package as
`<package-stem>.dabulk`. The stable filename is discovery only: the package
descriptor and v6 Payload Directory remain authoritative for the container hash. This is
distinct from cooked `.dbulk`.
The companion is DURF/DABK v2 with permanent identity
`49efbbb4-e2434e35-a7c01c34-9ed84ea0`, a 64-byte format header, sorted 64-byte
entries, 16-byte payload alignment, at most 65,536 unique payload ids, and a
1 GiB file/payload ceiling. Readers reject invalid identity/version/features,
duplicate or unordered ids,
misalignment, overlap, gaps, nonzero padding, bounds overflow, size/hash
mismatch, wrong container identity, and trailing bytes.

AssetCore's bounded container mechanism supplies DURF discovery,
little-endian IO, checked alignment, canonical ordering, zero padding, safe
range projection, and layout validation. DABK owns its descriptors, suffix,
recovery, and publication transaction; the reflected asset owns payload meaning
and schema.

Save constructs and validates the replacement companion, copies any prior
stable companion to `<package-stem>.dabulk.durin-backup`, and publishes the
companion before the package. A synchronous package or bundle failure restores
the prior companion before returning. Live payload load validates the stable
file against the package descriptor; on mismatch it may restore only a complete
backup with the exact expected container hash. A matching stable file commits
recovery and removes a stale backup. Construct-free inspection is read-only and
internal backup and atomic temporary files never enter orphan or submit closure.
Cleanup runs only after package, companion, and catalog publication verify.
Relocation journals the stable companion as owned payload, and deletion
discovers it from package descriptors. Generation-named authored companions are
unsupported after corpus migration. Referenced `.dabulk` files are authored
source and must be submitted with their `.dasset`; repositories must not ignore
the suffix wholesale.

## Related Asset Data Contracts

Platform payload DDC objects, cooked DBLK companions, deterministic publication,
and authored-versus-cooked runtime policy are defined by
[Asset Data Lifecycle and Storage](AssetDataLifecycle.md). Asset-specific build
and payload details remain with their owning contracts, including
[Texture System](../Rendering/TextureSystem.md) and
[Static Mesh Rendering](../Rendering/StaticMeshRendering.md).

Thumbnail keys, persistence, scheduling, lifetime, and presentation are defined
by [Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md). Repository
storage rules for packages, sources, caches, and cooked output are defined by
[Content Version Control](../../Development/VersionControl/ContentVersionControl.md).
