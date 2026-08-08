# Reflected Container Operations Plan

Summary: Turn the current STL-specific array and map helpers into capability-aware reflected container operations with typed registration, linear traversal, safe loading, and deterministic serialization boundaries.

Last reviewed: 2026-08-06

Status: Archived
Completed: 2026-08-06

## Current Status

- Stages 0 through 5 are complete. The production inventory contains
  16 generated default-allocator `std::vector` properties and no generated Map;
  handwritten Map registrations are confined to focused native-test fixtures.
- Stage 0 DHT characterization recorded the former accidental acceptance of
  `std::vector<bool>` and custom-argument `std::unordered_map`, the
  four-container nesting limit, and the former alias/custom allocator/key
  boundary that Stage 3 replaced with explicit diagnostics.
- Stage 0 focused probes recorded pre-migration behavior: indexed Map serialization
  performs `n(n-1)` iterator advances, value-only traversal performs
  `n(n-1)/2`, truncated Array loads mutate a resized prefix, duplicate Map keys
  overwrite earlier values, and DAST v2 plus snapshot bytes depend on unordered
  insertion/bucket history.
- The canonical Map key domain and recursive key-token ordering are frozen
  below. The Stage 1 contract now fixes the version-1 descriptors, capability
  masks, checked results, callback lifetimes, detached commit model, typed
  parameters, consumer requirements, and legacy-removal boundary.
- Stage 2 implements the version-1 runtime descriptors, typed container
  registration, checked property operations, linear visitors, detached storage,
  and constrained default-form STL adapters. CoreObjectTests pass 66/66 and
  AssetPackageTests pass 39/39.
- The migration is complete. Generated registration and every runtime consumer
  use checked typed operations; indexed Map traversal and obsolete helper
  fixtures are removed. Container loads are transactional, canonical writers
  are independent of unordered insertion and bucket history, and DAST v2
  remains backward-readable without a version change.
- The lasting contract now lives in `Runtime/Core/ReflectionSystem.md` and
  `Runtime/Assets/AssetPackages.md`. DHT passes 173/173, CoreObjectTests 66/66,
  AssetPackageTests 40/40, EditorPropertyTests 25/25, the complete native-test
  aggregate, and the full `all` build.

## Goal

Make a reflected Array or Map describe a logical container schema plus an
immutable, versioned set of supported runtime operations. Reflection consumers
must be able to traverse, mutate, serialize, deserialize, snapshot, edit, and
trace supported containers without assuming their C++ implementation, silently
requesting unavailable element operations, depending on unordered iteration,
or leaving partially decoded values behind after failure.

The result must preserve the existing reflected Array/Map identities and DAST
v2 grammar while making the current `std::vector` and `std::unordered_map`
adapters compile-safe, diagnosable, and replaceable behind the reflection
boundary.

## Scope

- Audit every reflected `std::vector` and `std::unordered_map` declaration,
  including nested containers, struct elements, object references, map keys,
  map values, handwritten registrations, and editor mutation consumers.
- Define immutable, versioned `FArrayOps` and `FMapOps`-style runtime
  descriptors with explicit capabilities and checked operations.
- Separate logical property schema from storage-backend operations:
  `FArrayProperty` continues to own its inner property and `FMapProperty`
  continues to own key/value properties.
- Add real typed `FArrayPropertyParams` and `FMapPropertyParams` registration
  records after the typed struct-property parameter contract is complete.
- Replace per-property generated helper function bodies with reusable templates
  for supported standard-library container specializations.
- Preserve `std::vector<T>` and `std::unordered_map<K, V>` as the initial and
  only generated adapters in this plan.
- Make generated adapters publish only operations that their concrete container
  and reflected element types can actually perform.
- Replace index-based Map traversal with a single-pass visitor or cursor
  contract used by GC, Archive, snapshots, AssetCore, and editor inspection.
- Make current serializers check exact container and element capabilities and
  fail with stable diagnostics instead of silently skipping unsupported data.
- Make container loading bounded and transactional: failure leaves the
  destination container logically unchanged.
- Reject duplicate decoded Map keys rather than silently applying
  `insert_or_assign` last-write-wins behavior.
- Establish and test deterministic emission for current authored DAST v2 and
  other consumers that promise byte-stable snapshots, independent of
  `std::unordered_map` insertion and bucket order.
- Preserve GC traversal for object references nested in array elements, map
  keys, map values, nested structs, and nested containers.
- Add focused DHT, CoreDObject, AssetCore, editor-model, compatibility,
  corruption, determinism, and complexity coverage.

## Non-Goals

- Introducing `TArray`, `TMap`, or any other engine-owned container storage
  implementation.
- Replacing `std::vector` or `std::unordered_map` in repository call sites.
- Adding reflection support for `std::map`, `std::set`, `std::optional`,
  `std::variant`, multi-maps, sparse containers, or third-party containers.
- Supporting custom `std::vector` allocators or custom
  `std::unordered_map` hash, equality, or allocator template arguments through
  generated reflection. They receive explicit unsupported diagnostics until a
  later adapter-registration design owns them.
- Making `std::vector<bool>` a reflected Array. Its proxy reference semantics
  are rejected explicitly rather than special-cased.
- Extending or redefining `FDStructOps`. This plan consumes the completed struct
  lifecycle and logical-operation capabilities as published.
- Guaranteeing generic deserialization for a value type that exposes no usable
  construction path. Registration remains valid, but a consumer requesting an
  unavailable operation fails deterministically.
- Adding move-only generic element construction if the completed property and
  struct operation contracts do not already support it.
- Designing DAST v3 tables, opcodes, compression, default-relative encoding, or
  compact container payloads. Those remain owned by the
  [Compact Asset Serialization Roadmap](../../../Roadmaps/Archive/2026-08/CompactAssetSerialization.md).
- Serializing container object layout, capacity, bucket state, hash seed,
  allocator state, or native iterator order.
- Changing reflected property names, property kinds, field order, type
  signatures, object-reference identity, or DAST v2 version numbers.
- Treating arbitrary `FArchive` implementations as byte-canonical when they do
  not advertise such a contract.

## Design Decisions and Invariants

### Dependency and Parallel-Work Boundary

- Stage 0 owns an inventory, behavior characterization, test matrix, and
  complexity baseline. It may proceed while Reflected Struct Operations is
  being implemented because it does not change production reflection APIs.
- Stages 1 through 5 require the completed Reflected Struct Operations and
  Typed Struct Property Registration baselines. The exact property value
  lifecycle API and typed-parameter representation are consumed, not guessed.
- This plan owns typed Array and Map parameters. It does not reopen the completed
  `FStructPropertyParams` design or add container fields back to it.
- Implementation touches DHT generation, CoreDObject property registration,
  Archive, GC, AssetCore, and editor property access. It must not share a source
  checkout writer with another reflection implementation task. Separate
  worktrees may be used for independent preparation, followed by one integrated
  regeneration and validation pass.
- The Durin Math API Facade may proceed in parallel while it preserves math
  aliases, reflection descriptors, and serialized component schemas as already
  required by its plan.

### Logical Schema and Backend Boundary

- `FArrayProperty` represents an ordered logical sequence and owns exactly one
  inner `FProperty` schema.
- `FMapProperty` represents unique logical keys mapped to values and owns
  exactly one key and one value `FProperty` schema. It does not imply an
  iteration order.
- Container implementation names never enter serialized type signatures.
  Existing signatures remain `Array<...>` and `Map<...,...>`.
- `FArrayOps` and `FMapOps` are immutable static descriptors referenced by
  runtime properties. Their first version includes an explicit descriptor
  version and capability mask; adding operations does not reinterpret an older
  descriptor layout silently.
- The ops descriptors erase the storage type from consumers but do not erase
  element schema. Element construction, destruction, copying, equality,
  serialization, and reference semantics remain owned by the inner/key/value
  property and, for structs, its referenced `DStruct` and `FDStructOps`.
- Handwritten intrinsic or test registrations use the same typed parameters and
  operation descriptors as generated registrations. There is no permissive
  legacy path with weaker validation.

### Capability Contract

