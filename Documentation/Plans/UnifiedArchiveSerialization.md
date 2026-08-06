# Unified Archive Serialization Plan

Summary: Unify live `DObject` and reflected-value state transfer behind one `Serialize(FArchive&)` entry with purpose-specific Archives while preserving DAST v3 compatibility and byte-only tooling.

Last reviewed: 2026-08-07

Status: Active
Completed:

## Current Status

- Stage 0 is active from baseline commit `897a12a0`. No implementation has
  started.
- `DObject::Serialize(FArchive&)` is already virtual and its base implementation
  walks reflected non-`Transient` properties, but only the transient object-graph
  and duplication paths call it. AssetCore package save and load instead own a
  second recursive property serializer in `AssetSystem.cpp`.
- `FArchive` currently models only Load versus Save, raw `sizeof(T)` byte
  serialization, strings, sticky errors, object references, and canonical Map
  ordering. It has no purpose, field identity, logical type, format-version,
  discovery, or structured-record contract.
- Object-graph scope is currently discovered through `AddReferencedObjects`,
  even though emitted values are selected through `Serialize`. This mixes GC
  reachability with persistence reachability and may gather hidden strong
  references that have no serialized edge.
- DAST v3 inspection, reference indexing, redirector fixup, compatibility
  reporting, and cook canonicalization intentionally operate on field records
  without constructing live objects. They remain byte-level consumers in the
  selected architecture.
- The plan is an active architecture prerequisite of the
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

## Implementation Stages

### Stage 0: Freeze the archive contract and migration baseline

- [ ] Inventory every `FArchive`, `DObject::Serialize`, reflected-value,
  `FDStructOps::Serialize`, object-reference, package save/load, snapshot,
  duplication, inspection, fixup, and reference-index call site and classify it
  by direction, purpose, thread, object construction, field visibility, and
  failure owner.
- [ ] Freeze public names and signatures for Archive state, capability queries,
  field descriptors/scopes, logical type descriptors, version context, object-
  reference operations, and structured diagnostics.
- [ ] Record the exact supported property/reference matrix for ObjectGraph,
  Duplicate, Snapshot, EditableCopy, and AuthoredPackage purposes, including
  raw, hard, soft, weak, hidden, transient, fixed-array, nested struct, Array,
  and Map cases.
- [ ] Add or freeze DAST v3 golden package bytes covering every supported
  logical property shape, nested references, unknown fields, canonical Maps,
  and constructor defaults before replacing AssetCore serialization.
- [ ] Freeze object-graph v1 semantic fixtures and document the deliberate v2
  cut, including scope changes caused by replacing GC discovery with Serialize
  discovery.
- [ ] Decide and record the exact CoreDObject-to-AssetCore error translation and
  field-path diagnostic format before public Archive ABI work begins.
- [ ] Update this plan if code evidence contradicts a selected invariant; do not
  carry alternate Archive APIs into Stage 1.

#### Acceptance Gate

- Every current consumer has one selected Archive purpose, ownership module,
  thread, format/version domain, and validation owner.
- DAST v3 golden bytes and compatibility outcomes are reproducible before the
  migration, and the object-graph v2 compatibility cut is explicit.
- The public Archive contract has no unresolved naming, capability, structured-
  field, version, reference, or error decision.

### Stage 1: Build the semantic CoreDObject Archive layer

- [ ] Replace the raw generic `operator<<` contract with explicit stable value
  overloads, constrained adapters, and capability-gated raw bytes.
- [ ] Add Archive direction, purpose, discovery, persistence, canonical-order,
  structured-field, version-context, path-stack, remaining-payload, and object-
  reference capabilities with sticky first-failure behavior.
- [ ] Add balanced field/object scopes and base-reflected-walk markers that
  detect missing or duplicate `Super::Serialize` and duplicate field identities.
- [ ] Refactor reflected scalar, enum, string, Name, Guid, object, soft-object,
  struct, fixed-array, Array, and Map serialization onto the semantic value
  operations without changing current runtime behavior.
- [ ] Make `FDStructOps::Serialize` archive-universal, retain reflected fallback
  and `AuthoredFieldsComplete`, and preserve detached transactional load and
  `PostDeserialize` behavior.
- [ ] Migrate `FMemoryWriter`, `FMemoryReader`, snapshot reference tables, and
  focused test Archives to the new API.
- [ ] Add compile-time rejection and runtime failure tests for unsupported raw
  layouts, unavailable capabilities, unbalanced scopes, duplicate fields,
  malformed serializers, and sticky errors.

#### Acceptance Gate

- CoreDObject exposes one coherent Archive ABI with no unconstrained raw-layout
  `operator<<` fallback.
- Every supported reflected value round-trips through the new memory Archive,
  custom struct dispatch is purpose-independent, and all malformed scope and
  capability cases fail before live mutation.
- CoreDObject and DHT focused tests pass with no AssetCore dependency.

### Stage 2: Migrate object graph, duplication, and value consumers

- [ ] Add a test `DObject` override that calls `Super::Serialize`, emits named
  native scalar/struct/container/reference fields, records Archive purpose and
  call count, and can inject a deterministic failure.
- [ ] Replace object-graph `AddReferencedObjects` discovery with a discovery
  Archive over `DObject::Serialize`; retain separate structural Outer traversal
  and freeze the resulting graph before emission.
- [ ] Introduce object-graph v2 with semantic primitive encoding, deterministic
  IDs, complete payload-consumption checks, and no v1 reader.
