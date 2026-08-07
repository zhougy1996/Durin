# Unified Archive Serialization Plan

Summary: Unify live `DObject` and reflected-value state transfer behind one `Serialize(FArchive&)` entry with purpose-specific Archives while preserving DAST v3 compatibility and byte-only tooling.

Last reviewed: 2026-08-07

Status: Completed
Completed: 2026-08-07

## Current Status

- Stages 0 through 6 are complete. Stage 6 started from the shared-codec
  consolidation commit `4aa4543d`; the architecture investigation baseline
  remains `897a12a0`.
- `DObject::Serialize(FArchive&)` is virtual and its base implementation walks
  reflected non-`Transient` properties. Transient object-graph, duplication,
  and DAST v3 package save/load paths call it exactly once per live object.
- `FArchive` now exposes direction, purpose, capabilities, version context,
  logical field types, structured and container path scopes, semantic stable
  value operations, and first-failure diagnostics. Object graph, duplication,
  snapshots, and editable copy use purpose-specific reference semantics with no
  generic process-address encoding.
- Object-graph v2 discovers serialized hard references through the same virtual
  `DObject::Serialize` calls used for emission, retains structural Outer
  traversal, freezes IDs before writing, and excludes raw, soft, and hidden
  GC-only references.
- DAST v3 inspection, reference indexing, redirector fixup, compatibility
  reporting, and cook canonicalization intentionally operate on field records
  without constructing live objects. They remain byte-level consumers in the
  selected architecture.
- The Stage 0 consumer inventory, public API names, purpose matrix, DAST v3
  migration corpus, object-graph v1 cut, diagnostic grammar, and AssetCore
  error translation are frozen below. No unresolved Archive contract decision
  remains for Stage 1.
- The plan is a completed architecture prerequisite of the
  [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md).
  It does not start DAST v4, change the v3 wire format, or authorize content
  migration.

## Goal

Make `DObject::Serialize(FArchive&)` the single entry used whenever live object
state is discovered, saved, loaded, or remapped. Different Archive
implementations must provide object-graph, duplication, snapshot, and authored-
package semantics without duplicating the reflected value walk.

The migration must preserve DAST v3 bytes and compatibility behavior exactly,
replace the transient object-graph format with an explicitly versioned v2
contract, and leave byte-only package tooling construct-free and worker-safe.

## Scope

- A CoreDObject-owned Archive state, capability, structured-field, logical-value,
  object-reference, version-context, and failure contract.
- One virtual `DObject::Serialize(FArchive&)` path for reflected properties and
  explicitly named native fields.
- Purpose-specific discovery, object-graph, duplication, snapshot, DAST save,
  and DAST load Archives.
- A shared reflected-value serializer for scalar, enum, Name, Guid, string,
  object, soft-object, struct, Array, Map, fixed-array, and nested values.
- Universal `FDStructOps::Serialize` dispatch through the same Archive contract,
  with reflected-field fallback and transactional load repair.
- DAST v3 save/load adapters that call live-object `Serialize` while preserving
  field records, dependencies, compatibility reports, exact unknown payloads,
  data-loss consent, and deterministic bytes.
- Shared DAST logical value codecs used by package Archives and byte-only
  inspection, reference extraction, canonicalization, and fixup.
- Focused and end-to-end validation across CoreDObject, AssetCore, Engine,
  editor package workflows, and the full build.

## Non-Goals

- Defining or implementing DAST v4 tables, default-relative encoding,
  compression, multi-version readers, migration tooling, or repository content
  resaves.
- Preserving the transient object-graph v1 byte format or adding a v1 reader;
  that internal format is replaced atomically by v2.
- Routing GC marking through `Serialize`, or replacing GC schemas,
  `AddReferencedObjects`, weak-reference behavior, or destruction ownership.
- Constructing live objects during package inspection, registry scanning,
  reference indexing, redirector fixup, deletion analysis, or cook
  canonicalization.
- Network replication, delta serialization, text import/export, SaveGame policy,
  hot reload, async package loading, or editor transaction format changes.
- Allowing arbitrary opaque authored bytes without a stable field identity,
  logical type, bounds, reference semantics, inspection behavior, and explicit
  version policy.
- Persisting a general GUID-keyed custom-version table in DAST v3. The Archive
  API may carry custom versions, but DAST v4 owns their authored wire contract.

## Design Decisions and Invariants

### Ownership and Module Boundary

- CoreDObject owns `FArchive`, archive state and capabilities, structured field
  descriptors and scopes, typed value operations, reflected property walking,
  object-graph Archives, duplication Archives, snapshot Archives, and struct
  serializer dispatch.
- AssetCore owns package discovery/save/load Archives, DAST v3 field adaptation,
  package object IDs, external paths, dependency collection, compatibility
  reports, legacy-field migration context, and byte-only package tooling.
- CoreDObject never includes AssetCore types. Package-specific errors are
  translated from the CoreDObject Archive failure into `FAssetResult` at the
  AssetCore boundary.
- DHT may generate or validate typed reflection metadata needed by the common
  value layer, but generated classes do not receive format-specific save/load
  functions.

### One Live-Object Entry

- Every operation that reads, writes, remaps, or discovers serialized live
  `DObject` state calls `Object->Serialize(Archive)`.
- `DObject::Serialize` remains the default reflected-property implementation.
  A derived override calls `Super::Serialize(Archive)` exactly once, then emits
  additional native state only through stable named fields and semantic value
  operations.
- The Archive tracks entry into one object and whether the base reflected walk
  completed. Missing `Super::Serialize`, duplicate base calls, duplicate field
  identities, nested object entry, and unclosed fields fail deterministically.
- Save and discovery calls are logically const even though the historical
  signature accepts a non-const object. A serializer must not mutate object
  state, allocate persistent children, load assets, publish work, or depend on
  invocation count.
- A serializer may run once for discovery and again for emission. The same
  object state and archive policy must expose the same field set, logical types,
  custom versions, and references in both passes; late discovery is an error.
- Construction, object skeleton creation, Outer assignment, package
  publication, `PostLoad`, and rollback remain consumer-owned lifecycle steps,
  not hidden behavior of `DObject::Serialize`.
- Property snapshots and editable-property copies operate on values rather than
  a complete object, so they share the value Archive layer without pretending
  to invoke the object-level entry.

### Archive State and Capabilities

- Direction and purpose are separate. The state identifies Load or Save plus a
  purpose such as Discovery, ObjectGraph, Duplicate, PropertySnapshot, or
  AuthoredPackage.
- Capability queries, not `dynamic_cast`, control named fields, raw bytes,
  canonical Map order, object-reference kinds, unknown-field retention,
  remaining-byte checks, custom versions, and multi-pass discovery.
- `IsLoading()` and `IsSaving()` remain convenience queries. Discovery is
  save-like and additionally reports `IsDiscovering()` so custom serializers do
  not invent separate discovery-only state.
- Every Archive has one sticky first failure, a structured path stack, and an
  optional format identity/version context. A later operation cannot clear or
  replace the first failure.
- The version context separates transient object-graph, snapshot, and DAST
  package versions from the engine release version. DAST v3 Archives expose the
  package format version but reject registration of a nonempty authored custom-
  version table because v3 has no canonical location for it.
- No Archive implementation may silently reinterpret an unsupported operation.
  Missing capabilities fail before destination mutation or output publication.

### Structured Fields and Typed Values

- The unconstrained `operator<<(T&)` that writes `sizeof(T)` bytes is removed.
  Stable overloads or constrained adapters cover signed and unsigned integers,
  floating-point values, bool, strings, Name, Guid, enums, object references,
  soft-object paths, reflected structs, Arrays, Maps, and supported nesting.