- Every Array descriptor declares whether it supports const traversal, mutable
  traversal, count, clear, reserve, default growth, shrink, detached staging,
  and transactional commit.
- Every Map descriptor declares whether it supports const value traversal,
  mutable mapped-value traversal, count, clear, reserve, lookup, insertion,
  removal, key rename, detached staging, and transactional commit.
- Read-only reflection and GC require only the traversal capabilities relevant
  to their schemas. Editor mutation and loading must check their stronger
  capabilities separately.
- Missing mandatory function slots for a declared capability fail property
  registration. Missing optional capabilities leave the property registered and
  cause only consumers requesting those operations to fail.
- Mutating operations return a checked result that distinguishes unsupported
  capability, invalid input, duplicate key, allocation/construction failure,
  and backend rejection. Public reflection APIs do not silently turn a missing
  operation into count zero or a no-op.
- Counts convert to and from `size_t` through checked bounds. A serialized
  `uint64` count is rejected before allocation when it exceeds the container,
  property, or input budget.
- A backend adapter may expose faster optional operations, but generic
  consumers depend only on the frozen semantic contract.

### Element Lifecycle and Construction

- DHT does not unconditionally instantiate `resize`, `new T()`, `new T(copy)`,
  `insert_or_assign`, or key `operator==` merely because a container property is
  reflected.
- Reusable STL adapter templates use constrained or `if constexpr` paths so an
  unsupported C++ operation produces an absent capability, not a generated
  translation-unit error.
- Reflected struct elements obtain lifecycle capabilities from their `DStruct`.
  Container registration never publishes a second struct constructor,
  destructor, copy operation, equality function, or serializer.
- Field-walk deserialization into a new element requires a valid construction
  path before any field is written. If the element cannot be constructed and no
  applicable semantic decoder can create it, loading fails before destination
  mutation.
- Each successfully created temporary key, value, or element is destroyed
  exactly once through its owning value-operation contract. Uninitialized
  storage is never destroyed, and live storage is never constructed over.
- Copy construction and copy assignment remain distinct. Map insertion and
  container commit may request only the capability their implementation
  actually uses.
- Array shrink destroys removed live elements through the container backend;
  reflection does not separately destroy the same elements.

### Traversal, Mutation, and Complexity

- Array traversal may retain indexed random access because the supported
  backend is contiguous and provides stable O(1) element lookup for the
  duration of a non-mutating traversal.
- Map traversal is callback- or cursor-based and completes in O(n) backend
  iterator advances. Consumers do not call `GetKeyPtr(Index)` or
  `GetMappedValuePtr(Index)` in a loop.
- A traversal token cannot outlive the operation that created it. No iterator
  or entry address survives structural mutation, reserve, rehash, clear, or
  transaction commit.
- Map keys are exposed as const logical values. GC or another consumer may not
  mutate a key in place in a way that invalidates hashing or equality.
- Key rename is an optional semantic mutation implemented by the backend. It
  rejects a missing old key, an equivalent new key, and a collision without
  losing the original entry.
- Editor operations use the same contains, insert, rename, and remove results as
  serialization and tests; they do not reproduce STL-specific behavior.

### Ordering and Deterministic Serialization

- `std::unordered_map` iteration and bucket order are explicitly non-semantic.
  Reflection traversal order is not persisted or exposed as a stable editor
  identity.
- Consumers that promise byte-stable output must obtain a canonical logical key
  token for every entry, sort by that token, reject duplicate tokens that are
  inconsistent with Map key equality, and only then emit key/value records.
- Stage 0 freezes one recursive canonical key-token contract for the currently
  supported reflected Map key domain, including integer, enum, string, name,
  GUID, floating-point, and reflected-struct cases actually present in the
  inventory. Unsupported key kinds fail before output rather than falling back
  to bucket order.
- Canonical tokens are comparison material, not persisted container layout.
  Existing DAST v2 readers continue accepting historical entry order, while new
  DAST v2 saves of equivalent supported maps produce the same logical entry
  order.
- Object-reference Map keys remain unsupported by generated reflection. Map
  values and nested containers may still contain references and are serialized
  after canonical key ordering is established.
- Generic runtime archives retain their documented ordering contract. A
  byte-canonical snapshot path opts into canonical Map ordering explicitly;
  an arbitrary streaming Archive is not forced to buffer entries unless its
  contract requires deterministic bytes.
- DAST v3 may choose a different compact representation, but it consumes the
  same logical schema and must not depend on STL iteration state.

### Transactional Loading and Failure Policy

- Array and Map loading decode into managed temporary container storage. The
  destination is committed only after the count, every nested value, duplicate
  checks, post-deserialize hooks, and input-consumption checks succeed.
- A failed load leaves the destination logically equal to its value before the
  operation. It does not leave a resized prefix, a cleared map, or partially
  inserted entries.
- Map loading rejects duplicate logical keys with a stable diagnostic. It does
  not silently select first-write-wins or last-write-wins.
- Count, allocation, construction, nested decoding, insertion, duplicate, and
  trailing-byte failures all unwind temporary values and containers exactly
  once.
- Archive sticky errors established by Reflected Struct Operations propagate
  through container recursion. A container helper cannot mask a failed nested
  operation by returning normally.
- Authored AssetCore errors retain property path, container role
  (element/key/value), and entry index or canonical key context where available.
- Registration fails before property publication when schema links are absent,
  descriptor versions are unsupported, declared capability slots are invalid,
  or owner layout facts conflict with typed parameters.

### Generated Registration and Compatibility

- `FArrayPropertyParams` and `FMapPropertyParams` become distinct parameter
  types rather than aliases of the positional `FPropertyParamsBase` aggregate.
- Their public construction APIs fix the property kind internally and accept
  only common field schema, nested property descriptors, the appropriate ops
  resolver, and optional metadata/accessors.
- Generated initializers contain no placeholders for unrelated class, enum,
  struct, object-wrapper, or generic lifecycle fields.
- A generated property references one reusable specialization such as the
  selected standard-vector or standard-unordered-map adapter. It does not emit
  property-local Num/Get/Resize/Create/Destroy/Insert function bodies.
- Unsupported template forms produce source-qualified DHT diagnostics naming
  the property and unsupported requirement. They do not degrade to an unknown
  property or fail later in generated C++.
- Runtime metadata and generated code are rebuilt together. Mixed old and new
  parameter or ops layouts in one binary are unsupported.
- Existing DAST v2 Array/Map tags, nested type signatures, count fields, and
  entry payload grammar remain readable. Canonical save ordering and stricter
  malformed-input rejection do not require a format-version increment.
- Replacing the STL adapters with engine-owned containers later requires a full
  C++ rebuild but does not require asset migration when reflected schema and
  logical serialization contracts remain unchanged.

## Current Foundations and Gaps

### Foundations

- `FArrayProperty` already owns an inner property and routes count, element
  access, and resize through `FArrayPropertyHelper`.
- `FMapProperty` already owns key/value properties and routes traversal and
  mutations through `FMapPropertyHelper`.
- DHT already builds recursive descriptors for nested arrays and map values and
  generates concrete standard-library helper functions.
- Archive, GC reference schemas, snapshots, AssetCore, and reflected editor
  views already dispatch recursively by `EPropertyGenFlags::Array` and `Map`.
- AssetCore already length-checks container counts and recursively serializes
  logical elements rather than native container bytes.
- The Compact Asset Serialization Roadmap already requires canonical maps,
  explicit bounds, deterministic ordering, and strict duplicate handling for
  the future authored format.
- Reflected Struct Operations and Typed Struct Property Registration define the
  value-semantics authority and typed nested-struct descriptor that this plan
  needs.

### Gaps

- The current helper records have no descriptor version, capability mask,
  result channel, validation contract, or transactional storage operations.
- Array deserialization calls `resize`, which unconditionally requests default
  element construction from the C++ container interface.
- Map deserialization allocates `new Key()` and `new Value()` and then copies
  them through `insert_or_assign`, bypassing reflected value capabilities.
- Map indexed access advances from `begin()` for every requested index, making
  full traversal O(n^2).
