# Reflected Container Operations Plan

Summary: Turn the current STL-specific array and map helpers into capability-aware reflected container operations with typed registration, linear traversal, safe loading, and deterministic serialization boundaries.

Last reviewed: 2026-08-05

Status: Active
Completed:

## Current Status

- Planning is complete and Stage 0 is ready to begin as preparation work that
  may run alongside the Reflected Struct Operations implementation.
- DHT currently recognizes `std::vector<T>` as `FArrayProperty` and
  `std::unordered_map<K, V>` as `FMapProperty`. It does not expose a general
  reflected-container registration contract.
- Generated array helpers use indexed element access and `resize`; generated
  map helpers allocate default key/value temporaries, copy them into the map,
  and implement indexed access by advancing from `begin()` for every entry.
- Runtime Archive, GC, snapshots, authored DAST v2 serialization, and editor
  property mutation all consume the same helper tables, but they do not share
  explicit capability, failure, ordering, or transaction semantics.
- Production implementation after Stage 0 is blocked on completion of
  [Reflected Struct Operations](ReflectedStructOperations.md) and
  [Typed Struct Property Registration](TypedStructPropertyRegistration.md).
  This plan consumes their final value-operation and typed-parameter contracts
  without modifying either plan.
- The current STL storage backend remains in place. No engine-owned container
  type is required to complete this plan.

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
  [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md).
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

- [ ] Inventory every generated and handwritten reflected
  `std::vector`/`std::unordered_map` property by module, nested shape,
  element/key/value kind, reference content, serialization use, and editor use.
- [ ] Record every accepted and rejected standard-container spelling, including
  aliases, nested containers, explicit allocator/hash/equality arguments,
  `std::vector<bool>`, unsupported key kinds, and maximum nesting depth.
- [ ] Trace the current helper call graph through registration, Archive,
  snapshots, GC, AssetCore, compatibility inspection, and editor mutation.
- [ ] Measure current generated helper source volume and record representative
  compile-output baselines for direct, nested, struct, and reference-bearing
  containers.
- [ ] Measure Map traversal iterator advances for serialization and GC at
  multiple sizes and record the O(n^2) baseline without adding timing-only CI
  gates.
- [ ] Produce DAST v2 and snapshot probes for equivalent unordered maps built
  with different insertion orders and record which outputs currently diverge.
- [ ] Characterize current behavior for oversized counts, truncated elements,
  duplicate keys, failed struct post-load hooks, missing helpers, and partial
  destination mutation.
- [ ] Freeze the supported reflected Map key domain and specify one recursive
  canonical key-token ordering, including float NaN/signed-zero behavior and
  struct-field recursion where applicable.
- [ ] Record the exact upstream `FProperty`/`FDStructOps` capabilities needed by
  each proposed Array and Map operation without changing the upstream plans.
- [ ] End the stage with a handoff listing the baseline commit, working set,
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

### Stage 1: Freeze the Post-Prerequisite Container Contract

Dependencies: completed Reflected Struct Operations and Typed Struct Property
Registration plans, plus the Stage 0 handoff.

- [ ] Validate the final property value-operation, managed-storage, Archive
  error, and typed-parameter APIs against the Stage 0 operation matrix.
- [ ] Freeze the first `FArrayOps` and `FMapOps` descriptor layouts, versions,
  capability bits, checked result types, and descriptor validation rules.
- [ ] Freeze callback lifetime, mutation invalidation, const-key, traversal
  reentrancy, and thread-ownership rules.
- [ ] Freeze temporary-container creation, destruction, reserve, decode, and
  commit semantics required for transactional loading.
- [ ] Define distinct typed `FArrayPropertyParams` and `FMapPropertyParams`
  construction APIs and their safe common-base dispatch representation.
- [ ] Map each Archive, snapshot, GC, AssetCore, compatibility, and editor
  consumer to the minimum exact capabilities it may request.
- [ ] Decide the legacy helper and positional parameter removal boundary; no
  permanent dual runtime representation is allowed.
- [ ] Update the plan before implementation if the completed upstream contracts
  make any Stage 0 assumption invalid.
- [ ] End the stage with a handoff listing the baseline commit, working set,
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

### Stage 2: Implement Typed Runtime Container Operations

Dependencies: Stage 1 contract and handoff.

- [ ] Add immutable versioned Array and Map operation descriptors, capability
  queries, checked operation results, and registration validation.
- [ ] Implement real typed Array and Map property parameter records without
  changing the completed typed Struct parameter contract.
- [ ] Update generated-property construction to dispatch and validate typed
  container parameters before publishing a property.
- [ ] Update `FArrayProperty` and `FMapProperty` to expose checked logical
  operations and remove silent missing-helper fallbacks.
- [ ] Add single-pass Map visitor/cursor traversal and migrate internal property
  primitives away from indexed Map access.
- [ ] Add managed detached container storage with exactly-once destruction and
  atomic commit/swap support.
- [ ] Implement reusable constrained adapters for the supported
  `std::vector<T>` and `std::unordered_map<K,V>` forms.