- Explicit raw-byte serialization remains available for bounded runtime payloads
  and declared authored byte fields. An authored Archive rejects raw bytes
  outside an active field with a stable logical byte type.
- `FArchiveFieldDescriptor` identifies a field by declaring qualified type,
  stable field name, logical recursive type descriptor, array dimension, and
  relevant property flags. C++ offsets, `sizeof`, padding, property addresses,
  registration order, and RTTI are not persistent identities.
- Reflected fields derive descriptors from `FProperty`. Native fields must
  declare descriptors explicitly; an unlabelled `Archive << NativeValue` is
  valid only for a non-structured runtime Archive.
- Field scopes are RAII and balanced. Runtime Archives may ignore field names
  after validating balance; authored Archives map scopes to DAST field records
  or field lookup.
- `SerializeDObjectProperties` becomes the base implementation over the common
  field/value layer. Format-specific recursive property switches do not remain
  in both CoreDObject and AssetCore.
- Archive policy owns filters such as `Transient`, editable-only copying, and
  `FAssetPackageSerializationOptions::PropertyFilter`; a filter cannot change
  stable field identity or make discovery and emission disagree.

### Struct Serialization

- `FDStructOps::Serialize(FArchive&, void*)` is archive-universal. A custom
  serializer replaces exactly one complete reflected struct field walk for all
  Archive purposes and must obey the same named-field and capability rules as
  object serialization.
- A struct without a custom serializer uses its reflected non-`Transient`
  fields. `AuthoredFieldsComplete` remains the fail-closed assertion that this
  fallback represents all durable authored state.
- There is no separate runtime-only custom serializer and authored custom codec
  callback. Existing test-only serializers are migrated to the universal
  contract; no production serializer requires compatibility shims.
- Struct loads continue to decode into managed detached storage and commit only
  after the complete serializer or fallback field walk and optional
  `PostDeserialize` succeed.
- Hidden GC references remain declared through `CollectReferences`; a custom
  serializer does not implicitly change GC reachability.

### Object Graph and Reference Semantics

- `AddReferencedObjects` and compiled GC schemas define GC reachability only.
  They no longer discover a serialized object graph.
- A discovery Archive calls the same `DObject::Serialize` path as emission and
  observes only references that the selected persistence policy will serialize.
- Runtime object-graph scope starts at the root, includes required Outer chains
  and structural descendants, and recursively includes supported serialized
  hard references. Raw and soft references retain their documented exclusion
  and identity semantics.
- Authored package object scope remains the main asset's Outer tree. A hard
  reference inside that tree uses an internal object ID; an external hard
  reference may target only another package's main asset and adds a dependency;
  a soft reference stores only its path and adds no dependency.
- Duplication uses an Archive whose object-reference operation remaps references
  in the duplicated Outer tree and preserves external references. It does not
  serialize process pointer values as a persistent representation.
- Discovery freezes object IDs, fields, dependencies, logical types, and version
  use before emission. Any new object, field, dependency, type, or version seen
  during emission fails the operation without publishing output.

### DAST v3 and Byte-Only Tooling

- The new AssetCore Archives adapt structured field scopes to the existing DAST
  v3 object and field records. A same-state v3 package produced before and after
  migration must be byte-for-byte identical.
- Package load constructs and registers all object skeletons, assigns Outers,
  resolves dependencies, and then calls each object's `Serialize` with a DAST
  load Archive. Missing fields retain constructor defaults; incompatible or
  unknown fields enter the existing legacy-field and compatibility pipeline.
- Field payload bounds, trailing-byte checks, container transactionality,
  post-deserialize repair, package rollback, reverse `PostLoad`, dirty state,
  and compatibility-risk save rejection remain unchanged.
- DAST inspection, reference extraction, redirector fixup, canonicalization,
  deletion analysis, and cook consume serialized records through one shared
  AssetCore logical value codec. They do not call `DObject::Serialize`, because
  no live object exists and worker execution must remain side-effect-free.
- Package Archives and byte-only tools may have different control flow, but
  they must not implement independent logical encodings. One codec owns each
  DAST scalar, container, struct, and reference payload grammar.
- General custom-version persistence is deferred to the DAST v4 wire-contract
  plan. Until then, authored native fields use versioned logical type
  descriptors and existing legacy-field upgrade machinery.

### Format, Failure, Thread, and Ordering Policy

- DAST remains version 3 and retains exact current bytes throughout this plan.
  A byte change is a failed gate, not an implicit package-format update.
- The transient object-graph format advances from v1 to v2 when the typed
  Archive encoding lands. There is no v1 reader or migration path because the
  format is process-local engine plumbing and tests; the version constant and
  fixtures change in the same commit.
- Property snapshot bytes are process-local and unversioned; snapshots cannot
  cross process restart, module reload, or engine version boundaries.
- Live-object discovery, save, load, duplication, and `PostLoad` execute on the
  game thread. Existing byte-only inspection and compatibility work may execute
  on workers from frozen reflection catalogs and immutable bytes.
- Object records retain deterministic root/Outer ordering. Field identity, not
  C++ invocation order, owns authored compatibility; DAST v3 emission preserves
  its current deterministic record order for exact-byte parity.
- Any Archive, schema, reference, version, bounds, late-discovery, or callback
  failure aborts the owning operation. Failed package loads roll back the whole
  package; failed saves publish no file or registry state; failed duplicates and
  object-graph loads retire every constructed object.

## Current Foundations and Gaps

### Foundations

- `DObject::Serialize` already provides one virtual object hook and a reflected
  base implementation.
- `FArchive` already has load/save direction, sticky failure, bounded memory
  reads, string handling, object-reference customization, and canonical Map
  ordering.
- `SerializeReflectedPropertyValue` already covers the current reflected value
  set, transactional container loading, struct custom serializers, and
  post-deserialize repair for runtime Archives.
- Object graph and duplication already construct object skeletons before
  resolving references and already call `DObject::Serialize` for value transfer.
- DAST v3 already has stable field identities, type signatures, bounded
  payloads, exact unknown retention, compatibility reports, deterministic Map
  order, dependency tables, and failure-atomic file publication.
- Package inspection and reference indexing already operate without live
  objects, providing the required worker-safe tooling boundary.

### Gaps

- The generic raw `operator<<` exposes C++ layout as the default encoding and
  cannot support purpose-specific semantic dispatch.
- AssetCore duplicates the complete scalar/container/struct/reference property
  switch instead of adapting the common Archive value layer.
- Package save/load bypass `DObject::Serialize`, so native object overrides are
  invisible to authored persistence.
- Object-graph discovery uses GC reference collection rather than the emitted
  serialization path.
- Archive mode conflates direction with purpose and has no structured field,
  logical type, field path, discovery, or format-version context.
- There is no production `DObject::Serialize` override or authored custom
  serializer test proving `Super::Serialize`, native fields, versions,
  references, and error propagation across Archive purposes.
- DAST package Archives do not exist, so package dependencies, compatibility,
  field filtering, and unknown retention are coupled to one monolithic
  `AssetSystem.cpp` implementation.

## Stage 0 Frozen Contract

This section is migration input, not a second lasting runtime specification.
Stage 6 moves the implemented contract to the owning Runtime documentation.

### Consumer Inventory