- Current Archive code silently skips Array or Map work when helpers or nested
  properties are absent; AssetCore reports only coarse missing-helper errors.
- DAST v2 map output follows `std::unordered_map` iteration order and therefore
  lacks a cross-insertion-order byte-stability guarantee.
- Loading clears or resizes the destination before nested decoding completes,
  and duplicate Map keys overwrite earlier values.
- DHT recognizes the container spelling before proving that generated helper
  operations are valid. `std::vector<bool>` and unsupported template argument
  forms do not receive a precise early diagnostic.
- Every reflected container property emits its own helper declarations and
  definitions, increasing generated-source size and coupling code generation to
  STL member expressions.
- Tests cover recursive Array/Map registration, GC, snapshots, editing, and
  package round trips, but do not establish the complete capability, rollback,
  duplicate, canonical-order, unsupported-type, or linear-traversal contract.

## Implementation Stages

### Stage 0: Audit and Characterize Reflected Containers

- [x] Inventory every generated and handwritten reflected
  `std::vector`/`std::unordered_map` property by module, nested shape,
  element/key/value kind, reference content, serialization use, and editor use.
- [x] Record every accepted and rejected standard-container spelling, including
  aliases, nested containers, explicit allocator/hash/equality arguments,
  `std::vector<bool>`, unsupported key kinds, and maximum nesting depth.
- [x] Trace the current helper call graph through registration, Archive,
  snapshots, GC, AssetCore, compatibility inspection, and editor mutation.
- [x] Measure current generated helper source volume and record representative
  compile-output baselines for direct, nested, struct, and reference-bearing
  containers.
- [x] Measure Map traversal iterator advances for serialization and GC at
  multiple sizes and record the O(n^2) baseline without adding timing-only CI
  gates.
- [x] Produce DAST v2 and snapshot probes for equivalent unordered maps built
  with different insertion orders and record which outputs currently diverge.
- [x] Characterize current behavior for oversized counts, truncated elements,
  duplicate keys, failed struct post-load hooks, missing helpers, and partial
  destination mutation.
- [x] Freeze the supported reflected Map key domain and specify one recursive
  canonical key-token ordering, including float NaN/signed-zero behavior and
  struct-field recursion where applicable.
- [x] Record the exact upstream `FProperty`/`FDStructOps` capabilities needed by
  each proposed Array and Map operation without changing the upstream plans.
- [x] End the stage with a handoff listing the baseline commit, working set,
  inventory artifact, key symbols, measured gaps, selected canonical ordering,
  and validation evidence.

#### Acceptance Gate

- Every existing reflected container shape and every runtime consumer has one
  recorded owner and required operation set.
- Unsupported standard-container forms have an explicit intended diagnostic;
  none are accidentally treated as supported future work.
- Current nondeterminism, rollback, duplicate-key, generated-volume, and
  traversal-complexity behavior is reproducible from focused fixtures or probes.
- The canonical key-token contract is single-valued for every supported key
  kind and does not depend on address, allocation, bucket, locale, process seed,
  or insertion order.
- No production reflection ABI, generated output, or serialized asset has been
  changed during this preparation stage.

#### Stage 0 Inventory and Evidence

The generated production inventory is source-driven from every `DPROPERTY`
followed by a standard container declaration. All entries use the default
`std::vector<T>` specialization and depth one; no production `DPROPERTY` is a
Map or a nested container.

| Module and owner | Properties and logical shape | Reference, persistence, and editor use |
| --- | --- | --- |
| AssetImportCore `FImportRecordPayload` | `Bytes: Array<uint8>` | Authored import-record payload; DAST v2 and Archive/snapshot traversal; no editor mutation flag |
| AssetImportCore `DImportRecord` | `Sources`, `Outputs`, `DetachedTombstones`, `AcceptedDiagnostics`: four Arrays of reflected structs | Authored import metadata; DAST v2, Archive/snapshot, compatibility; import UI consumes the owning model rather than generic `Edit` metadata |
| DurinEd `DEditorEngine` | `RetiredPlayWorlds: Array<TObjectPtr<DWorld>>`, `RetiredPlayLevels: Array<TObjectPtr<DLevel>>` | Transient reference-bearing arrays; GC and runtime Archive only |
| Engine `AActor` | `OwnedComponents`, `InstanceComponents`: Arrays of `TObjectPtr<DActorComponent>` | Authored/reference-bearing; DAST v2, Archive/snapshot, duplication, GC, and structural editor workflows |
| Engine `DLevel` | `Actors: Array<TObjectPtr<AActor>>` | Authored/reference-bearing; DAST v2, Archive/snapshot, duplication, GC, and hierarchy editing |
| Engine `DSceneComponent` | `AttachChildren: Array<TObjectPtr<DSceneComponent>>` | Transient reference-bearing; GC and runtime relationship traversal |
| Engine `DStaticMeshComponent` | `OverrideMaterials: Array<TObjectPtr<DMaterialInterface>>` | Authored/reference-bearing; DAST v2, Archive/snapshot, GC, and material-slot customization |
| Engine `DMaterial` | `ParameterDefinitions: Array<FMaterialParameterDefinition>` | Authored struct elements; DAST v2, Archive/snapshot, compatibility, and material editor model |
| Engine `DMaterialInstance` | `ParameterOverrides: Array<FMaterialParameterOverride>` | Authored struct elements with `Edit`; generic/editor-model mutation, Undo/Redo, DAST v2, and snapshots |
| Engine `FSplineCurve` | `Points: Array<FSplinePoint>` | Authored struct elements with `Edit`; generic editing, DAST v2, Archive/snapshot |
| Engine `DStaticMesh` | `MaterialSlots: Array<FStaticMeshMaterialSlotDefinition>` | Authored struct elements; DAST v2, Archive/snapshot, compatibility, and slot customization |

Handwritten registrations are test-owned. `ReflectionTypeTests.cpp` covers raw
and wrapped object Arrays, scalar/string/enum Arrays, nested Arrays, struct
Arrays, `Map<int32,string>`, `Map<int32,Array<TObjectPtr<DObject>>>`,
`Map<string,int32>`, and `Map<string,Array<DObject*>>`.
`ZPropertyValueSnapshotTests.cpp` covers GUID, wrapped-reference, `FVector3`, and
non-default-constructible Arrays plus `Map<int32,string>`.
`PackageTests.cpp` covers int32/GUID/`FVector3` Arrays and
`Map<string,int32>`/`Map<string,FVector3>`. Editor test support covers
`Array<int32>` and `Map<string,int32>`. The DHT integration fixture additionally
owns direct, nested, struct, reference-bearing, Map-to-Array, unavailable
construction/copy, and struct-key source-generation shapes.

Current spelling behavior and the intended Stage 3 boundary are:

| Form | Stage 0 behavior | Intended diagnostic or support |
| --- | --- | --- |
| `std::vector<T>` | Accepted when `T` recursively resolves | Supported default specialization |
| `std::unordered_map<K,V>` | Accepted when key/value recursively resolve | Supported default specialization |
| Whitespace/qualified spellings | Normalized and accepted | Supported |
| Aliases of supported containers | Not resolved as containers in the source spelling path | Resolve and preserve aliases, or issue `DHT_CONTAINER_ALIAS_UNRESOLVED` with property/source context |
| `std::vector<T, Allocator>` | Rejected by the generic non-hermetic-type error | `DHT_CONTAINER_UNSUPPORTED_TEMPLATE_ARGUMENTS` |
| `std::unordered_map<K,V,Hash,Equal,Allocator>` | Accidentally accepted; extra arguments are dropped from emitted spelling | `DHT_CONTAINER_UNSUPPORTED_TEMPLATE_ARGUMENTS` |
| `std::vector<bool>` | Accidentally accepted; generated address-taking is not compilable | `DHT_CONTAINER_VECTOR_BOOL` |
| Object-reference or container Map key | Rejected by generic type resolution | `DHT_CONTAINER_UNSUPPORTED_KEY` |
| Unsupported element/value kind | Rejected by generic type resolution | `DHT_CONTAINER_UNSUPPORTED_VALUE` naming element or value role |
| Four nested container levels | Accepted | Supported maximum |
| Five or more nested container levels | Rejected by generic type resolution | `DHT_CONTAINER_DEPTH_EXCEEDED` |
| Other standard containers (`map`, `set`, `optional`, and others) | Rejected | `DHT_CONTAINER_UNSUPPORTED_TEMPLATE` and remain non-goals |