- [ ] Migrate duplication to purpose-specific Archives and verify internal
  remapping, shared external references, constructor-created inner reuse,
  failure cleanup, and post-load behavior.
- [ ] Migrate property snapshots and editable-property copy to the common value
  Archive layer while retaining their property-level filters, detached rooting,
  logical equality, and transaction semantics.
- [ ] Remove process-pointer serialization from generic memory Archives; any
  address-based diagnostic or test helper must be explicit and nonpersistent.
- [ ] Verify that serialized graph scope excludes hidden GC-only and soft
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

### Stage 3: Route DAST v3 package saving through Serialize

- [ ] Split package Archive and logical-value code out of monolithic
  `AssetSystem.cpp` while preserving AssetCore ownership and public APIs.
- [ ] Implement a DAST discovery Archive that calls every package object's
  `Serialize`, gathers fields, logical types, internal/external references,
  dependencies, and version use, then freezes the package manifest.
- [ ] Implement a DAST save Archive that maps object and field scopes onto the
  existing v3 records and rejects every field, type, dependency, object, or
  version not present in the frozen manifest.
- [ ] Preserve package object scope, Outer ordering, dependency ordering,
  canonical Map ordering, `Transient` handling, property filters, redirector
  header/body validation, and atomic publication.
- [ ] Route `SerializeAssetPackageBytes`, single-package save, bundle save,
  relocation staging, and test-only serialization through the same package
  Archive builder.
- [ ] Compare all golden fixtures and representative tracked packages against
  the pre-migration serializer and require exact DAST v3 byte equality.
- [ ] Add authored-save tests for the custom object serializer, stable native
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

### Stage 4: Route DAST v3 package loading through Serialize

- [ ] Implement a DAST load Archive that resolves fields by stable identity and
  logical type, bounds each payload, resolves object IDs and external paths, and
  records missing, incompatible, unknown, and unconsumed fields.
- [ ] Preserve the two-phase object skeleton/dependency construction sequence,
  constructor-default behavior, existing-inner reuse, whole-package rollback,
  reverse `PostLoad`, dirty-state policy, and load-mutation reporting.
- [ ] Feed unmatched field records into the existing legacy-field upgrader and
  compatibility report with exact original payloads and unchanged risk/data-
  loss behavior.
- [ ] Invoke universal struct custom serializers and post-deserialize repair
  through the authored Archive while retaining detached transactionality.
- [ ] Add package round trips for the custom object serializer, native field
  versions, missing defaults, old native field signatures, nested references,
  failure injection, truncation, trailing bytes, and `PostLoad` rejection.
- [ ] Remove the live-object call sites of AssetCore's standalone recursive
  `DeserializeValue` path after parity is proven.

#### Acceptance Gate

- Every loaded live package object receives exactly one DAST load call through
  `DObject::Serialize`, followed by the selected finalization order.
- Existing v3 fixtures retain identical success, failure, compatibility,
  unknown-retention, mutation-report, and dirty-state outcomes.
- A failed field, struct repair, custom serializer, dependency, or `PostLoad`
  leaves no published partial package or surviving constructed graph.

### Stage 5: Consolidate byte-only DAST tooling and remove legacy paths

- [ ] Extract one AssetCore logical value codec used by package Archives,
  inspection, compatibility probing, reference extraction, redirector fixup,
  relocation, deletion analysis, and cook canonicalization.
- [ ] Preserve value-only frozen reflection catalogs and worker-safe inspection;
  prove that these paths construct no `DObject`, call no object serializer or
  `PostLoad`, and mutate no package, registry, dirty state, or authored file.
- [ ] Remove duplicate scalar/container/struct/reference encoding switches,
  obsolete byte helpers, direct live-object property loops, transitional
  adapters, and public APIs that expose the old raw Archive assumptions.
- [ ] Add differential tests showing that package Archive and byte-only tooling
  parse and rewrite every supported logical type identically, including nested
  references and canonical Map keys.
- [ ] Verify reference-index, redirector fixup, cook canonicalization,
  compatibility audit, and data-loss consent behavior against the unchanged v3
  corpus and frozen malformed fixtures.

#### Acceptance Gate

- AssetCore has one logical DAST value grammar implementation and no second
  live-object serializer outside the Archive adapters.
- Inspection and rewrite tooling remain construct-free, deterministic,
  bounded, and behaviorally identical on all v3 fixtures.
- Repository search finds no obsolete direct package `ForEachProperty` save/load
  loop or pointer-layout Archive fallback.

### Stage 6: Document and qualify the unified architecture

- [ ] Move lasting Archive, object graph, struct, package, versioning, reference,
  failure, and tooling boundaries into their owning Runtime documentation.
- [ ] Update the Compact Asset Serialization Roadmap with the completed
  prerequisite, remaining DAST v4 version-table decision, and any measured
  constraints discovered during migration.
- [ ] Run focused DHT, CoreDObject, AssetCore package/compatibility/reference,
  Engine duplication/PIE, asset import, material, texture, level, and editor
  document tests under the documented Agent Build Profile.
- [ ] Complete a successful full `all` build because the CoreDObject public ABI,
  AssetCore package implementation, generated reflection consumers, and editor
  runtime change together.
- [ ] Perform an editor package open/edit/save/unload/reload/restart smoke on
  representative Level, Material, Texture, StaticMesh, ImportRecord, and
  redirector assets; verify deterministic resave and unchanged registry data.
- [ ] Record final baseline, working set, key symbols, decisions, open questions,
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