| Current consumer | Selected purpose and direction | Owner / thread | Live objects and visible fields | Format and failure owner | Validation owner |
| --- | --- | --- | --- | --- | --- |
| `SaveObjectGraphToMemory` / `LoadObjectGraphFromMemory` | `ObjectGraph`, Save / Load | CoreDObject / game thread | Constructs on load; all reflected non-`Transient` fields; `TObjectPtr` edges only | Object graph v1 today, v2 in Stage 2; CoreDObject retires failed graphs | `CoreDObjectTests` object-graph cases plus Engine SkyBox graph cases |
| Object-graph scope discovery | `Discovery`, Save | CoreDObject / game thread | Today calls `AddReferencedObjects`; Stage 2 calls `Serialize`; structural Outer descendants remain explicit | No emitted format; first Archive failure aborts before emission | Stage 2 discovery/emission parity and GC-only exclusion cases |
| `DuplicateObjectGraph` | `Duplicate`, Save then Load | CoreDObject / game thread | Constructs/reuses duplicate skeletons; all reflected non-`Transient` fields; remaps duplicated-tree `TObjectPtr` and shares external references | Process-local value stream; CoreDObject retires the duplicate root on any failure | CoreDObject plus Engine material, spline, world, and PIE tests |
| `CapturePropertyValue` / `RestorePropertyValue` | `PropertySnapshot`, Save / Load | CoreDObject / game thread | No complete object construction; one explicitly selected value; hard and raw object values use a rooted reference table | Process-local unversioned bytes; snapshot operation owns failure and destination commit | `ZPropertyValueSnapshotTests` |
| `CopyEditableObjectProperties` | `EditableCopy`, Save then Load | CoreDObject / game thread | No complete object construction; reflected `Edit` and non-`Transient` fields only; remaps wrapped hard references | Process-local value stream; CoreDObject owns transactional property failure | CoreDObject value tests plus Details panel and PIE apply tests |
| `BuildPackageBytes`, `SerializeAssetPackageBytes`, single save, bundle save, relocation/import/cook callers | `AuthoredPackage`, Discovery then Save | AssetCore / game thread | Live package Outer tree; reflected non-`Transient` fields selected by package filter; Stage 3 calls each object's `Serialize` | DAST v3; AssetCore owns dependency/type policy and atomic publication | `AssetPackageTests` plus import, material, texture, mesh, environment, and cook suites |
| `FAssetManager::LoadPackageInternal` | `AuthoredPackage`, Load | AssetCore / game thread | Constructs all skeletons and Outers first; matched fields load, missing fields retain constructor state, unmatched fields enter compatibility handling | DAST v3; AssetCore owns dependency loads, rollback, compatibility, dirty state, and reverse `PostLoad` | `AssetPackageTests` load, compatibility, truncation, and rollback cases |
| `InspectAssetPackage`, compatibility probe, registry scan, reference extraction/index, redirector fixup, relocation/deletion analysis, and cook canonicalization | Byte-only DAST codec; no live Archive purpose | AssetCore / caller or worker over immutable bytes | Constructs no `DObject`; sees all stored field records including unknown payloads | DAST v3; each operation owns bounded diagnostics and publication | AssetCore package/compatibility/reference/cook tests and editor source-reference consumers |
| `FDStructOps::Serialize` and reflected struct fallback | Inherits enclosing Archive purpose and direction | CoreDObject / enclosing thread | One detached struct value on load; custom serializer replaces exactly one complete reflected struct walk | Enclosing format/version; Archive failure aborts commit before `PostDeserialize` publication | CoreDObject struct/value tests and AssetCore math/incomplete-struct cases |

The inventory deliberately treats application call sites of
`SerializeAssetPackageBytes` as clients of the one AssetCore package builder,
not as independent serialization formats. `AddReferencedObjects` and compiled
GC schemas remain a separate GC consumer and receive no Archive purpose.

### Public CoreDObject Names and Signatures

Stage 1 uses these names. Renaming or adding an alternate public abstraction
requires updating this plan before implementation.

```cpp
enum class EArchiveDirection : uint8 { Load, Save };
enum class EArchivePurpose : uint8 {
    Discovery, ObjectGraph, Duplicate, PropertySnapshot, EditableCopy,
    AuthoredPackage
};
enum class EArchiveCapability : uint32 {
    None, StructuredFields, RawBytes, CanonicalMapOrder, ObjectReferences,
    SoftObjectReferences, UnknownFieldRetention, RemainingPayload,
    CustomVersions, MultiPassDiscovery
};
enum class EArchiveObjectReferenceKind : uint8 { Null, Internal, External };

struct FArchiveState {
    EArchiveDirection Direction;
    EArchivePurpose Purpose;
    EArchiveCapability Capabilities;
};
struct FArchiveLogicalTypeDescriptor;
struct FArchiveFieldDescriptor {
    FName DeclaringType;
    FName Name;
    FArchiveLogicalTypeDescriptor LogicalType;
    uint32 ArrayDimension;
    EPropertyFlags PropertyFlags;
};
struct FArchiveFormatVersion {
    FName Format;
    uint32 Version;
};
struct FArchiveCustomVersion { FGuid Key; int32 Version; };
struct FArchiveVersionContext;
enum class EArchiveFailureCode : uint8;
struct FArchiveFailure {
    EArchiveFailureCode Code;
    std::string Path;
    std::string Message;
};

class FArchiveObjectScope;
class FArchiveFieldScope;
class FArchivePathScope;
class FArchive {
public:
    explicit FArchive(FArchiveState State,
                      FArchiveVersionContext Versions = {});
    bool IsLoading() const;
    bool IsSaving() const;
    bool IsDiscovering() const;
    EArchivePurpose GetPurpose() const;
    bool HasCapability(EArchiveCapability Capability) const;
    const FArchiveVersionContext& GetVersionContext() const;
    const FArchiveFailure* GetFailure() const;
    void Fail(EArchiveFailureCode Code, std::string_view Message);
    FArchiveObjectScope EnterObject(DObject& Object);
    FArchiveFieldScope EnterField(const FArchiveFieldDescriptor& Field);
    FArchivePathScope EnterFixedArrayElement(uint64 Index);
    FArchivePathScope EnterArrayElement(uint64 Index);
    FArchivePathScope EnterMapKey(uint64 Index);
    FArchivePathScope EnterMapValue(uint64 Index);
    void MarkBaseReflectedFieldsSerialized();
    virtual void SerializeRawBytes(std::span<std::byte> Bytes);
    virtual void SerializeObjectReference(DObject*& Value);
    virtual void SerializeSoftObjectPath(FSoftObjectPath& Value);
    virtual uint64 GetRemainingPayloadBytes() const;
};

void SerializeReflectedPropertyValue(FArchive&, FProperty&, void*, uint32);
void SerializeDObjectProperties(FArchive&, DObject&);
```

`FArchiveLogicalTypeDescriptor` is an owning recursive descriptor with stable
factory functions named `Scalar`, `Enum`, `String`, `Name`, `Guid`, `Bytes`,
`Object`, `SoftObject`, `Struct`, `Array`, `Map`, and `FixedArray`. It contains
logical signedness/width or floating width, qualified enum/struct/reference
type, recursive element/key/value descriptors, and the authored native-field
version where applicable. It never contains an offset, address, `sizeof`
aggregate, padding, RTTI name, or reflection registration index.

`FArchiveObjectScope`, `FArchiveFieldScope`, and `FArchivePathScope` are
move-only RAII types. Path scopes supply the frozen diagnostic grammar's
`Fixed`, `Array`, `MapKey`, and `MapValue` indices without introducing a second
field identity. A failed scope remains balanced but cannot make later
operations replace the first failure. Stable primitive `operator<<` overloads
remain for the supported integer and floating widths, `bool`, `FName`, `FGuid`,
strings, enums, and the supported container adapters; there is no unconstrained
template fallback.

### Purpose and Reference Matrix