The current call graph and minimum requested operations are:

| Consumer | Current requests | Frozen minimum capability direction |
| --- | --- | --- |
| Generated registration / `ConstructGeneratedProperty` | Positional schema, helper pointer, generic lifecycle thunks | Typed schema plus valid descriptor version/capability slots; no mutation capability required merely to register |
| Logical equality | Array count/indexed const access; Map count plus indexed key/value matching | Const traversal and recursive property equality; Map lookup may accelerate but is optional |
| Runtime Archive and snapshots | Array count/mutable index/resize; Map count/indexed key/value, clear, default temporaries, insert | Save needs const traversal; load needs bounded detached staging, value construction/decode, duplicate detection, and atomic commit |
| GC schema | Array count/mutable index; Map count/indexed const keys/values | Const Array traversal and single-pass const Map traversal only for reference-bearing roles |
| AssetCore DAST v2 | Same indexed save and eager-resize/clear load operations as Archive | Const traversal plus canonical key tokens for save; transactional bounded staging/commit for load |
| Compatibility inspection | Nested inner/key/value schema only | Typed logical schema; no backend mutation operations |
| Reflected property view/editing | Array count/index/resize; Map indexed inspection, contains, insert, rename, remove | Separate const traversal, mapped-value mutation, resize, contains, insert, collision-safe rename, and remove capabilities |

The final upstream value contract supplies
`FProperty::CanDefaultConstructValue`, `CanDestroyValue`,
`CanCopyConstructValue`, `CanCopyAssignValue`, the corresponding checked value
operations, `FReflectedValueStorage`, recursive `ArePropertyValuesIdentical`,
and `DStruct`/`FDStructOps` construction, destruction, copy, serializer,
post-deserialize, authored-field, equality, and reference-collector queries.
Array growth requests default construction and destruction; detached copying or
commit requests copy construction or copy assignment only when the chosen
adapter actually uses it. Map decode requests key/value construction and
destruction, insertion requests only the adapter's actual copy operations, and
canonical struct keys require complete authored fields with recursive logical
equality. Container descriptors do not duplicate any struct callback.

#### Canonical Map Key Token v1

Canonical ordering compares an in-memory token as a sequence of typed atoms,
not as native object bytes. Each atom begins with the stable logical property
kind; fixed-width payloads are written big-endian, variable payloads are
length-delimited, and struct fields include their stable declaration ordinal and
array index. This makes recursive concatenation unambiguous and independent of
address, locale, allocation, hash seed, bucket state, or insertion order.

- `bool` uses `0` then `1`. Unsigned integers use big-endian magnitude. Signed
  integers flip the sign bit before big-endian encoding. Enums use their frozen
  underlying signedness and width.
- `float` and `double` normalize both signed zeros to positive zero. Every other
  bit pattern is transformed to a sortable total-order integer: negative raw
  bits are complemented and non-negative raw bits have the sign bit flipped.
  NaN sign, signaling/quiet state, and payload therefore have a deterministic
  order. Two entries producing the same NaN token while Map equality says the
  keys differ are rejected as an inconsistent duplicate-token case.
- `std::string` uses its exact byte sequence. `FName` uses the comparison-name
  entry's stable bytes plus the instance number, never the process-local entry
  id or display casing. `FGuid` uses `A`, `B`, `C`, then `D` as four big-endian
  `uint32` atoms.
- A reflected-struct key is supported only when authored fields are complete,
  equality is the recursive reflected-field equality, and every non-transient
  field is recursively tokenizable. Fields are visited in reflection order;
  fixed C++ array indices participate in order. Custom `Identical`, custom
  semantic Archive-only state, hidden identity, object references, container
  fields, incomplete authored state, or unsupported field kinds make the key
  unsupported before output.
- Object references and container values are not Map keys. Duplicate canonical
  tokens are checked against Map key equality: equal keys are an ordinary
  duplicate, while unequal keys sharing a token are a canonicalization error.

#### Stage 0 Measurements and Failure Matrix

The self-contained DHT fixture emits 121,783 UTF-8 bytes / 1,671 lines. Its 9
Array and 5 Map helper bodies account for 49,302 bytes / 829 lines. A direct
scalar Array helper is 1,857 bytes / 41 lines; nested outer/inner helpers are
1,922 and 1,911 bytes; a wrapped-reference Array is 1,998 bytes; a direct Map is
6,215 bytes / 92 lines; and a Map-to-reference-Array adds a 6,691-byte Map body
plus a 2,007-byte Array body. The measurement regenerates the hermetic fixture
and matches helper bodies from `*Num` through the matching helper table; it is
evidence, not a source-byte CI contract.

The counted helper probe records these exact iterator advances:

| Entries | Archive Map save (key + value) | Value-only traversal used by value-only GC |
| ---: | ---: | ---: |
| 0 | 0 | 0 |
| 1 | 0 | 0 |
| 4 | 12 | 6 |
| 16 | 240 | 120 |

The formulas are `n(n-1)` and `n(n-1)/2`, respectively, because every indexed
access starts at `begin()`. Key-and-value GC has the same `n(n-1)` count as
Archive save. Logical Map equality additionally nests indexed scans and can be
worse than quadratic.

| Input/failure | Current behavior |
| --- | --- |
| Count above 10,000,000 | Archive/AssetCore reject before resize/clear; the destination remains unchanged |
| Truncated Array element | Destination is resized first; successfully decoded prefix elements remain and the old suffix may survive inside the new size |
| Truncated Map key/value | Destination is cleared first; entries decoded before failure remain |
| Duplicate decoded Map key | `insert_or_assign` silently applies last-write-wins |
| Nested struct post-load rejection | The current struct element is transactional, but earlier container elements and eager resize/clear remain committed |
| Missing helper/schema link | Archive establishes a sticky helper error; AssetCore returns `UnsupportedProperty`; GC asserts for a reference-bearing malformed property |
| Trailing property payload | AssetCore rejects after property decode, but container mutation has already occurred before package-level rollback/discard |

`FReflectedContainerBaselineTests` fixes the count, truncation, duplicate, and
iterator-advance behavior. `FPackageAssetTests.DastV2MapBytesFollowUnorderedInsertionHistoryBaseline`
and the snapshot byte assertion in
`LogicalEqualityUsesFieldsAssociationsAndExactFloatingBits` prove that logically
equal Maps currently produce different DAST v2 and snapshot bytes after
different insertion/rehash histories.

#### Stage 0 Handoff

- Baseline commit: `281c9083`. Stage 0 working set: this plan, the hermetic DHT
  reflection-generation fixture, CoreObject container/snapshot probes, and the
  AssetCore package ordering probe. Production sources were inspected but not
  modified.
- Key symbols: `_make_property_from_spelling`, `_array_helper_definition`,
  `_map_helper_definition`, `FArrayPropertyHelper`, `FMapPropertyHelper`,
  `FArrayProperty`, `FMapProperty`, `SerializeReflectedPropertyValue`,
  `FGCReferenceSchemaRegistry`, AssetCore `SerializeValue`/`DeserializeValue`,
  `FReflectedPropertyView`, and `FReflectedPropertyEditTarget`.
- Decisions: keep default STL adapters only; reject `vector<bool>`, custom
  template arguments, object/container keys, unsupported depth and key kinds at
  DHT; use Canonical Map Key Token v1 above; require detached transactional
  decode; and keep schema/value semantics owned by nested `FProperty`/
  `FDStructOps`.
- Validation: DHT fixture `43 passed`; focused CoreObject baseline and snapshot
  probes passed; focused AssetPackage DAST v2 ordering probe passed. The stage
  changes no editor-visible production behavior and does not require a full
  runtime build.

### Stage 1: Freeze the Post-Prerequisite Container Contract

Dependencies: completed Reflected Struct Operations and Typed Struct Property
Registration plans, plus the Stage 0 handoff.