- [ ] Derive adapter capabilities from the concrete container plus inner/key/
  value property operations, leaving unsupported paths uninstantiated.
- [ ] Add focused native tests for descriptor validation, missing capabilities,
  const/mutable traversal, invalidation, lifecycle counts, and rollback at the
  runtime property layer.
- [ ] End the stage with a handoff listing the baseline commit, changed runtime
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

### Stage 3: Generate Reusable Adapters and Strict Diagnostics

Dependencies: Stage 2 runtime registration and adapters.

- [ ] Update DHT parsing to distinguish supported default-template forms from
  `vector<bool>`, explicit allocator/hash/equality variants, unsupported key
  kinds, and unsupported nesting.
- [ ] Emit source-qualified stable diagnostics for every unsupported form before
  writing generated C++.
- [ ] Emit typed Array and Map parameter construction with nested property
  descriptors and one reusable adapter specialization reference.
- [ ] Stop emitting property-local Array Num/Get/Resize helpers and Map
  Num/Get/Create/Destroy/Insert/Rename/Remove helpers.
- [ ] Stop emitting generic container lifecycle template addresses when typed
  registration and the reusable adapter are the authority.
- [ ] Preserve aliases and qualified type resolution without spelling the
  storage backend into logical reflected signatures.
- [ ] Update exact DHT output fixtures for direct, nested, struct-bearing, and
  reference-bearing containers, checking required and forbidden tokens.
- [ ] Add negative generation tests for unsupported forms and capability-limited
  element types so failures occur at the intended diagnostic boundary.
- [ ] Regenerate representative modules and verify no legacy helper declarations
  or positional Array/Map aggregate initializers remain.
- [ ] End the stage with a handoff listing the baseline commit, generated working
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

### Stage 4: Make Consumers Capability-Aware and Transactional

Dependencies: Stage 3 generated/runtime integration.

- [ ] Migrate GC schema compilation and visitation to checked Array traversal
  and single-pass Map traversal, preserving nested key/value/struct references.
- [ ] Migrate runtime Archive and property snapshots to exact capability checks,
  sticky nested errors, bounded counts, and managed temporary containers.
- [ ] Migrate AssetCore DAST v2 loading to temporary decode plus atomic commit,
  stable duplicate-key rejection, detailed property-path diagnostics, and
  unchanged destinations on failure.
- [ ] Implement canonical Map ordering for new DAST v2 saves using the frozen
  logical key token while keeping historical unordered v2 files readable.
- [ ] Apply canonical Map ordering to snapshot paths that advertise stable byte
  identity; retain streaming order only where an Archive explicitly does not
  promise canonical bytes.
- [ ] Migrate compatibility inspection and type-signature recursion to typed
  container schema without changing `Array`/`Map` signatures.
- [ ] Migrate reflected editor inspection and mutation to checked visitor,
  contains, insert, rename, and remove operations with actionable diagnostics.
- [ ] Add fault injection across element construction, nested decode,
  post-deserialize rejection, insertion, duplicate detection, allocation/budget
  failure, and commit.
- [ ] Add cross-insertion-order DAST v2 and stable-snapshot golden tests plus
  backward reads of historical unordered fixtures.
- [ ] End the stage with a handoff listing the baseline commit, migrated
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

### Stage 5: Qualify, Document, and Close the Migration

Dependencies: Stage 4 consumer migration.

- [ ] Remove legacy helper aliases, constructors, silent fallback branches,
  indexed Map APIs, and obsolete generated fixtures after repository-wide
  generated output uses the new contract.
- [ ] Run a repository-wide reflected-container audit and confirm every property
  resolves typed schema and a supported or intentionally capability-limited
  adapter.
- [ ] Add regression coverage for arrays/maps of scalars, enums, strings, names,
  GUIDs, `FVector3`, ordinary reflected structs, structs with custom operations,
  object pointers, nested arrays, and map values containing arrays.
- [ ] Add negative coverage for non-default-constructible, non-copyable,
  `vector<bool>`, unsupported key, custom-template-argument, missing-ops,
  duplicate-key, oversized-count, truncated-input, and post-load rejection
  cases.
- [ ] Verify DAST v2 package save/load/resave, compatibility inspection,
  snapshots, duplication, Undo/Redo, GC, editor mutation, and object graph
  Archive behavior.
- [ ] Repeat the Stage 0 source-volume and Map traversal measurements and record
  the before/after evidence.
- [ ] Document the lasting reflected-container contract in the owning
  reflection/serialization documentation and leave this plan as implementation
  history rather than a competing specification.
- [ ] Follow the repository build and test instructions for focused native
  suites and a successful full `all` build, using the required long-running
  command timeouts.
- [ ] End the stage with a handoff listing the baseline commit, final working
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

- [Documentation entry point](../README.md)
- [Reflected Struct Operations Plan](ReflectedStructOperations.md)
- [Typed Struct Property Registration Plan](TypedStructPropertyRegistration.md)
- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

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