| Value or policy | ObjectGraph | Duplicate | PropertySnapshot | EditableCopy | AuthoredPackage |
| --- | --- | --- | --- | --- | --- |
| Reflected non-`Transient` field | Yes | Yes | Caller-selected value | Only `Edit` | Yes, subject to package filter |
| `Transient` field | No | No | Allowed only when explicitly selected | No | No |
| Wrapped hard object reference | Serialized; grows discovered v2 scope | Remap internal, share external | Serialized and rooted | Remap through supplied map | Internal ID or external main-asset path and dependency |
| Raw object pointer property | Excluded | Excluded | Explicit snapshot only | Excluded | Rejected |
| Soft object reference | Path only; never grows scope | Path only; cache not copied | Path plus no live-cache identity | Path only | Path only; no dependency |
| Weak/cache state inside soft reference | Excluded | Excluded | Excluded | Excluded | Excluded |
| Hidden native/GC-only reference | Excluded from v2 | Excluded unless emitted as a named native field | Out of scope unless selected value contains it | Excluded | Excluded unless emitted as a named native field |
| Fixed array | Element order | Element order | Element order | Element order | One field payload, element order |
| Struct / nested struct | Universal custom serializer or reflected fallback | Same | Same | Same | Same, requiring complete authored fallback |
| `Array` | Count then element order | Same | Same | Same | Same, bounded and transactional on load |
| `Map` | Archive-selected canonical order | Same | Logical equality; canonical bytes when requested | Same | Canonical key-token order |
| Raw bytes | Only an explicit bounded runtime operation | Same | Explicit bounded value only | Not used by reflected copy | Only inside an active `Bytes` field |

Discovery is the Save column of the selected purpose with
`IsDiscovering() == true`; it exposes exactly the same fields, types,
references, filters, and version use as emission. ObjectGraph v2 additionally
includes required Outer chains and structural descendants. AuthoredPackage
scope remains only the main asset's Outer tree and therefore does not grow from
external hard references.

### DAST v3 Migration Corpus

DAST v3 remains native-little-endian and retains its current same-platform byte
contract. Stage 3 compares the old writer and the package Archive writer in the
same test process before removing the old path; equality is over the complete
byte vector, not a parsed projection or hash. The following existing scenarios
are the frozen pre-migration corpus:

| Contract | Frozen evidence |
| --- | --- |
| Scalar, string, Name, Guid, fixed/nested values, `Array`, canonical `Map`, reflected struct, internal child, and external hard reference | `SavesLoadsContainersReferencesAndRegistryMetadata`, `PreservesMathStructBitsAcrossDastAndObjectGraphs`, `DastMapBytesAreCanonicalAcrossInsertionAndBucketHistory`, `WriterUsesVersionedWireSignaturesForLogicalEncodings` |
| Soft direct/fixed/Array/Map values, null and missing paths, and no dependency/load side effect | `DastSoftFieldsRoundTripWithoutHardDependenciesOrTargetLoads` and `DastSoftReaderRejectsMalformedTagsBoundsAndPaths` |
| Unknown and incompatible fields with exact payload retention, constructor state, risk, data-loss consent, and dirty policy | `ReportsUnknownFieldsWithoutMarkingPackageDirty`, `RegisteredSafeCleanupProducesStructuredReportAndDirtyPackage`, `KeepsRawScalarWidthInSerializedSchema`, and compatibility-probe cases |
| Canonical Maps, nested reference extraction, inspection, redirector fixup, and cook rewrite | `DastReadsNoncanonicalMapOrderAndRejectsDuplicateDecodedKeys`, soft inspection/reference cases, `RedirectorFixupRewritesHardSoftAndExternalOccurrencesBeforeDeletion`, and `CookCanonicalizesRedirectedRootsReferencesAndPublishedBytes` |
| Bounds, trailing bytes, malformed tags/IDs, dependency/type failures, and whole-package rollback | truncation, malformed soft payload, duplicate-key, redirected hard-reference, and package-load rollback cases in `AssetPackageTests` |

The corpus must be run before the Stage 3 writer switch and again in the commit
that removes the old writer. Representative tracked packages are compared by
complete bytes with `SerializeAssetPackageBytes`; no repository content is
resaved. A newly supported logical type cannot join DAST v3 during this plan
without first adding a pre-migration old-writer fixture and updating this
table.

### Object-Graph v1 Cut

The v1 semantic fixture is
`ObjectGraphSerializationRoundTripsScalarStringAndObjectReference`, augmented
by `PreservesMathStructBitsAcrossDastAndObjectGraphs` and Engine SkyBox graph
tests. It freezes magic `0x4E524F44`, version `1`, root/object IDs, Outer-first
construction, scalar/string/Name/Guid/enum/container/struct value behavior,
wrapped hard-reference restoration, raw-reference exclusion, `Transient`
exclusion, and failure on malformed/truncated input.

V1 scope is intentionally *not* a compatibility promise: discovery walks
Outer descendants plus `AddReferencedObjects`, so hidden strong GC references
may produce otherwise empty object records. V2 changes the version to `2`, has
no v1 reader, discovers through the same `Serialize` calls used for emission,
retains structural Outer descendants, includes emitted wrapped hard-reference
edges, and excludes raw, soft, and hidden GC-only edges. Tests that assert the
v1 version or GC-derived scope change in the same Stage 2 commit.

### Diagnostics and AssetCore Translation

The canonical diagnostic path is:

```text
Object[<id>:<qualified-class>:<object-path>].Field[<declaring-type>::<name>]
  .Fixed[<index>].Struct[<qualified-type>::<field>]
  .Array[<index>].MapKey[<entry-index>].MapValue[<entry-index>]
```

Only present segments are emitted, on one line without whitespace. Native and
reflected fields use the same `Field` segment. A discovery failure may use `?`
for an object ID not yet frozen. The stable message form is
`ArchiveFailure:<FailureCode>:<Path>: <detail>`. `FArchiveFailure` retains the
code, path, and detail separately; string formatting occurs at the boundary.

AssetCore translates the first Archive failure as follows:

| Archive failure | `EAssetError` |
| --- | --- |
| Unsupported capability/type/raw authored bytes, malformed serializer, missing or duplicate base walk, duplicate field, unbalanced scope, or late discovery during Save/Discovery | `UnsupportedProperty` |
| Unsupported package/custom version | `UnsupportedVersion` |
| Truncation, bounds, trailing payload, invalid wire tag, duplicate decoded key, or malformed scope observed while loading bytes | `CorruptFile` |
| Invalid/missing internal object ID, Outer relation, or reference kind inconsistent with the frozen package graph | `InvalidObjectGraph` |
| Invalid external or soft path | `InvalidPath` |
| Missing external hard target | `MissingDependency` |
| Field/reference runtime type mismatch | `TypeMismatch` |
| Object/struct serializer or repair callback rejection without a more specific classification | `UnsupportedProperty` on Save/Discovery; `InvalidObjectGraph` on Load |

AssetCore prepends no second path. It preserves the formatted Archive failure
as `FAssetResult::Message`; package-level context may append a separate sentence
after it. Publication, compatibility, dirty-state, rollback, and dependency
errors that arise outside the Archive retain their existing AssetCore codes.

## Implementation Stages

### Stage 0: Freeze the archive contract and migration baseline

- [x] Inventory every `FArchive`, `DObject::Serialize`, reflected-value,
  `FDStructOps::Serialize`, object-reference, package save/load, snapshot,
  duplication, inspection, fixup, and reference-index call site and classify it
  by direction, purpose, thread, object construction, field visibility, and
  failure owner.
- [x] Freeze public names and signatures for Archive state, capability queries,
  field descriptors/scopes, logical type descriptors, version context, object-
  reference operations, and structured diagnostics.