- [x] Validate the final property value-operation, managed-storage, Archive
  error, and typed-parameter APIs against the Stage 0 operation matrix.
- [x] Freeze the first `FArrayOps` and `FMapOps` descriptor layouts, versions,
  capability bits, checked result types, and descriptor validation rules.
- [x] Freeze callback lifetime, mutation invalidation, const-key, traversal
  reentrancy, and thread-ownership rules.
- [x] Freeze temporary-container creation, destruction, reserve, decode, and
  commit semantics required for transactional loading.
- [x] Define distinct typed `FArrayPropertyParams` and `FMapPropertyParams`
  construction APIs and their safe common-base dispatch representation.
- [x] Map each Archive, snapshot, GC, AssetCore, compatibility, and editor
  consumer to the minimum exact capabilities it may request.
- [x] Decide the legacy helper and positional parameter removal boundary; no
  permanent dual runtime representation is allowed.
- [x] Update the plan before implementation if the completed upstream contracts
  make any Stage 0 assumption invalid.
- [x] End the stage with a handoff listing the baseline commit, working set,
  frozen public/private symbols, compatibility decisions, and focused contract
  validation.

#### Acceptance Gate

- The complete operations and typed-parameter API is implementable without
  extending `FDStructOps` or duplicating struct value semantics.
- Registration, read-only traversal, editor mutation, serialization, and loading
  have distinct capability requirements and stable failure results.
- A property may register with optional mutation or construction capabilities
  absent, while every consumer is required to check before use.
- The contract supports linear Map traversal and transaction commit without
  exposing STL iterators or container layout.
- All legacy removal and compatibility decisions are explicit; implementation
  has no unresolved API branch.

#### Stage 1 Runtime Contract

The public runtime names are `ContainerOpsVersion`, `EContainerOpResult`,
`EArrayOpsFlags`, `EMapOpsFlags`, `FArrayOps`, `FMapOps`,
`FArrayConstVisitor`, `FArrayMutableVisitor`, `FMapConstVisitor`, and
`FMapMutableValueVisitor`. Version 1 has numeric value `1`. Descriptors are
immutable function-local static objects returned through generated resolver
functions and remain valid for the process lifetime.

`EContainerOpResult` has these stable values: `Success`, `Unsupported`,
`InvalidInput`, `OutOfRange`, `NotFound`, `DuplicateKey`, `EquivalentKey`,
`AllocationFailure`, `ConstructionFailure`, and `BackendRejected`. A callback
never supplies a borrowed diagnostic string. `FArrayProperty` and
`FMapProperty` translate results into stable property/role-aware diagnostics;
callers may inspect the enum without parsing text. `Unsupported` means the
descriptor intentionally omitted a capability, while `ConstructionFailure`
means a published operation could not create the requested live value.

`EArrayOpsFlags` contains `Count`, `ConstTraversal`, `MutableTraversal`,
`RandomAccess`, `Clear`, `Reserve`, `DefaultGrow`, `Shrink`,
`DetachedStorage`, and `TransactionalCommit`. `FArrayOps` stores, in order:
version and flags; container size/alignment; mandatory container initialize and
destroy callbacks; `Num`; synchronous const and mutable visitors; optional
const/mutable indexed access; checked clear, reserve, and resize callbacks;
checked detached-container creation and destruction; and checked commit.
`Resize` may publish `Shrink` without `DefaultGrow`; wrappers preflight the
direction against the separate flags and nested property value capabilities.

`EMapOpsFlags` contains `Count`, `ConstTraversal`,
`MutableMappedTraversal`, `Clear`, `Reserve`, `Lookup`, `MutableLookup`,
`Insert`, `Remove`, `RenameKey`, `DetachedStorage`, and
`TransactionalCommit`. `FMapOps` stores, in order: version and flags; container
size/alignment; mandatory initialize and destroy callbacks; `Num`; synchronous
const and mutable-mapped visitors; const/mutable lookup with an out pointer;
checked clear and reserve; non-overwriting copy insertion; remove; collision-
safe rename; detached-container creation/destruction; and checked commit. Map
keys are `const void*` in every callback. `Insert` returns `DuplicateKey` and
never assigns an existing mapped value. Rename distinguishes missing old key,
equivalent old/new key, and collision as `NotFound`, `EquivalentKey`, and
`DuplicateKey`.

For both descriptors, a capability bit and its function slot must either both
be present or both be absent. Container size must be nonzero, alignment must be
a power of two, version must be supported, and mandatory lifecycle callbacks
must be non-null. `DetachedStorage` requires paired create/destroy callbacks;
`TransactionalCommit` additionally requires detached storage and a commit that
leaves the destination unchanged on failure. Default STL adapters publish
transactional commit only when swap is `noexcept`. Registration rejects an
invalid combination before attaching the property to its owner.

Visitors are synchronous. Element/key/value pointers and any traversal token
are valid only for the callback invocation. A callback may not retain them,
reenter traversal of the same logical container, reserve, resize, clear,
insert, remove, rename, commit, or otherwise structurally mutate it. Array
mutable traversal may mutate an element without changing container structure;
Map mutable traversal may mutate only the mapped value, never the key. Nested
operations on a different container are permitted. Descriptors provide no
locking: concurrent const traversal requires the ordinary backend guarantee,
and any mutation requires caller-owned exclusive access.

Detached loading follows one ownership sequence: create one empty detached
container; reserve after checked count conversion; construct/decode each nested
value through its owning `FProperty`/`FDStructOps`; add it through the detached
container's checked operation; run every post-deserialize and duplicate check;
commit by no-fail swap; then destroy the detached handle, which now owns the old
destination. Before commit, every error destroys the detached container and
leaves the destination untouched. No live temporary is constructed over or
destroyed twice.

#### Stage 1 Typed Registration Contract

`EPropertyParamLayout` gains `Array` and `Map` without changing existing numeric
values. `FArrayPropertyParams final` and `FMapPropertyParams final` derive from
`FPropertyParamsBase` only to participate in the common owner pointer array.
They zero irrelevant legacy base slots, fix `Kind` and `Layout`, and own typed
fields rather than reusing base `Inner`, `Key`, `Value`, helper, referenced-
type, or lifecycle positions.

- `FArrayPropertyParams(Name, Flags, ArrayDim, Offset, InnerParams,
  OpsResolver, MetaData = nullptr, NumMetaData = 0)` is the direct-member
  constructor. `WithAccessors` replaces `Offset` with a paired mutable/const
  accessor. The record owns `InnerParams` and a resolver returning
  `const FArrayOps*`.
- `FMapPropertyParams(Name, Flags, ArrayDim, Offset, KeyParams, ValueParams,
  OpsResolver, MetaData = nullptr, NumMetaData = 0)` and its `WithAccessors`
  form own key/value parameters and a resolver returning `const FMapOps*`.
- Registration reads common name/flags/dimension/location/metadata, validates
  `Layout` against `Kind`, casts only in the matching switch arm, resolves and
  validates the descriptor, recursively constructs the typed nested schema,
  and derives container size, alignment, initialization, and destruction from
  the descriptor. Nested descriptors must be non-null and owned by exactly one
  published container property. Accessors must be paired and require offset
  zero. Metadata pointer/count must agree.

`FArrayProperty` and `FMapProperty` store exactly one validated operations
pointer and expose checked logical methods; they never also store a legacy
helper. Stage 2 removes `FArrayPropertyHelper`, `FMapPropertyHelper`, the
positional Array/Map aliases, helper-taking runtime constructors, and silent
zero/null/no-op fallbacks. It retains transitional indexed Map wrappers backed
only by the new single-pass visitor so existing consumers compile until Stage 4
migrates them; Stage 5 removes those wrappers. Stage 3 removes legacy generated
declarations/bodies, switches every DHT initializer, and then removes the two
opaque positional ABI slots and typed-record source bridge retained solely for
the Stage 2/3 commit boundary. No runtime compatibility adapter or dual runtime
representation is added.

#### Stage 1 Consumer Capability Matrix

| Consumer operation | Required capabilities |
| --- | --- |
| Registration and compatibility schema | Valid descriptor lifecycle/layout and nested schema only |
| Logical Array equality / snapshot save / Array GC | `Count` plus `ConstTraversal`; random access is not required by generic consumers |
| Logical Map equality / snapshot save / Map GC | `Count` plus `ConstTraversal`; optional `Lookup` may accelerate equality |
| Streaming Archive save | Array/Map `Count` and `ConstTraversal`; no canonical buffering promise |
| Transactional Array load | `DetachedStorage`, `Reserve` when count is nonzero and budgeting requests it, applicable `DefaultGrow`, mutable access/traversal, and `TransactionalCommit`, intersected with inner construction/decode/destruction capabilities |
| Transactional Map load | `DetachedStorage`, optional `Reserve`, `Insert`, duplicate detection through insert/lookup, and `TransactionalCommit`, intersected with key/value construction/decode/destruction and adapter copy capabilities |
| DAST v2/canonical snapshot save | Map `Count`/`ConstTraversal` plus canonical-token support for the key schema; Array uses `Count`/`ConstTraversal` |
| Editor Array inspection/mutation | Inspection uses `Count`/`ConstTraversal`; mutation separately requests `Clear`, direction-specific resize, mutable traversal/access, and transaction capture for Undo/Redo |
| Editor Map inspection/mutation | Inspection uses `Count`/`ConstTraversal`; editing separately requests `MutableLookup` or mutable-mapped traversal, `Insert`, `RenameKey`, `Remove`, and `Clear` |

The completed upstream APIs satisfy the contract without extension:
`FReflectedValueStorage` owns temporary element values;
`FProperty::Can*Value` and checked construct/copy/destroy operations gate
lifecycle; `SerializeReflectedPropertyValue` supplies sticky Archive failures;
and `FDStructOps` remains the sole struct lifecycle, semantic serialization,
post-load, equality, and hidden-reference authority. Containers add no move
operation and request copy construction/assignment only along an adapter path
that actually uses it.

#### Stage 1 Handoff

- Baseline commit: `89963ba1` (`test(reflection): characterize reflected
  containers`). Stage 1 working set: this plan plus targeted validation of
  `DObjectGlobals.h/.cpp`, `DurinPropertyTypes.h`, `Property.cpp`, `StructOps.h`,
  and the completed prerequisite handoffs.
- Frozen public/private symbols: `ContainerOpsVersion`, the checked result and
  capability enums, `FArrayOps`, `FMapOps`, four visitor signatures, two typed
  parameter records and resolver types, validated `FArrayProperty`/
  `FMapProperty` checked wrappers, and private detached-container RAII used by
  loading consumers.
- Compatibility decisions: logical Array/Map kinds and signatures remain; old
  DAST v2 order remains readable; runtime helper/positional compatibility is
  intentionally compile-time atomic and has no adapter; arbitrary Archive save
  remains streaming while canonical writers opt in.
- Validation: the Stage 0 operation matrix maps to the final upstream value
  APIs without extending `FDStructOps`; every capability/slot, lifetime,
  threading, mutation, result, commit, and removal decision is closed. Plan
  validation passes; Stage 1 changes documentation only.

### Stage 2: Implement Typed Runtime Container Operations

Dependencies: Stage 1 contract and handoff.

- [x] Add immutable versioned Array and Map operation descriptors, capability
  queries, checked operation results, and registration validation.
- [x] Implement real typed Array and Map property parameter records without
  changing the completed typed Struct parameter contract.
- [x] Update generated-property construction to dispatch and validate typed
  container parameters before publishing a property.
- [x] Update `FArrayProperty` and `FMapProperty` to expose checked logical
  operations and remove silent missing-helper fallbacks.
- [x] Add single-pass Map visitor/cursor traversal and migrate internal property
  primitives away from indexed Map access.
- [x] Add managed detached container storage with exactly-once destruction and
  atomic commit/swap support.
- [x] Implement reusable constrained adapters for the supported
  `std::vector<T>` and `std::unordered_map<K,V>` forms.
- [x] Derive adapter capabilities from the concrete container plus inner/key/
  value property operations, leaving unsupported paths uninstantiated.
- [x] Add focused native tests for descriptor validation, missing capabilities,
  const/mutable traversal, invalidation, lifecycle counts, and rollback at the
  runtime property layer.
- [x] End the stage with a handoff listing the baseline commit, changed runtime
  types, adapter specializations, removed legacy APIs, and test results.

#### Acceptance Gate

- Array and Map properties register through distinct typed parameters and valid
  versioned operation descriptors.
- Runtime consumers cannot interpret an Array parameter as a Map parameter or
  read irrelevant positional fields.
- Unsupported element construction or copying compiles successfully, publishes
  no false capability, and returns the expected stable failure only when used.
- Map traversal performs O(n) iterator advances for n visited entries.
- Temporary containers and values are destroyed exactly once on success and on
  every injected failure path.

#### Stage 2 Handoff

- Baseline commit: `fe06bd4f` (`docs(reflection): freeze container operations
  contract`). Runtime working set: new `ContainerOps.h`, typed registration in
  `DObjectGlobals.h/.cpp`, checked Array/Map wrappers in
  `DurinPropertyTypes.h`/`Property.cpp`, and narrow capability-name updates in
  Archive, GC, AssetCore, and the editor view.
- `ContainerOpsVersion`, `EContainerOpResult`, `EArrayOpsFlags`, `EMapOpsFlags`,
  `FArrayOps`, `FMapOps`, four visitor types, and
  `FDetachedContainerStorage` are implemented. The reusable adapters accept
  only default-form `std::vector<T>` and `std::unordered_map<K,V>`; unsupported
  construction/copy paths remain uninstantiated and publish no capability.
- Runtime properties store one validated ops pointer. The legacy helper types,
  helper-taking runtime constructors, positional parameter aliases, and silent
  missing-helper fallbacks are removed. Transitional indexed Map wrappers use
  the visitor and remain only until the Stage 4 consumers migrate and Stage 5
  deletes the wrappers.
- The Stage 2/3 source boundary intentionally retains two unread opaque slots
  in `FPropertyParamsBase` plus a typed-record normalization constructor so old
  generated scalar/container initializers can coexist with this commit. Stage 3
  owns their removal together with all DHT-emitted helper tables.
- Validation: CoreObjectTests pass 66/66 and AssetPackageTests pass 39/39.
  Descriptor mismatch, unavailable immovable-vector capabilities, const and
  mutable single-pass traversal with early stop, duplicate rejection, detached
  rollback, commit swap, and exact lifecycle balance are covered. A focused
  `EditorPropertyTests` build reaches the expected Stage 3 boundary and fails
  only where existing Engine DHT output still names `FArrayPropertyHelper`.

### Stage 3: Generate Reusable Adapters and Strict Diagnostics

Dependencies: Stage 2 runtime registration and adapters.

- [x] Update DHT parsing to distinguish supported default-template forms from
  `vector<bool>`, explicit allocator/hash/equality variants, unsupported key
  kinds, and unsupported nesting.
- [x] Emit source-qualified stable diagnostics for every unsupported form before
  writing generated C++.
- [x] Emit typed Array and Map parameter construction with nested property
  descriptors and one reusable adapter specialization reference.
- [x] Stop emitting property-local Array Num/Get/Resize helpers and Map
  Num/Get/Create/Destroy/Insert/Rename/Remove helpers.
- [x] Stop emitting generic container lifecycle template addresses when typed
  registration and the reusable adapter are the authority.
- [x] Preserve aliases and qualified type resolution without spelling the
  storage backend into logical reflected signatures.
- [x] Update exact DHT output fixtures for direct, nested, struct-bearing, and
  reference-bearing containers, checking required and forbidden tokens.
- [x] Add negative generation tests for unsupported forms and capability-limited
  element types so failures occur at the intended diagnostic boundary.
- [x] Regenerate representative modules and verify no legacy helper declarations
  or positional Array/Map aggregate initializers remain.
- [x] End the stage with a handoff listing the baseline commit, generated working
  set, diagnostic IDs/text, representative before/after source measurements,
  and focused DHT validation.