- [x] Record the exact supported property/reference matrix for ObjectGraph,
  Duplicate, Snapshot, EditableCopy, and AuthoredPackage purposes, including
  raw, hard, soft, weak, hidden, transient, fixed-array, nested struct, Array,
  and Map cases.
- [x] Add or freeze DAST v3 golden package bytes covering every supported
  logical property shape, nested references, unknown fields, canonical Maps,
  and constructor defaults before replacing AssetCore serialization.
- [x] Freeze object-graph v1 semantic fixtures and document the deliberate v2
  cut, including scope changes caused by replacing GC discovery with Serialize
  discovery.
- [x] Decide and record the exact CoreDObject-to-AssetCore error translation and
  field-path diagnostic format before public Archive ABI work begins.
- [x] Update this plan if code evidence contradicts a selected invariant; do not
  carry alternate Archive APIs into Stage 1.

#### Acceptance Gate

- Every current consumer has one selected Archive purpose, ownership module,
  thread, format/version domain, and validation owner.
- DAST v3 golden bytes and compatibility outcomes are reproducible before the
  migration, and the object-graph v2 compatibility cut is explicit.
- The public Archive contract has no unresolved naming, capability, structured-
  field, version, reference, or error decision.

#### Stage 0 Handoff

- Baseline commit: `054e2914a8b029c00905670cb0789f71291b964b`;
  architecture investigation baseline: `897a12a0`.
- Working set: this plan; `Archive.h/.cpp`, `Object.cpp`, `StructOps.h`,
  `AssetSystem.h/.cpp`, `PackageTests.cpp`, `ReflectionTypeTests.cpp`, and
  `ZPropertyValueSnapshotTests.cpp` are the validated Stage 1/2/3 reference
  set. No production source changed in Stage 0.
- Key symbols and decisions: `FArchiveState`, `EArchiveDirection`,
  `EArchivePurpose`, `EArchiveCapability`, `FArchiveLogicalTypeDescriptor`,
  `FArchiveFieldDescriptor`, `FArchiveVersionContext`, move-only object/field
  scopes, and `FArchiveFailure` are frozen. Discovery is Save-like; scope
  balance and the base reflected walk are Archive invariants; DAST v3 parity is
  complete same-process byte equality; object graph v2 deliberately drops v1
  and GC-derived scope.
- Open questions: none block Stage 1. DAST v4 custom-version persistence and
  compact tables remain explicitly deferred and cannot change the Stage 1 ABI.
- Stage 1 initial working set: `Archive.h/.cpp`, `Object.cpp`, `StructOps.h`,
  `ReflectionTypeTests.cpp`, and `ZPropertyValueSnapshotTests.cpp`. Expand only
  for a direct reflected-value or DHT compile dependency found from these files.
- Validation on 2026-08-07: `DevTool doc plan validate --scope all` passed;
  `CoreObjectTests` passed the frozen object-graph v1 case; and
  `AssetPackageTests` passed seven focused DAST cases covering the package
  round trip, soft references, canonical Maps, struct bit preservation,
  versioned signatures, unknown fields, and truncated-package rollback. No
  runtime launch or full build was required because Stage 0 changes contracts
  only.

### Stage 1: Build the semantic CoreDObject Archive layer

- [x] Replace the raw generic `operator<<` contract with explicit stable value
  overloads, constrained adapters, and capability-gated raw bytes.
- [x] Add Archive direction, purpose, discovery, persistence, canonical-order,
  structured-field, version-context, path-stack, remaining-payload, and object-
  reference capabilities with sticky first-failure behavior.
- [x] Add balanced field/object scopes and base-reflected-walk markers that
  detect missing or duplicate `Super::Serialize` and duplicate field identities.
- [x] Refactor reflected scalar, enum, string, Name, Guid, object, soft-object,
  struct, fixed-array, Array, and Map serialization onto the semantic value
  operations without changing current runtime behavior.
- [x] Make `FDStructOps::Serialize` archive-universal, retain reflected fallback
  and `AuthoredFieldsComplete`, and preserve detached transactional load and
  `PostDeserialize` behavior.
- [x] Migrate `FMemoryWriter`, `FMemoryReader`, snapshot reference tables, and
  focused test Archives to the new API.
- [x] Add compile-time rejection and runtime failure tests for unsupported raw
  layouts, unavailable capabilities, unbalanced scopes, duplicate fields,
  malformed serializers, and sticky errors.

#### Acceptance Gate

- CoreDObject exposes one coherent Archive ABI with no unconstrained raw-layout
  `operator<<` fallback.
- Every supported reflected value round-trips through the new memory Archive,
  custom struct dispatch is purpose-independent, and all malformed scope and
  capability cases fail before live mutation.
- CoreDObject and DHT focused tests pass with no AssetCore dependency.

#### Stage 1 Handoff

- Baseline commit: `970f1d55` (the completed Stage 0 contract freeze).
- Working set: `Archive.h/.cpp`, `Object.cpp`, `StructOps.h`,
  `ReflectionTypeTests.cpp`, and `ZPropertyValueSnapshotTests.cpp`. The direct
  public-signature compile dependency in `AssetCoreTests/PackageTests.cpp` was
  updated without restoring any tests removed by the earlier AssetSystem
  refactor; `StructOps.h` required no source change because its callback was
  already Archive-universal.
- Key symbols and decisions: `FArchiveState`, capability queries,
  `FArchiveVersionContext`, recursive `FArchiveLogicalTypeDescriptor`, move-only
  object/field scopes, base-walk markers, and structured `FArchiveFailure` now
  form the CoreDObject ABI. Stable primitive/enum/string/Name/Guid operations
  replace the raw template. Reflected fixed arrays, containers, structs, hard
  references, and soft paths use that layer; authored reflected struct fallback
  requires `AuthoredFieldsComplete`. Memory Archives retain their current
  process-reference behavior until its explicit Stage 2 removal.
- Open questions: none block Stage 2. Object IDs, discovery freeze, v2 graph
  bytes, address-reference removal, duplication cleanup, and container path
  index segments remain Stage 2 work and must not be inferred from v1 bytes.
- Stage 2 initial working set: `Archive.h/.cpp`, `Object.h/.cpp`,
  `ReflectionTypeTests.cpp`, and `ZPropertyValueSnapshotTests.cpp`; expand only
  for direct duplication, PIE, or generated-object test dependencies.
- Validation on 2026-08-07: full `CoreObjectTests` passed 72 tests; all 187 DHT
  pytest cases passed using a workspace-local `--basetemp`; `AssetPackageTests`
  built and its current transactional soft-Archive case passed. The first DHT
  invocation was environment-limited by denied access to the user temp root,
  then passed unchanged after relocating only pytest temporary files.

### Stage 2: Migrate object graph, duplication, and value consumers

- [x] Add a test `DObject` override that calls `Super::Serialize`, emits named
  native scalar/struct/container/reference fields, records Archive purpose and
  call count, and can inject a deterministic failure.
- [x] Replace object-graph `AddReferencedObjects` discovery with a discovery
  Archive over `DObject::Serialize`; retain separate structural Outer traversal
  and freeze the resulting graph before emission.
- [x] Introduce object-graph v2 with semantic primitive encoding, deterministic
  IDs, complete payload-consumption checks, and no v1 reader.
- [x] Migrate duplication to purpose-specific Archives and verify internal
  remapping, shared external references, constructor-created inner reuse,
  failure cleanup, and post-load behavior.
- [x] Migrate property snapshots and editable-property copy to the common value
  Archive layer while retaining their property-level filters, detached rooting,
  logical equality, and transaction semantics.