#### Acceptance Gate

- Supported reflected containers generate concise typed metadata with reusable
  adapters and unchanged logical property schemas.
- Unsupported forms fail deterministically in DHT with property and requirement
  context; none fail later through proxy-address, constructor, copy, equality,
  or STL-member template errors.
- Generated output is byte-deterministic and contains no per-property container
  helper bodies or generic positional Array/Map parameter aggregates.
- Representative generated source size is reduced and the measurement method is
  recorded without making source-byte count a correctness contract.

#### Stage 3 Handoff

- Baseline commit: `3d8370af` (`feat(reflection): implement typed container
  operations`). Generated working set: DHT container recognition in
  `reflection_parser.py`, typed emission in `reflection_source_writer.py`, exact
  generation fixtures in `test_reflection_generation.py`, the Stage 2 bridge
  removal in `DObjectGlobals.h`, and typed handwritten registrations in the
  CoreDObject/AssetCore fixtures plus intrinsic math schemas.
- Supported source forms are the default `std::vector<T>` and
  `std::unordered_map<K,V>` specializations, including aliases resolved through
  canonical clang types and qualified nested reflected types. Generated records
  keep logical Array/Map schemas and reference `ResolveArrayOps<decltype(...)>`
  or `ResolveMapOps<decltype(...)>`; they emit no local container functions,
  helper tables, or generic container lifecycle addresses.
- Stable diagnostics are `[DHT-CONT001]` non-default vector allocator,
  `[DHT-CONT002]` `vector<bool>` proxy references, `[DHT-CONT003]` non-default
  unordered-map hash/equality/allocator, `[DHT-CONT004]` unsupported Map key,
  and `[DHT-CONT005]` nesting beyond depth four. Each includes the DPROPERTY
  name and source line and is raised during parsing before C++ generation.
- Repeating the Stage 0 hermetic-fixture method (UTF-8 byte length and
  `splitlines()`, with one new alias Array added for Stage 3 coverage) produces
  55,094 bytes / 727 lines with 10 Array and 5 Map resolver references. The
  Stage 0 fixture produced 121,783 bytes / 1,671 lines with 9 Arrays and 5 Maps,
  of which helper bodies occupied 49,302 bytes / 829 lines. This is diagnostic
  evidence rather than a source-size contract.
- A full DurinEditor `all` build regenerated 42 active `.gen.cpp` files. The
  generated tree contains 16 typed resolver references and zero matches for
  `FArrayPropertyHelper`, `FMapPropertyHelper`, `_ArrayNum(`, or `_MapGetKey(`.
  Focused validation: DHT tests pass 173/173, CoreObjectTests 66/66,
  AssetPackageTests 39/39, EditorPropertyTests 25/25, and the full `all` build
  succeeds. Stage 4 owns consumer transactions, canonical ordering, and final
  removal of transitional indexed Map wrappers.

### Stage 4: Make Consumers Capability-Aware and Transactional

Dependencies: Stage 3 generated/runtime integration.

- [x] Migrate GC schema compilation and visitation to checked Array traversal
  and single-pass Map traversal, preserving nested key/value/struct references.
- [x] Migrate runtime Archive and property snapshots to exact capability checks,
  sticky nested errors, bounded counts, and managed temporary containers.
- [x] Migrate AssetCore DAST v2 loading to temporary decode plus atomic commit,
  stable duplicate-key rejection, detailed property-path diagnostics, and
  unchanged destinations on failure.
- [x] Implement canonical Map ordering for new DAST v2 saves using the frozen
  logical key token while keeping historical unordered v2 files readable.
- [x] Apply canonical Map ordering to snapshot paths that advertise stable byte
  identity; retain streaming order only where an Archive explicitly does not
  promise canonical bytes.
- [x] Migrate compatibility inspection and type-signature recursion to typed
  container schema without changing `Array`/`Map` signatures.
- [x] Migrate reflected editor inspection and mutation to checked visitor,
  contains, insert, rename, and remove operations with actionable diagnostics.
- [x] Add fault injection across element construction, nested decode,
  post-deserialize rejection, insertion, duplicate detection, allocation/budget
  failure, and commit.
- [x] Add cross-insertion-order DAST v2 and stable-snapshot golden tests plus
  backward reads of historical unordered fixtures.
- [x] End the stage with a handoff listing the baseline commit, migrated
  consumers, canonical-order contract, compatibility fixtures, failure matrix,
  and focused suite results.

#### Acceptance Gate

- GC, serialization, snapshots, and editor traversal no longer use indexed Map
  access and retain complete nested-reference coverage.
- Missing capabilities fail at the requesting consumer with stable context; no
  Array or Map is silently skipped or treated as empty.
- Every failed container load leaves the destination logically unchanged and
  releases all temporary values exactly once.
- Duplicate decoded keys are rejected consistently by Archive/snapshot and
  AssetCore paths that accept external bytes.
- Equivalent supported Maps produce identical new DAST v2 and canonical
  snapshot bytes regardless of insertion, reserve, rehash, or bucket history.
- Historical valid DAST v2 files continue loading without resave or migration.

#### Stage 4 Handoff

- Baseline commit: `0d4ce2f3` (`feat(reflection): generate reusable container
  adapters`). Consumer working set: `Archive.cpp`/`Archive.h`,
  `GCReferenceSchema.cpp`, `AssetSystem.cpp`, `DurinPropertyTypes.h`/
  `Property.cpp`, and the two DurinEd reflected-property consumer sources.
- GC uses const Array and single-pass const Map visitors. Runtime Archive uses
  checked Count/traversal capabilities when saving; Array and Map loads decode
  into `FDetachedContainerStorage`, enforce the 10,000,000-entry bound, reject
  duplicate Map keys, and commit only after every nested value succeeds.
- `BuildCanonicalMapKeyToken` implements the version-1 typed token for bool,
  integral, floating-point, enum, string, comparison-name plus number, GUID,
  and complete reflected-struct keys. Object/container keys and structs with
  hidden or custom equality/serialization semantics fail before canonical
  output. DAST v2 always sorts by this token; property snapshot writers opt in
  through `FArchive::RequiresCanonicalMapOrder`, while ordinary streaming
  Archives retain backend iteration order.
- AssetCore keeps the `Array<...>` and `Map<...,...>` compatibility signatures
  and DAST version 2. Its diagnostics add `ArrayElement[index]`,
  `MapEntry[index].Key`, and `MapEntry[index].Value` context. A compatibility
  fixture reads deliberately reordered historical Map entries, while duplicate
  keys fail as corrupt input and leave no loaded package cached.
- DurinEd gathers Map entries through one mutable visitor and uses checked
  lookup/insert/remove/rename results with actionable messages. Stable edit-path
  resolution likewise performs one visitor pass instead of repeated indexed
  lookup. Transitional indexed Map wrappers now have no production consumers
  and remain solely for Stage 5 removal.
- Failure coverage includes oversized counts, truncated nested values,
  unavailable construction, injected detached-construction and commit failures,
  post-deserialize rejection, duplicate insertion, and destination rollback.
  Canonical snapshot and DAST tests cover opposite insertion order and distinct
  rehash histories.
- Validation: CoreObjectTests pass 66/66, AssetPackageTests pass 40/40,
  EditorPropertyTests pass 25/25, the complete native-test aggregate passes,
  and `build --target all` succeeds for `Win64-Debug-DurinEditor-Tests`.

### Stage 5: Qualify, Document, and Close the Migration

Dependencies: Stage 4 consumer migration.

- [x] Remove legacy helper aliases, constructors, silent fallback branches,
  indexed Map APIs, and obsolete generated fixtures after repository-wide
  generated output uses the new contract.
- [x] Run a repository-wide reflected-container audit and confirm every property
  resolves typed schema and a supported or intentionally capability-limited
  adapter.
- [x] Add regression coverage for arrays/maps of scalars, enums, strings, names,
  GUIDs, `FVector3`, ordinary reflected structs, structs with custom operations,
  object pointers, nested arrays, and map values containing arrays.