- [x] Remove process-pointer serialization from generic memory Archives; any
  address-based diagnostic or test helper must be explicit and nonpersistent.
- [x] Verify that serialized graph scope excludes hidden GC-only and soft
  references, includes supported hard references, and cannot grow after
  discovery freezes.

#### Acceptance Gate

- Object graph, duplication, snapshot, and editable-copy behavior all use the
  new Archive/value layer, and complete-object flows call the same virtual
  `DObject::Serialize` entry.
- The custom test object proves balanced `Super::Serialize`, discovery/emission
  parity, reference remapping, native-field round trips, and failure cleanup.
- Object-graph v2 tests, CoreDObject tests, and Engine duplication/PIE consumers
  pass with no dependency on GC reference discovery.

#### Stage 2 Handoff

- Baseline commit: `cfd7747b6ae1a94778dfcc39e736e5276ffc20d0` (the completed
  Stage 1 semantic Archive layer).
- Working set: `Archive.h/.cpp`, `ReflectionTypeTests.cpp`, and this plan.
  `Object.h/.cpp` required no change. Existing AssetCore math/object-graph and
  Engine World, Spline, SkyBox, and Material tests were direct validation
  dependencies only; no tests removed by the earlier AssetSystem refactor were
  restored.
- Key symbols and decisions: `FObjectGraphDiscoveryArchive` calls each object's
  virtual `Serialize`, while `FObjectGraphContext::Discover` separately retains
  Outer and structural-inner traversal and freezes IDs before v2 emission.
  Object-graph references use explicit Null/Internal tags; duplicate references
  use Null/Internal/External tags plus operation-local tables; snapshot and
  editable-copy references remain operation-local tables. Generic memory
  Archives reject object references. Container path scopes add stable `Fixed`,
  `Array`, `MapKey`, and `MapValue` indices and prevent false duplicate-field
  failures across container elements. Object-graph save publication and
  editable copy are failure-atomic, and complete payload consumption is checked.
- Open questions: none block Stage 3. DAST v3 object/field manifests,
  dependency freeze, external authored paths, exact old-writer byte parity, and
  AssetCore error translation remain Stage 3 work.
- Stage 3 initial working set: this plan; `Archive.h/.cpp`,
  `AssetSystem.h/.cpp`, `PackageTests.cpp`, and the current DAST v3 codec/helper
  owners reached directly from `AssetSystem.cpp`. Expand only for a direct
  package-save caller, logical-codec extraction, or generated test-object
  dependency.
- Validation on 2026-08-07: all 75 `CoreObjectTests` passed; focused
  `WorldTests` PIE duplication/apply, Spline duplication, SkyBox object graph,
  and Material duplication/PostLoad cases passed; focused AssetCore math
  object-graph/truncation and soft-Archive cases passed. No full `all` build or
  editor launch was required because Stage 2 has no user-visible editor change.

### Stage 3: Route DAST v3 package saving through Serialize

- [x] Split package Archive and logical-value code out of monolithic
  `AssetSystem.cpp` while preserving AssetCore ownership and public APIs.
- [x] Implement a DAST discovery Archive that calls every package object's
  `Serialize`, gathers fields, logical types, internal/external references,
  dependencies, and version use, then freezes the package manifest.
- [x] Implement a DAST save Archive that maps object and field scopes onto the
  existing v3 records and rejects every field, type, dependency, object, or
  version not present in the frozen manifest.
- [x] Preserve package object scope, Outer ordering, dependency ordering,
  canonical Map ordering, `Transient` handling, property filters, redirector
  header/body validation, and atomic publication.
- [x] Route `SerializeAssetPackageBytes`, single-package save, bundle save,
  relocation staging, and test-only serialization through the same package
  Archive builder.
- [x] Compare all golden fixtures and representative tracked packages against
  the pre-migration serializer and require exact DAST v3 byte equality.
- [x] Add authored-save tests for the custom object serializer, stable native
  field descriptors, missing `Super::Serialize`, duplicate fields, unsupported
  custom versions, late discovery, external hard references, soft references,
  and deterministic repeated saves.

#### Acceptance Gate

- No live-object DAST save path directly performs a class property loop outside
  `DObject::Serialize` and the shared reflected base implementation.
- Every accepted pre-migration package produces identical v3 bytes and
  dependencies after migration; failures publish no file, dirty-state change,
  or registry update.
- Package save, bundle-save, redirector, canonical Map, soft/hard-reference,
  and deterministic-byte tests pass.

#### Stage 3 Handoff

- Baseline commit: `a734ab914f33b1e3905156478769565c1764de9c` (the completed
  Stage 2 runtime Archive-consumer migration).
- Working set: this plan; `Archive.h/.cpp`; `AssetSystem.cpp`; new private
  `AssetPackageArchive.h/.cpp`; and `PackageTests.cpp`. `AssetSystem.h` required
  no public-API change. Asset import/cook plus Engine Material, Texture,
  StaticMesh, and Environment tests were direct validation dependencies only;
  no tests removed by the earlier AssetSystem refactor were restored.
- Key symbols and decisions: `FAuthoredCaptureArchive` runs discovery and save
  passes over every package object's virtual `Serialize`, captures structured
  field/path events, freezes object IDs, logical types, dependencies, and
  versions, and rejects any emission manifest mismatch. The save pass maps the
  captured semantic tree back to the existing v3 object/field/payload grammar;
  reflected metadata retains its historical wire kind/signature while native
  fields add an explicit descriptor version. Hard references encode internal
  IDs or external main-asset paths and dependencies; soft references remain
  path-only. `BuildPackageBytes` is now a thin client of the one private
  authored-package builder, so byte serialization, single save, bundle save,
  relocation, and cook staging share the same failure-atomic path.
- Exact-parity evidence: a temporary in-process legacy-writer oracle compared
  complete old/new byte vectors for the full 79-test `AssetPackageTests` frozen
  corpus, including filters, redirectors, canonical Maps, fixtures, bundle
  saves, compatibility cases, and deterministic repeats. All comparisons
  passed; the oracle and old live-object property loop were then removed.
- Open questions: none block Stage 4. DAST load field lookup, unknown-field
  retention, transactional value commit, legacy upgrade routing, and reverse
  `PostLoad` remain Stage 4 work.
- Validation on 2026-08-07: all 75 `CoreObjectTests` and all 79
  `AssetPackageTests` passed; all 23 `AssetImportCoreTests`, 13
  `AssetImportTests`, and 12 `AssetCookTests` passed. Focused Material default
  cook, Texture scene-import and cook, StaticMesh cooked-package, and
  Environment authored-payload tests passed. No full `all` build or editor
  launch was required because Stage 3 has no user-visible editor change.

### Stage 4: Route DAST v3 package loading through Serialize

- [x] Implement a DAST load Archive that resolves fields by stable identity and
  logical type, bounds each payload, resolves object IDs and external paths, and
  records missing, incompatible, unknown, and unconsumed fields.
- [x] Preserve the two-phase object skeleton/dependency construction sequence,
  constructor-default behavior, existing-inner reuse, whole-package rollback,
  reverse `PostLoad`, dirty-state policy, and load-mutation reporting.
- [x] Feed unmatched field records into the existing legacy-field upgrader and
  compatibility report with exact original payloads and unchanged risk/data-
  loss behavior.
- [x] Invoke universal struct custom serializers and post-deserialize repair
  through the authored Archive while retaining detached transactionality.
- [x] Add package round trips for the custom object serializer, native field
  versions, missing defaults, old native field signatures, nested references,
  failure injection, truncation, trailing bytes, and `PostLoad` rejection.
- [x] Remove the live-object call sites of AssetCore's standalone recursive
  `DeserializeValue` path after parity is proven.

#### Acceptance Gate

- Every loaded live package object receives exactly one DAST load call through
  `DObject::Serialize`, followed by the selected finalization order.
- Existing v3 fixtures retain identical success, failure, compatibility,
  unknown-retention, mutation-report, and dirty-state outcomes.
- A failed field, struct repair, custom serializer, dependency, or `PostLoad`
  leaves no published partial package or surviving constructed graph.

#### Stage 4 Handoff

- Baseline commit: `c3768716cf1cc7fae704d0bfd5ff2334feed4f9b` (the completed
  Stage 3 authored-package save migration).
- Working set: this plan; `Archive.h/.cpp`; `AssetSystem.cpp`;
  `AssetPackageArchive.h/.cpp`; and `PackageTests.cpp`. ImportRecord, asset
  import, and cook sources were validation dependencies only; no tests removed
  by the earlier AssetSystem refactor were restored.
- Key symbols and decisions: `FAuthoredLoadArchive` consumes bounded v3 field
  records by declaring type, stable field name, wire kind, and logical
  signature, resolves internal IDs and external package paths, and exposes
  missing fields to serializers without overwriting constructor defaults.
  Each live object receives one virtual `Serialize` call. Top-level unknown or
  incompatible records retain their exact payloads and flow into the existing
  structure upgrader and compatibility report; nested struct behavior retains
  the v3 contract of skipping unknown fields and rejecting incompatible known
  fields. Array, Map, fixed-array, and nested struct cursors are independently
  bounded, while CoreDObject's detached container/struct commit keeps value
  repair transactional. Authored struct `PostDeserialize` now receives
  `AuthoredAsset` and DAST version 3. The existing skeleton-first construction,
  dependency phase, existing-inner reuse, reverse `PostLoad`, dirty/mutation
  reporting, and package rollback remain owned by `LoadPackageInternal`.
- Legacy-path boundary: the live-package call site of `DeserializeValue` was
  removed. Its remaining uses are byte-only inspection, reference extraction,
  fixup, and value-tooling consumers intentionally retained for Stage 5.
- Open questions: none block Stage 5. Shared logical-codec extraction and final
  removal of byte-tool duplication remain Stage 5 work.
- Validation on 2026-08-07: all 75 `CoreObjectTests`, 81
  `AssetPackageTests`, 23 `AssetImportCoreTests`, 13 `AssetImportTests`, and 12
  `AssetCookTests` passed. Focused tests covered one-call custom object loading,
  native versions and missing defaults, exact legacy payload retention,
  nested struct/container references, truncation/trailing data, custom struct
  repair rejection, serializer failure, and `PostLoad` rollback. No full `all`
  build or editor launch was required because Stage 4 has no user-visible
  editor change.

### Stage 5: Consolidate byte-only DAST tooling and remove legacy paths

- [x] Extract one AssetCore logical value codec used by package Archives,
  inspection, compatibility probing, reference extraction, redirector fixup,
  relocation, deletion analysis, and cook canonicalization.
- [x] Preserve value-only frozen reflection catalogs and worker-safe inspection;
  prove that these paths construct no `DObject`, call no object serializer or
  `PostLoad`, and mutate no package, registry, dirty state, or authored file.
- [x] Remove duplicate scalar/container/struct/reference encoding switches,
  obsolete byte helpers, direct live-object property loops, transitional
  adapters, and public APIs that expose the old raw Archive assumptions.
- [x] Add differential tests showing that package Archive and byte-only tooling
  parse and rewrite every supported logical type identically, including nested
  references and canonical Map keys.
- [x] Verify reference-index, redirector fixup, cook canonicalization,
  compatibility audit, and data-loss consent behavior against the unchanged v3
  corpus and frozen malformed fixtures.

#### Acceptance Gate

- AssetCore has one logical DAST value grammar implementation and no second
  live-object serializer outside the Archive adapters.
- Inspection and rewrite tooling remain construct-free, deterministic,
  bounded, and behaviorally identical on all v3 fixtures.
- Repository search finds no obsolete direct package `ForEachProperty` save/load
  loop or pointer-layout Archive fallback.

#### Stage 5 Handoff

- Baseline commit: `475f8c5c0b58cd091639861c7bc1b991ff80bfae` (the completed
  Stage 4 authored-package load migration).
- Working set: this plan; `AssetPackageValueCodec.h`;
  `AssetPackageArchive.cpp`; `AssetSystem.cpp`; and `PackageTests.cpp`. No tests
  removed by the earlier AssetSystem refactor were restored.
- Key symbols and decisions: the private `AssetPackageValueCodec` now owns the
  bounded memory reader/writer, the reflected and native DAST v3 type-signature
  grammar, fixed-array logical unwrapping, and native logical-kind mapping used
  by both Archive adapters and byte-only tools. AssetSystem's standalone
  recursive `SerializeValue` implementation and its canonical Map traversal
  helpers were deleted; authored emission remains exclusively in
  `FAuthoredCaptureArchive`. Redirector fixup validates a Map key through the
  bounded byte-tool decoder, then copies the exact consumed key slice instead
  of decoding and re-encoding it through a second serializer. The surviving
  `DecodeByteToolValue` name makes its value-only role explicit: it is used only
  for canonical Map route keys and explicit inspection `TryReadStruct`, never
  for live package loading. Reference extraction and rewrite retain their
  route-specific walkers because they emit stable occurrence paths and preserve
  unknown struct field bytes rather than materializing an object graph.
- Construct-free boundary: authored inspection now explicitly records that it
  does not construct the inspected class or invoke its virtual serializer.
  Existing reference-index, unloaded redirector-fixup, relocation, deletion,
  compatibility, malformed-payload, canonical Map, and cook tests continue to
  exercise the frozen reflection catalog without publishing or dirtying a
  package. Repository search finds no `SerializeValue`/`DeserializeValue`
  legacy function and no direct package save/load `ForEachProperty` loop.
- Open questions: none block Stage 6. Lasting ownership documentation and the
  complete qualification matrix remain Stage 6 work.
- Validation on 2026-08-07: all 81 `AssetPackageTests`, 23
  `AssetImportCoreTests`, 13 `AssetImportTests`, and 12 `AssetCookTests` passed.
  The corpus covers exact authored bytes, all supported reflected scalar,
  string/name/GUID, struct, fixed/variable container, hard/soft reference, and
  canonical Map forms; malformed bounds, reference indexing, redirector fixup,
  cook canonicalization, compatibility audit, and data-loss consent retained
  their existing outcomes. No full `all` build or editor launch was required
  because Stage 5 has no user-visible editor change.

### Stage 6: Document and qualify the unified architecture

- [x] Move lasting Archive, object graph, struct, package, versioning, reference,
  failure, and tooling boundaries into their owning Runtime documentation.
- [x] Update the Compact Asset Serialization Roadmap with the completed
  prerequisite, remaining DAST v4 version-table decision, and any measured
  constraints discovered during migration.
- [x] Run focused DHT, CoreDObject, AssetCore package/compatibility/reference,
  Engine duplication/PIE, asset import, material, texture, level, and editor
  document tests under the documented Agent Build Profile.
- [x] Complete a successful full `all` build because the CoreDObject public ABI,
  AssetCore package implementation, generated reflection consumers, and editor
  runtime change together.
- [x] Perform an editor package open/edit/save/unload/reload/restart smoke on
  representative Level, Material, Texture, StaticMesh, ImportRecord, and
  redirector assets; verify deterministic resave and unchanged registry data.