- [x] Add negative coverage for non-default-constructible, non-copyable,
  `vector<bool>`, unsupported key, custom-template-argument, missing-ops,
  duplicate-key, oversized-count, truncated-input, and post-load rejection
  cases.
- [x] Verify DAST v2 package save/load/resave, compatibility inspection,
  snapshots, duplication, Undo/Redo, GC, editor mutation, and object graph
  Archive behavior.
- [x] Repeat the Stage 0 source-volume and Map traversal measurements and record
  the before/after evidence.
- [x] Document the lasting reflected-container contract in the owning
  reflection/serialization documentation and leave this plan as implementation
  history rather than a competing specification.
- [x] Follow the repository build and test instructions for focused native
  suites and a successful full `all` build, using the required long-running
  command timeouts.
- [x] End the stage with a handoff listing the baseline commit, final working
  set, removed compatibility surfaces, validation commands/results, lasting
  documentation, and any evidence-gated follow-up.

#### Acceptance Gate

- No production registration or generated source uses
  `FArrayPropertyHelper`, `FMapPropertyHelper`, positional Array/Map aliases, or
  indexed Map traversal.
- All supported and intentionally unsupported container forms have explicit
  generation, registration, capability, and consumer behavior.
- Determinism, rollback, duplicate, lifecycle, GC, compatibility, editor, and
  complexity gates pass across direct and nested containers.
- A successful full `all` build and required focused suites confirm generated
  code, runtime modules, tools, tests, and editor consumers agree on the final
  descriptor ABI.
- Lasting documentation identifies the STL adapters as replaceable backends and
  names DAST v3 compact encoding as separate deferred roadmap work.

#### Stage 5 Handoff

- Baseline commit: `6c15d143` (`feat(reflection): make container consumers
  transactional`). Final implementation working set: the `FMapProperty` public
  surface and implementation, CoreDObject mutation/snapshot fixtures, and the
  lasting reflection and asset-package contracts. The implementation plan is
  completed in the same change.
- The three transitional indexed Map functions and their visitor context are
  removed. The remaining mutation test resolves a mapped value by logical key,
  and 55 lines of unused Stage 0 property-local Map helper fixture code are
  deleted. Repository searches find no production or generated use of
  `FArrayPropertyHelper`, `FMapPropertyHelper`, indexed Map access, positional
  Array/Map aliases, `_ArrayNum(`, or `_MapGetKey(`.
- The active generated tree contains 42 `.gen.cpp` files, 16 Array resolver
  references, no generated Map properties, and zero forbidden legacy tokens.
  Handwritten Map registrations remain confined to focused native-test
  fixtures and use `ResolveMapOps` plus typed schemas. Production reflected
  Arrays cover scalar bytes, intrinsic/reflected structs, object pointers, and
  editor/runtime structs; generated and handwritten tests cover the wider
  Array/Map matrix and intentionally capability-limited adapters.
- Repeating the Stage 0 hermetic DHT measurement gives 55,094 UTF-8 bytes / 727
  lines with 10 Array and 5 Map resolver references, unchanged from Stage 3 and
  down from 121,783 bytes / 1,671 lines at Stage 0. The single-pass traversal
  test observes exactly `n` callbacks for 0, 1, 4, and 16 Map entries and one
  callback when traversal stops early, replacing the former indexed quadratic
  iterator-advance behavior.
- Lasting ownership is documented in
  `Documentation/Runtime/Core/ReflectionSystem.md` for typed registration,
  capabilities, traversal, detached commit, adapter restrictions, and
  canonical-key support, and in
  `Documentation/Runtime/Assets/AssetPackages.md` for DAST v2 canonical save,
  historical reads, bounded transactions, duplicate rejection, and rollback.
  Compact DAST v3 encoding remains deferred to its separate roadmap.
- Validation: DHT tests pass 173/173; CoreObjectTests pass 66/66;
  AssetPackageTests pass 40/40; EditorPropertyTests pass 25/25; the complete
  `test --target all --agent` aggregate and `build --target all --agent` both
  succeed for `Win64-Debug-DurinEditor-Tests`. No evidence-gated follow-up is
  open.

## Validation Matrix

| Area | Required coverage |
| --- | --- |
| DHT parsing | Supported vector/unordered-map aliases and nesting; precise rejection of vector<bool>, custom template arguments, unsupported keys, and depth overflow |
| Generated registration | Typed Array/Map params, nested descriptors, reusable adapter specialization, deterministic output, and forbidden legacy helper bodies/thunks |
| Descriptor validation | Version, capability/slot consistency, schema links, kind-safe dispatch, owner layout/accessors, and handwritten registration parity |
| Array lifecycle | Empty/non-empty, grow/shrink, non-default element, non-copyable element, nested struct hooks, failure unwind, and atomic commit |
| Map lifecycle | Temporary key/value ownership, insert/collision/remove/rename, absent capabilities, duplicate decode, failure unwind, and atomic commit |
| Traversal | Array indexing, Map single-pass const/mutable visitation, mutation invalidation, empty containers, and O(n) iterator advances |
| Determinism | Different insertion orders, reserve/rehash histories, scalar/name/GUID/enum/float/struct keys, DAST v2 resave, and canonical snapshots |
| Serialization failure | Oversized counts, truncation at key/value/element boundaries, unsupported construction, post-load rejection, trailing bytes, and unchanged destination |
| Compatibility | Historical unordered DAST v2 reads, unchanged Array/Map signatures, unknown fields, inspection, and no version bump |
| GC and references | Direct and nested object references in array elements, map keys where handwritten support permits them, map values, structs, and nested containers |
| Editor mutation | Display, add, resize, clear, value edit, key collision, rename, remove, Undo/Redo, and actionable unsupported diagnostics |
| Integration | Archive snapshots, duplication, asset package round trip, compatibility reports, generated modules, focused native suites, and full all build |

## Definition of Done

- Reflected Array and Map properties use typed schema registration and immutable,
  versioned, capability-aware backend operation descriptors.
- Supported STL adapters are centralized and replaceable; generated properties
  no longer contain bespoke helper implementations.
- Reflected struct lifecycle and logical semantics have one authority even when
  the struct is an array element, map key, or map value.
- Unsupported construction, copying, mutation, traversal, or serialization
  requests fail deterministically without causing generated C++ compilation
  failures or silent data loss.
- Full Map traversal is O(n), keys are not mutated in place, and iterator
  lifetime never crosses structural mutation.
- Current deterministic writers canonicalize supported Maps independently of
  unordered-container history.
- Container loading is bounded and transactional, rejects duplicate keys, and
  preserves the original destination on every failure.
- DAST v2 logical schema and historical readability remain intact; no native
  container layout becomes persistent data.
- GC, snapshots, Archive, AssetCore, compatibility inspection, editor mutation,
  DHT tests, focused native suites, and the full build pass.
- The lasting reflection documentation owns the final contract, and this plan
  is marked complete with evidence before archival.

## Deferred Follow-ups

- An engine-owned Array/Map storage plan driven by allocator, memory tracking,
  inline capacity, cache behavior, debugging, or public API goals.
- Public registration of third-party or custom container adapters.
- Ordered maps, sets, optional/variant values, sparse containers, multi-maps,
  and heterogeneous lookup.
- Custom allocator/hash/equality adapters and persisted comparator/hash policy
  validation.
- Generic move-only element construction if future value operations add an
  explicit move-construction contract.
- Stable per-entry editor identities for reorderable or unordered containers.
- DAST v3 compact container opcodes, canonical wire tokens, compression, and
  default-relative container deltas under the Compact Asset Serialization
  roadmap.

## Related Documentation

- [Documentation entry point](../../../README.md)
- [Reflected Struct Operations Plan](ReflectedStructOperations.md)
- [Typed Struct Property Registration Plan](TypedStructPropertyRegistration.md)
- [Compact Asset Serialization Roadmap](../../../Roadmaps/Archive/2026-08/CompactAssetSerialization.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/reflection_parser.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DurinPropertyTypes.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/DObjectGlobals.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Property.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/GCReferenceSchema.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCompatibility.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/ReflectedPropertyEditing.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/PropertyChangeTests.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/ZPropertyValueSnapshotTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