- [x] Record final baseline, working set, key symbols, decisions, open questions,
  exact-byte evidence, focused validation, full-build result, and editor smoke
  in the stage handoff.

#### Acceptance Gate

- Lasting documentation describes one live-object serialization entry and the
  selected Archive/tooling boundaries without competing legacy contracts.
- All focused suites, full build, representative editor smoke, deterministic
  DAST v3 resave, and tracked-content compatibility checks pass from one
  coherent generated-code baseline.
- The Compact Asset Serialization Roadmap can begin its next child plan without
  reintroducing a second object serializer or inventing an alternate Archive
  contract.

#### Stage 6 Handoff

- Baseline commit: `4aa4543d549fd7028080e60bd996d146ed4fc87e` (the completed
  Stage 5 shared DAST value-tooling consolidation).
- Working set: this plan; `ReflectionSystem.md`; `AssetPackages.md`;
  `Versioning.md`; `CompactAssetSerialization.md`; `EngineTests/CMakeLists.txt`;
  and `NewLevelBaselineTests.cpp`.
- Key symbols and decisions: lasting CoreDObject documentation now defines
  `DObject::Serialize(FArchive&)` as the only complete-object entry, the
  direction/purpose/capability split, logical field identity, sticky first
  failure, universal struct serialization, object-graph v2 discovery and
  rollback, duplication, snapshots, and version context. Asset documentation
  defines the DAST v3 discovery/emission and skeleton-first load Archives,
  compatibility and failure policy, and the construct-free byte-tool boundary.
  Versioning explicitly rejects authored GUID custom-version tables in v3 and
  leaves their package-local representation, canonical order, bounds, unknown
  policy, discovery freeze, and exact retention to the v4 wire contract. The
  compact roadmap records the prerequisite as complete and carries forward the
  measured v3 inspection limits without treating them as a v4 encoding choice.
- Qualification repair: the Level reconstruction test previously depended on a
  warm StaticMesh DDC entry. `WorldTests` now links StandardAssetImport and the
  test explicitly scopes its production providers, so the representative Level
  package reconstructs from source on a cold cache. No tests removed by the
  earlier AssetSystem refactor were restored.
- Exact-byte and compatibility evidence: `AssetPackageTests` retained all v3
  golden-byte, deterministic Map, malformed payload, compatibility, reference
  index, redirector fixup, relocation, cook canonicalization, and data-loss
  outcomes. `DevTool asset baseline` reported all 17 tracked packages at DAST
  v3; three fail policies reported 17 compatible, zero incompatible,
  unsupported, failed, or stale packages. SHA-256 comparison across two editor
  launches found zero changes in all 17 tracked `.dasset` files. The Sandbox
  registry refreshed one stale pre-run cache, then produced identical
  `0AA3E58FA0A4D0F4CD0B1CA8CD496B1A4B7002FD513911D538CD2EB8A4C6765C`
  bytes after the first run and restart.
- Focused validation on 2026-08-07 under `windows-msvc-x64` /
  `Win64-Debug-DurinEditor-Tests`: 187 DHT tests; 75 `CoreObjectTests`; 81
  `AssetPackageTests`; 23 `AssetImportCoreTests`; 13 `AssetImportTests`; 62
  `WorldTests`; 18 `SplineTests`; 77 `MaterialTests`; 44 `StaticMeshTests`; 62
  `TextureTests`; 55 `EditorAssetWorkflowTests` (54 passed, one intentional
  skip); 27 `EditorPropertyTests`; and 27 `EditorShellTests`. Representative
  automated workflows cover Level and PIE duplication, Material and StaticMesh
  edits, Texture import/reload, ImportRecord restart state, redirector
  round-trip/fixup, document switching, dirty revisions, save, unload, reload,
  and rollback. Changed-document validation passed.
- Full build and editor smoke: `DevTool build --target all --agent` completed
  successfully from the same generated-code baseline. The resulting
  `DurinEditor.exe` opened the Sandbox project and configured default
  `/Game/Levels/NewLevel` in two hidden 300-tick launches separated by a full
  process restart. Both exited successfully; the second reused unchanged
  registry bytes and neither launch rewrote tracked packages.
- Open questions: none remain for this plan. DAST v4 version tables, compact
  metadata/default-relative encoding, mixed-version migration, and content
  rollout remain explicit child-plan work in the compact roadmap.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Archive ABI | Compile-time rejection of unsupported generic values; balanced object/field scopes; stable purpose, capability, path, version, and sticky-failure behavior |
| Object entry | Base and derived `DObject::Serialize` calls, exactly-once `Super`, native named fields, discovery/emission parity, and failure injection |
| Values | Every scalar, bool, string, Name, Guid, enum, struct, fixed array, Array, Map, nested container, hard reference, and soft reference across save/load |
| Structs | Reflected fallback, universal custom serializer, managed detached load, post-deserialize repair/rejection, hidden GC references, and incomplete-authored-state failure |
| Runtime graph | v2 header/bounds, Outer chains, cycles, shared references, serialized versus GC-only reachability, malformed IDs, truncation, and cleanup |
| Duplication and snapshots | Internal remap, external sharing, constructor-created inner reuse, property filters, rooting, equality, cancellation, and trailing-byte failures |
| DAST save | Exact v3 golden bytes, dependency and field ordering, property filters, late-discovery rejection, deterministic repeated saves, and atomic publication |
| DAST load | Constructor defaults, internal/external references, unknown retention, legacy upgrade, compatibility risk, rollback, reverse `PostLoad`, and dirty-state policy |
| Byte-only tooling | No live objects; bounded inspection, reference extraction, fixup, relocation, canonicalization, deletion analysis, and cook parity with package Archives |
| Version domains | Engine version independence, object-graph v2 cut, DAST v3 stability, unsupported authored custom-version diagnostics, and no implicit migration |
| Integration | DHT, CoreDObject, AssetCore, Engine, import, rendering-asset, level, and editor package workflows |
| Qualification | Successful full `all` build, representative editor save/reload/restart smoke, deterministic resave, and unchanged tracked package compatibility |

Build and test execution follows [Build and Run](../Development/Build/BuildAndRun.md)
and [Native Tests](../Development/Build/NativeTests.md).

## Definition of Done

- Every live complete-object state-transfer path calls
  `DObject::Serialize(FArchive&)`; reflected values and structs use one semantic
  Archive/value implementation.
- DAST v3 save and load are Archive adapters with byte-for-byte writer parity
  and unchanged compatibility, reference, inspection, migration-safety, and
  publication behavior.
- Object-graph v2 scope is discovered through Serialize rather than GC, and all
  runtime graph/duplication failures clean up completely.
- Byte-only package tools share the DAST logical codec while remaining
  construct-free and worker-safe.
- The raw generic `sizeof(T)` operator, duplicate live-object DAST serializer,
  and obsolete direct package property loops are removed.
- Focused tests, full build, editor smoke, lasting documentation, and roadmap
  handoff all pass from one coherent baseline.

## Deferred Follow-ups

- DAST v4 package-local custom-version tables and GUID-keyed custom version
  registration on authored assets.
- DAST v4 compact metadata tables, default-relative encoding, reader/writer,
  mixed-version migration, and repository content rollout.
- Network, replay, delta, text, SaveGame, hot-reload, and async Archive purposes.
- General opaque bulk payload virtualization; asset-specific DDC and cooked bulk
  formats retain their existing owners.
- Compile-time generation of native custom field descriptors if repeated manual
  declarations become measurable maintenance risk.

## Related Documentation

- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Versioning](../Runtime/Assets/Versioning.md)
- [Garbage Collection](../Runtime/Core/GarbageCollection.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Object.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/StructOps.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/ZPropertyValueSnapshotTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
