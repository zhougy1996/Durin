# DAST V4 Default-Relative Reflection Plan

Summary: Establish deterministic production default-relative reflection, explicit override provenance, and no-delta policy without activating the DAST v4 writer or reader.

Last reviewed: 2026-08-08

Status: Completed
Completed: 2026-08-08

## Current Status

- Completed on 2026-08-08 in one squashed implementation commit based on the
  activated-plan baseline `752d68e8`. Production now owns immutable struct
  defaults, tri-state logical identity, paired class and default-subobject
  baselines, a canonical wire-neutral delta plan, and a pointer-free
  authored-override ledger with complete no-delta behavior.
- The real Default Material reference is 6,275 bytes with 554 emitted and 231
  omitted fields, 1,275 comparisons, maximum depth 5, and digest
  `0xC4111B7609C78D4F`. Repeated and reverse-order planning are identical, and a
  v3-loaded object allocates no ledger.
- Production remains DAST v3-only. All 17 tracked packages match the activation
  SHA-256 manifest and no `.dasset` changed. The
  [Compact Asset Serialization Roadmap](../Roadmaps/Archive/2026-08/CompactAssetSerialization.md)
  now activates only the
  [DAST V4 Deterministic Writer Plan](DASTV4DeterministicWriter.md); reader,
  registry, migration, and content rollout remain separately gated.

## Goal

Provide one production-owned logical delta layer that a later deterministic v4
writer and reader can consume without inventing default semantics. The completed
plan must provide:

- immutable deterministic type-default storage for every eligible `DStruct`;
- bounded tri-state logical identity with exact failure reasons across the
  complete authored value grammar;
- class-default and default-subobject baseline resolution for live object
  fields, plus type-default resolution for every explicit struct value;
- a deterministic logical delta plan that decides omission, explicit emission,
  forced emission, and unsupported states without emitting package bytes;
- an optional object-owned authored-override ledger keyed by stable logical
  paths, preserving loaded-explicit and forced intent across unchanged
  load/resave; and
- an explicit delta/no-delta policy whose full-state mode remains deterministic
  and fail-closed.

The result must be sufficient for the next writer plan to perform only frozen
table discovery and byte emission. It must not need to redesign default
eligibility, nested comparison, provenance, or full-state fallback.

## Scope

- Registration-time `DStruct` default eligibility, construction, validation,
  immutable publication, reference rooting, module ownership, and teardown.
- Tri-state logical identity for every current reflected property kind,
  including bit-exact float behavior, order-independent Maps, nested structs,
  containers, fixed arrays, hard references, soft references, and explicit
  unsupported diagnostics.
- Bounded pairing of a class default object/default-subobject graph with one
  live object graph by stable Outer-relative class/name identity.
- Object-field comparison against the corresponding immutable class-default
  field, including inherited fields and fixed-array elements.
- Struct-field comparison against one immutable type default per `DStruct`,
  independent of the containing class default.
- A wire-neutral recursive delta plan over the unified Archive logical grammar.
- Stable logical override paths for top-level fields, nested struct fields,
  fixed-array indices, Array positions, and Map values addressed by existing
  canonical logical key tokens.
- Loaded-explicit and forced-override provenance, explicit clear/reset
  operations, path validation, deterministic ordering, copy/move/lifecycle
  behavior, and conservative container mutation semantics.
- Delta-enabled and no-delta policy, including exact precedence between value
  difference, loaded-explicit state, forced state, and unavailable defaults.
- Focused CoreDObject, Archive, AssetCore reference-model, Material/default,
  lifecycle, malformed-input, determinism, and full-build qualification.
- Lasting reflection/default/provenance contracts in Runtime documentation and
  roadmap evidence sufficient to activate only the deterministic v4 writer.

## Non-Goals

- Implementing or exposing a production DAST v4 package writer, reader, header
  reader, compatibility probe, registry path, or migration edge.
- Changing `AssetVersion`, the frozen v4 opcodes or section bytes, current v3
  save/load behavior, or any tracked `.dasset`.
- Encoding table ids, schema ids, object ids, Value records, retained descriptor
  closures, or custom versions in production.
- General-purpose archetypes, mutable class defaults, Blueprint-style inherited
  template editing, arbitrary default-subobject graphs, or hot reload.
- Delta operations for Array/Map insert/remove/reorder. Containers remain
  atomic authored values; only struct records nested inside them may use their
  own type-default comparison.
- Inferring override intent from editor dirty state, transaction history,
  object flags, allocator identity, pointer addresses, field order, container
  capacity, or runtime registration order.
- Automatically remapping positional Array provenance after structural edits.
  Positional paths remain positional; this affects only explicitness, never the
  reconstructed logical value.
- Supporting custom struct serializers whose durable state is not completely
  represented by reflected fields. They continue to fail closed until the
  evidence-gated custom-codec milestone is activated.

## Selected Design and Invariants

### Unreal Engine Reference and Durin Boundary

The selected model follows several proven Unreal Engine separations while
retaining Durin's stricter ownership and deterministic authored contract:

- UE documents the `UClass` class default object as the object used for delta
  serialization and initialization, and normally exposes defaults immutably.
  Durin likewise uses only `DClass::GetDefaultObject()` as the object-field
  baseline; it does not add a mutable-default API.
- UE property comparison is owned by `FProperty::Identical` and
  `Identical_InContainer`. Durin keeps comparison in reflection rather than in
  AssetCore, but returns a tri-state result so unavailable semantics cannot be
  mistaken for an ordinary difference.
- UE separates archive-wide `DoDelta` from `DoIntraPropertyDelta`. Durin freezes
  one explicit `EDefaultDeltaMode` and does not add independent container-delta
  behavior because the v4 wire contract treats Array and Map as complete values.
- UE's overridden-property APIs address values by serialized property chains.
  Durin adopts an object-owned logical path ledger, but avoids UE's experimental
  global manager, edit notifications, mutable CDO behavior, and engine-specific
  instancing operations.

Primary reference points are Epic's official API documentation for
[`UClass::GetDefaultObject`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UClass/GetDefaultObject),
[`FProperty::Identical_InContainer`](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/UObject/FProperty/Identical_InContainer),
[`FArchive::DoDelta`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FArchive),
[`UStruct::SerializeTaggedProperties`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UObject/UStruct/SerializeTaggedProperties/1),
and
[`FOverriddenPropertySet`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FOverriddenPropertySet).
These sources inform responsibility boundaries only; Durin's frozen wire and
logical equality rules remain authoritative.

### Two Default Baselines

- A top-level object field is compared with the same declaring type, stable
  field name, and fixed-array index on the object's immutable class default.
  Inherited properties resolve through their declaring class but read from the
  most-derived class default object.
- A top-level field identical to its class-default counterpart is omitted only
  in delta-enabled mode and only when no explicit or forced ledger entry applies.
- Once a Struct value is emitted, every field inside that Struct is compared
  with the immutable type-default value owned by its `DStruct`, not with the
  corresponding nested memory inside the containing class default. This rule is
  recursive and applies equally inside fixed arrays, Arrays, and Map values.
- Consequently a class may author a non-type-default Struct member. The whole
  object field can still be omitted when it matches the class default. If it
  differs and must be emitted, the Struct block contains all differences from
  its type default, including fields needed to reconstruct the class-specific
  default state.
- Scalar, enum, string, Name, Guid, bytes, Array, Map, hard reference, and soft
  reference values are atomic at their owning schema field. Their logical
  contents participate in identity, but no element-level insert/remove delta is
  introduced.

### Immutable DStruct Defaults

- Each `DStruct` gains explicit default state and reason enums analogous to, but
  independent from, class defaults. Eligible states are `Uninitialized`,
  `Constructing`, `Ready`, `Unavailable`, `Failed`, and `Released`.
- Eligibility requires immutable initialized ops, valid size/alignment,
  `DefaultConstruct`, destruction support, complete authored fields, no custom
  serializer, and recursively supported logical identity for every
  non-`Transient` field. A custom authoritative `Identical` callback is allowed
  only when the authored fields are complete; it compares the complete value.
- Defaults are constructed eagerly after struct registration and property
  attachment, in qualified-name order, before the registration batch publishes.
  Lazy construction is forbidden.
- Registration constructs two detached candidates and compares them through the
  same tri-state identity path. A non-identical or unsupported pair fails the
  default capability with a stable reason before either candidate is published.
  The second candidate is then destroyed; the first becomes immutable storage.
- Default construction may allocate ordinary value-owned memory, but it may not
  publish runtime objects, touch assets, depend on wall-clock/random state, or
  mutate registration. Tests audit current production structs and exercise
  explicit nondeterministic and re-entrant failures.
- The storage is aligned, owns exactly one live value, and is destroyed through
  `FDStructOps`. Strong references reachable from a published default are
  traversed by the compiled struct reference schema and retained for the same
  lifetime as the owning `DStruct` metadata.
- Module-aware teardown releases struct defaults before the owning ops/code can
  unload. Public access is const-only and returns no raw mutable owner.

### Tri-State Logical Identity

- Introduce `EPropertyIdentityResult { Identical, Different, Unsupported }` and
  a bounded diagnostic carrying property path, logical kind, and stable reason.
  `ArePropertyValuesIdentical(...)` remains as a compatibility wrapper and
  returns true only for `Identical`.
- Identity walks reflection metadata, never serialized bytes or native struct
  padding. Scalar and enum bits, float signed zero, NaN sign/exponent/payload,
  strings, Names, Guids, and soft paths retain the existing exact rules.
- Struct `Identical` callbacks remain authoritative. Otherwise every
  non-`Transient` reflected field is compared recursively in stable schema order.
- Arrays compare length and ordered elements. Maps compare logical key/value
  associations using lookup and existing key identity, never bucket or iteration
  order. Missing operations return `Unsupported` rather than `Different`.
- Ordinary hard references use pointer identity. During class-default comparison
  only, a reference to a default subobject is identical to the uniquely paired
  live counterpart in the current object graph. External references do not gain
  path-based equivalence.
- Every walk is bounded to nesting depth 64, detects recursive descriptor cycles,
  and stops at the first deterministic non-identical or unsupported path.

### Class-Default Object Graph Pairing

- `FDefaultObjectGraphMap` pairs one immutable class-default graph with one live
  graph. Root objects pair directly. Supported default subobjects pair by the
  canonical sequence of `(qualified class, stable object name)` from the root.
- Pairing is read-only, bounded by the v4 object-count limit, and rejects
  duplicate sibling identities, missing or multiply matched required template
  nodes, class mismatches, invalid Outers, and cycles before comparison begins.
  Ordinary live children with no template counterpart are outside the map and
  do not invalidate comparison.
- Only objects flagged `ClassDefaultObject` or `DefaultSubobject` may appear on
  the template side. Only their corresponding ordinary live objects may appear
  on the instance side. Arbitrary referenced objects are not adopted into the
  graph.
- A class without a ready default cannot use delta-enabled planning. No-delta
  planning may still produce full logical state when every authored field is
  otherwise supported.

### Logical Delta Plan

- CoreDObject owns a wire-neutral `FDefaultDeltaPlan` and builder. The plan is a
  canonical logical tree of object/struct fields with descriptor identity,
  baseline kind, provenance decision, and child/value disposition. It contains
  no package-local ids or encoded bytes.
- Planning consumes the same `FArchiveFieldDescriptor` and
  `FArchiveLogicalTypeDescriptor` vocabulary used by unified Archive discovery.
  Reflected and native stable named fields therefore share one decision path.
- Reflected fields may compare their live/CDO containers directly. Native fields
  are compared by running the same `DObject::Serialize(FArchive&)` entry on the
  live object and immutable CDO through paired discovery and wire-neutral value-
  capture Archives. Both sides must produce the same frozen field manifest and
  logical types. Captured values are detached logical nodes, not encoded bytes,
  and object-reference comparison receives the same default-object graph map.
- Discovery closes the logical field manifest before comparison. Any field
  appearing only during the value pass, changing type, or changing order-sensitive
  authored identity is an internal deterministic failure.
- In delta-enabled mode, a field is omitted only when identity is `Identical`
  and no ledger entry requires emission. `Different` emits explicit provenance.
  `Unsupported` fails the plan.
- Struct emission recursively evaluates fields against the immutable type
  default. An empty Struct block is valid when the containing object field must
  be explicit even though the value equals the struct type default.
- Plan construction has no destination mutation, object construction, package
  lookup, dependency load, callback beyond declared struct identity, or byte
  publication.

### Authored Override Ledger

- Each ordinary `DObject` may lazily own one `FAuthoredOverrideLedger`. An empty
  ledger allocates nothing. Class defaults and default subobjects never own
  authored override state.
- `EAuthoredOverrideProvenance` has exactly `LoadedExplicit` and `Forced`.
  Absence means no authored intent. Unknown retained values remain AssetCore
  reader/writer state and do not enter this known-field ledger.
- A path begins with `(declaring type, field name, fixed-array index)` and may
  continue through Struct fields, fixed-array indices, Array positions, or Map
  values selected by the existing canonical key token. Paths never contain
  `FProperty*`, addresses, table ids, hashes without collision checks, or runtime
  iteration positions.
- Entries validate against the current immutable schema when inserted and when
  queried. Invalid, excessive, cyclic, duplicate, or noncanonical paths fail
  without partially mutating the ledger. Entries sort by complete logical path.
- Reflected path segments validate against compiled reflection metadata. Native
  Archive fields validate against the frozen discovery manifest captured from
  the object's `Serialize` entry; no native path is accepted from an unverified
  caller-provided string alone.
- `LoadedExplicit` means a v4 reader observed provenance `00`; it forces
  emission even when the current value equals today's default. A later ordinary
  value edit does not silently clear it. `Forced` means an explicit caller or
  no-delta plan requires provenance `01` and takes precedence over
  `LoadedExplicit`.
- `ClearOverride(path)` removes only the exact node. `ClearOverrideSubtree(path)`
  removes the node and descendants. `ResetOverrides()` clears all known-field
  intent. Clearing never changes the reflected value.
- Array routes are positional. Structural mutation does not attempt identity
  remapping; any still-valid position retains its mark and a missing position is
  ignored. Map routes use canonical key bytes and survive iteration-order
  changes. These rules can over-preserve explicitness but cannot change logical
  data or produce a false omission.
- Duplication copies the ledger only after the destination graph exists and
  revalidates every path against the destination schema. GC ignores path tokens;
  the ledger owns no object pointer. Destroying an object destroys its ledger.
- Current v3 loading creates no entries and current v3 saving never queries the
  ledger, proving exact byte compatibility.

### Delta and No-Delta Policy

- `EDefaultDeltaMode` has exactly `Enabled` and `NoDelta`. It is explicit input
  to logical planning; it is not inferred from Archive purpose or capabilities.
- `Enabled` uses the two default baselines and the ledger. Decision precedence
  is `Forced` -> provenance `01`; `LoadedExplicit` -> provenance `00`; logical
  `Different` -> provenance `00`; logical `Identical` -> omit; `Unsupported` ->
  fail.
- `NoDelta` emits every supported non-`Transient` field recursively with
  provenance `01`. It does not require a ready class or struct default, but it
  still requires complete authored fields, stable descriptors, bounded logical
  traversal, and every operation needed to read the value.
- No-delta is a diagnostic/authoring policy for later writer integration, not a
  compatibility escape hatch. It cannot serialize raw memory, unknown types, or
  incomplete custom structs, and it cannot change the frozen v4 wire.
- The later writer may expose no-delta only through an explicit low-level option;
  ordinary asset save remains delta-enabled once v4 is activated.

### Failure, Determinism, and Ownership

- Default creation, graph pairing, identity, ledger mutation, and planning fail
  before publication or destination mutation with stable typed reasons.
- Repeated runs and reversed registration/discovery order produce identical
  default eligibility, paths, delta trees, provenance, diagnostics, and counts.
- CoreDObject owns reflection identity, struct defaults, object-graph pairing,
  and the known-field ledger. AssetCore may adapt a completed logical plan into
  the already frozen v4 reference model but owns no alternative comparison.
- The test-only v4 reference codec remains the byte oracle. This plan may replace
  its conservative authored-presence oracle with the production logical plan for
  qualification, but it must not promote the codec into production.

## Current Foundations and Gaps

### Foundations

- `DClass::GetDefaultObject()` is stable, const-only, acquire-published, and
  lifecycle-qualified for all eligible production classes.
- `DStruct` already exposes immutable `FDStructOps`, default construction,
  destruction, copy, authoritative identity, complete-authored-field, custom
  serialization, repair, and reference capabilities.
- `FReflectedValueStorage` already owns detached aligned property values, and
  `ArePropertyValuesIdentical(...)` implements the complete current logical
  identity grammar.
- Unified Archive discovery already freezes stable named field descriptors and
  logical types before authored emission.
- Default Material and production-class tests already prove constructor/CDO
  parity, including the bounded default-subobject graph.
- Stage 2/3 v4 fixtures already encode default omission, changed defaults,
  loaded-explicit/forced provenance, nested structs, and conservative complete
  real-content feasibility.

### Gaps to Close

- Property identity conflates semantic difference with unavailable operations.
- `DStruct` default construction is callable but owns no published immutable
  baseline, determinism state, failure reason, rooting, or teardown contract.
- Class-default comparison does not pair default subobjects with their live
  counterparts.
- `FArchive` exposes purpose and capabilities but no independent default-delta
  policy or stable override-path context.
- There is no production logical delta tree shared by reflected and native
  Archive fields.
- There is no known-field explicitness sidecar, so a future v4 reader could not
  preserve an explicit value equal to a changed default.
- Current feasibility uses a deliberately conservative authored-presence oracle
  and therefore proves an upper bound rather than actual production omission.

## Implementation Stages

### Stage 0: Freeze eligibility and tri-state logical identity

- [x] Record activation baseline, any worktree exclusions, exact build/test
  profile, production class/struct counts, current default feasibility, and the
  symbols that own identity and lifecycle.
- [x] Introduce tri-state property identity and stable bounded diagnostics while
  retaining the existing bool wrapper for current callers.
- [x] Cover every logical kind, authoritative struct identity, reflected fallback,
  fixed arrays, Arrays, Maps, hard/soft references, bit-exact floats, unavailable
  operations, depth, and descriptor cycles.
- [x] Audit every registered production `DStruct` for default construction,
  destruction, complete authored fields, custom serializers, hidden state,
  identity support, references, and deterministic construction side effects.
- [x] Freeze exact struct-default eligibility and failure reasons. Activate the
  custom-codec roadmap milestone instead of proceeding if any current durable
  struct cannot be represented safely.
- [x] Record the stage handoff with baseline, initial five-file working set,
  added direct dependencies, symbols, audit results, open questions, and focused
  validation.

#### Acceptance Gate

- Difference and unsupported semantics are observably distinct at every nested
  path; no caller can silently omit a value after an unavailable comparison.
- The complete current property grammar has deterministic positive, negative,
  malformed, and order-perturbation coverage.
- Every current production struct has one explicit eligible/ineligible reason,
  and no opaque durable-state question remains before default publication.

#### Initial Working Set

- This plan.
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`.
- `Engine/Source/Runtime/CoreDObject/Public/DObject/StructOps.h`.
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Property.h`.
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Property.cpp`.

The first direct validation expansion is
`Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`. Archive,
AssetCore, Engine Material, and lifecycle files enter only when a later stage
begins or a focused test points to that direct dependency.

#### Stage 0 Handoff

- Baseline: architecture baseline `a0b67a4e`; activated-plan/implementation
  baseline `752d68e8`. The `architect` worktree on `feat/arch` was clean at
  entry, has no concurrent source/build writer, and excludes all other
  worktrees and their uncommitted state.
- Profile and frozen evidence: `windows-msvc-x64`, preset
  `Win64-Debug-DurinEditor-Tests`, target `CoreObjectTests`; 38 production
  reflected classes have explicit CDO disposition, 25 are eligible, and the
  frozen complete Default Material v4 reference remains 10,869 bytes against
  the 16,384-byte and 20,659-byte gates.
- Working set: the initial plan, `Class.h`, `StructOps.h`, `Property.h`, and
  `Property.cpp`; direct validation inspection expanded to
  `ReflectionTypeTests.cpp`, while the existing logical-identity fixture in
  `ZPropertyValueSnapshotTests.cpp` received the focused tests. The production
  struct audit additionally read only the ten headers that declare `DSTRUCT`
  types.
- Identity boundary: `ComparePropertyValues` now owns tri-state identity,
  bounded paths, exact logical kind/reason, depth 64, descriptor-cycle
  detection, checked container operations, and canonical-key-sorted Map
  diagnostics. `ArePropertyValuesIdentical` remains the compatibility wrapper
  and returns true only for `Identical`. `EPropertyIdentityResult`,
  `EPropertyIdentityReason`, `FPropertyIdentityDiagnostic`,
  `EDStructDefaultState`, and `EDStructDefaultReason` are the frozen Stage 1
  vocabulary.
- Audit: the DurinEditor production registration surface contains 26 structs:
  six Core math mirror structs, 15 Runtime `DSTRUCT` types, and five Editor
  `DSTRUCT` types. All 26 have valid deterministic member-initializer default
  construction, destruction, complete reflected authored state, and recursive
  identity support; none has a custom serializer or authoritative identity
  callback. `FMaterialParameterValue` is the sole reflected strong-reference
  carrier and is covered by the compiled reference schema. The two ImportRecord
  structs with post-deserialize callbacks keep only derived `FAssetPath` caches;
  their complete durable state is the reflected path text. Result: 26 eligible,
  zero custom-codec blockers, and no opaque durable state.
- Validation: `CoreObjectTests` built and all 82 tests passed. Focused tri-state
  tests distinguish value differences from missing operations, prove stable Map
  paths across perturbed insertion order, and reject descriptor cycles and depth
  overflow without mutation.
- Open questions: none. Stage 1 may publish defaults for all 26 audited structs;
  synthetic malformed/nondeterministic/re-entrant fixtures remain intentionally
  ineligible and must prove the typed failure paths.

### Stage 1: Publish immutable DStruct defaults and class baselines

- [x] Add `DStruct` default state/reason, const access, aligned owned storage,
  eager two-candidate construction/identity validation, atomic batch
  publication, and fail-closed rollback.
- [x] Compile recursive default eligibility after ops and properties freeze;
  reject custom serializers, incomplete authored fields, unsupported nested
  values, invalid size/alignment, re-entrancy, nondeterminism, and publication
  side effects.
- [x] Root references reachable from published struct defaults and release them
  before module-owned ops unload. Prove clean repeated initialize/shutdown and
  partial registration failure.
- [x] Implement bounded `FDefaultObjectGraphMap` pairing for the four supported
  actor default-subobject graphs, including duplicate/missing/extra/class/Outer/
  cycle failures.
- [x] Implement top-level class-default property resolution and recursive
  struct type-default resolution with exact inherited/fixed-array behavior.
- [x] Add production-wide class and struct default determinism tests, worker
  const-read tests, GC/rooting tests, and derived-first module teardown tests.
- [x] Record the stage handoff and any audit-driven scope correction before the
  logical delta planner begins.

#### Acceptance Gate

- Every eligible production struct publishes exactly one immutable deterministic
  default or reports one stable ineligibility reason before use.
- Default storage and default-subobject pairing survive GC, repeated lifecycle,
  partial failure, worker reads, and module teardown without stale pointers or
  callbacks into unloaded code.
- Object fields resolve only class-default baselines; explicit Struct values
  resolve only type defaults; tests prove the distinction with non-type-default
  class members.

#### Stage 1 Handoff

- Baseline: the activated-plan commit `752d68e8`; Stage 0 and Stage 1 are part of
  the same squashed implementation commit. The build profile remains
  `windows-msvc-x64` with preset `Win64-Debug-DurinEditor-Tests`.
- Working set: `Class.h/.cpp`, `DObjectGlobals.cpp`, `Object.cpp`,
  `ObjectLifecycle.h/.cpp`, `Property.h/.cpp`, the new
  `DefaultObjectGraph.h/.cpp`, `SplineCurve.cpp`, `LaunchEngineLoop.cpp`, and
  the CoreDObject/Material reflection tests. No Archive or AssetCore production
  serialization path changed.
- Default boundary: `DStruct::GetDefaultValue` publishes one acquire-visible,
  aligned immutable value only after two independently constructed candidates
  compare identical. `Private::CreateDStructDefaultsForBatch` closes over
  nested struct dependencies, orders qualified names, publishes atomically,
  and destroys all pending values and object-construction side effects on any
  failure. Eligibility is frozen before construction with stable reasons for
  layout, ops, authored completeness, serializer, descriptor, recursion,
  re-entrancy, nondeterminism, side effects, and construction failures.
- Lifecycle: published defaults participate in compiled/reflected and custom
  struct reference collection. Global and module-scoped release destroy values
  before module ops unload; Launch now releases struct defaults alongside CDOs.
  Tests prove worker const reads, GC rooting followed by module release, exact
  typed ineligibility, re-entrancy, nondeterminism, side-effect cleanup, and
  rollback after an earlier member of the same batch constructed successfully.
- Baseline resolution: `FDefaultObjectGraphMap` pairs CDO/default-subobject
  graphs by exact qualified class and stable name, permits unrelated live
  extras, and reports bounded typed failures for invalid roots, missing,
  duplicate, and class-mismatched nodes. Class properties compare to the
  most-derived CDO with graph-relative hard references; explicit struct values
  compare recursively to the immutable type default. A fixture with a
  class-specific `FVector3{1,2,3}` proves these baselines are distinct.
- Audit correction: all 26 frozen production structs publish `Ready`. The
  production test explicitly activates lazy `FVector4`; the other 25 are
  reached by the Material target's loaded modules. `FSplinePoint` was the sole
  audit defect: its no-argument default generated a GUID. It now defaults to an
  invalid stable GUID, while every curve insertion/duplication path continues
  assigning a unique GUID.
- Validation: focused default/graph/lifecycle tests pass; `CoreObjectTests`
  and all 78 `MaterialTests` pass; a full `all` build succeeds. Documentation
  validation is recorded in this completed plan.
- Open questions: none. Stage 2 may consume only the published default APIs,
  graph-relative identity, and tri-state diagnostics; it must not add a
  production v4 byte encoder.

### Stage 2: Build the canonical logical delta planner

- [x] Introduce wire-neutral delta-plan types, stable ordering, exact baseline
  kinds, typed failures, deterministic counts, and discovery/value-pass freeze.
- [x] Adapt reflected and native unified Archive fields into the same plan without
  duplicating logical type inference in AssetCore.
- [x] Implement object omission against class defaults and recursive Struct
  omission against type defaults across nested fixed arrays, Arrays, and Map
  values; keep containers atomic.
- [x] Prove empty explicit Struct blocks, class-specific struct defaults,
  inherited fields, default-subobject hard references, soft paths, canonical
  Maps, signed zero, NaNs, and custom authoritative identity.
- [x] Add repeated and reverse-discovery determinism, depth/count bounds,
  unsupported-operation rollback, and zero-mutation tests.
- [x] Connect the production logical plan to the test-only v4 reference model
  without adding a production encoder. Record new Default Material omitted/
  explicit counts, section sizes, digest, and comparison work.
- [x] Preserve the 16,384-byte and 20,659-byte gates; investigate any regression
  before proceeding rather than increasing the frozen budgets.
- [x] Record the stage handoff with exact plan cardinalities and reference-model
  changes.

#### Acceptance Gate

- One deterministic logical tree explains every omitted and emitted field and
  fails before output on every unavailable semantic.
- The same planner handles non-Material synthetic types and current Default
  Material without asset-specific logic.
- Test-only complete v4 bytes remain within both size gates with every byte
  owned by the frozen contract.

#### Stage 2 Handoff

- Baseline: the activated-plan commit `752d68e8`; Stages 0-2 are part of the
  same squashed implementation commit. The build profile remains
  `windows-msvc-x64` with preset `Win64-Debug-DurinEditor-Tests`.
- Working set: `Archive.h/.cpp`, the new `DefaultDeltaPlan.h/.cpp`,
  `ReflectionTypeTests.cpp`, and the AssetPackage test-only feasibility model,
  tests, and target linkage. Added direct dependencies are the Stage 1 default
  registry/graph APIs and the real Engine `DMaterial` test fixture; production
  AssetCore and its v3 writer remain unchanged.
- Key symbols and decisions: `BuildDefaultDeltaPlan`,
  `FDefaultDeltaPlan`, `FDefaultDeltaNode`, and
  `AreDefaultDeltaPlansEquivalent` expose a wire-neutral, pointer-free value
  plan with typed diagnostics and exact counters. Unified Archive supplies
  opt-in logical capture hooks while preserving ordinary byte serialization.
  Reflected and native fields share one canonical descriptor order; object
  fields compare to class defaults, explicit Struct values compare recursively
  to immutable type defaults, hard references use default-graph-relative
  identity, containers remain atomic at their parent, and authoritative
  `FDStructOps::Identical` wins when available. Capture freezes discovery/value
  shapes, enforces depth/path/count limits, and clears the output transactionally
  on every unsupported or malformed case.
- Frozen Default Material result: one object, 785 logical fields, 554 emitted,
  231 omitted, 1,275 comparisons, and maximum depth 5. The complete test-only
  package is 6,275 bytes with section sizes `{1803, 62, 107, 5, 4219}`, one
  override, 231 omitted defaults, 134 parse operations, 133 allocation inputs,
  and digest `0xC4111B7609C78D4F`; both 16,384-byte and 20,659-byte gates hold.
- Validation: planner-focused tests pass 2/2; `CoreObjectTests` pass 88/88 and
  `AssetPackageTests` pass 105/105. Coverage includes reverse discovery,
  repeated-plan equality, signed zero, NaN payloads, empty emitted Structs,
  class-specific and authoritative Struct identity, default subobject hard
  references, production inherited/soft/container fields, depth/count limits,
  manifest drift, unavailable defaults, rollback, and zero mutation.
- Open questions: none. Stage 3 owns authored-intent storage and precedence plus
  complete recursive no-delta forcing; Stage 2 deliberately adds no production
  v4 byte encoder.

### Stage 3: Preserve authored override intent and no-delta behavior

- [x] Add lazy object-owned ledger storage, canonical logical path tokens,
  schema/path validation, deterministic sorting, exact/subtree clear, reset,
  duplication, and lifecycle behavior.
- [x] Cover top-level and nested Struct fields, fixed arrays, positional Arrays,
  canonical-key Map values, invalid routes, excessive depth, duplicate entries,
  schema changes, removed fields, and unavailable key tokens.
- [x] Implement `LoadedExplicit` and `Forced` precedence in the logical planner.
  Prove explicit-equals-current-default, changed defaults, clear/revert, nested
  intent, and repeated unchanged load/resave models.
- [x] Add explicit `Enabled` and `NoDelta` modes. Prove no-delta emits every
  supported authored field as forced without requiring published defaults and
  still fails closed for incomplete/custom/unsupported values.
- [x] Prove current v3 load creates no ledger and current v3 save never observes
  it; retain exact production v3 byte goldens and tracked package hashes.
- [x] Exercise duplication, GC, object destruction, class/struct teardown, worker
  reads, and container path persistence without storing object pointers in the
  ledger.
- [x] Update the v4 reference fixtures to cover ledger-driven provenance `00`
  and `01` while leaving retained unknown provenance `02` under the frozen test
  model.
- [x] Record the stage handoff and the exact writer-facing API boundary.

#### Acceptance Gate

- An unchanged loaded-explicit field remains explicit even when equal to the
  current default; forced state always wins; clearing intent never changes the
  value.
- Path state is deterministic, schema-validated, pointer-free, and logically
  safe under supported container changes, duplication, GC, and teardown.
- No-delta is complete logical emission, not raw-memory fallback, and production
  v3 behavior remains byte-identical.

#### Stage 3 Handoff

- Baseline: the activated-plan commit `752d68e8`; Stages 0-3 are part of the
  same squashed implementation commit. The build profile remains
  `windows-msvc-x64` with preset `Win64-Debug-DurinEditor-Tests`.
- Working set: the new `AuthoredOverrideLedger.h/.cpp`, `Object.h`, Archive's
  canonical-Map-key notification path, `DefaultDeltaPlan.h/.cpp`, focused
  CoreDObject tests, the production-v3 AssetPackage regression, and the
  test-only v4 feasibility adapter/tests. No production AssetCore reader or
  writer behavior changed.
- Ledger contract: ordinary objects allocate no ledger until the first valid
  entry. `FAuthoredOverridePath` contains only declaring/name tokens, fixed or
  positional indices, and collision-checked canonical Map key bytes; it stores
  no reflection or object pointer. Copy-on-write immutable snapshots provide
  concurrent reads. Mutation validates discovery/value schema, bounds, token
  shape, provenance, and canonical keys before atomic publication; bulk replace
  rejects duplicates transactionally. Exact clear, subtree clear, and reset do
  not touch authored values. Stale removed fields, missing Array positions, and
  removed Map keys are ignored during planning, while incompatible routes fail
  closed. Duplication copies only after the destination graph exists and
  revalidates every entry.
- Planner precedence is `Forced` -> forced provenance, `LoadedExplicit` ->
  explicit provenance, logical difference -> explicit provenance, identical ->
  omit, with unsupported identity still failing. Nested intent forces the
  required parent records. Map intent follows canonical key bytes across
  iteration-order changes; Array intent remains positional. `NoDelta` walks the
  complete owned live graph, emits every supported field and logical child as
  forced, uses no class or struct defaults, and still rejects manifest drift,
  incomplete/custom reflected Structs, and unsupported capture.
- Writer-facing boundary: a future v4 writer consumes only
  `BuildDefaultDeltaPlan(...)` and the immutable `FDefaultDeltaPlan` tree.
  Readers/editors populate intent through `DObject::SetAuthoredOverride` or the
  transactional `ReplaceAuthoredOverrides`; callers clear via exact, subtree,
  or reset APIs. The writer does not query ledger storage directly and does not
  infer intent from object state.
- Validation: `CoreObjectTests` pass 91/91 and `AssetPackageTests` pass 106/106.
  The real v3 Default Material initially owns no ledger; repeated loaded-explicit
  plans/packages are identical, forced upgrade changes only provenance, and
  clear restores the Stage 2 plan. A production v3 create/save/resave/load test
  proves ledger state changes neither bytes nor loaded state. Stage 2's frozen
  Default Material baseline remains 6,275 bytes, 554 emitted, 231 omitted, and
  digest `0xC4111B7609C78D4F`.
- Open questions: none. Stage 4 may document and qualify these APIs, but must not
  add the production v4 writer in this plan.

### Stage 4: Qualify and hand off to the deterministic v4 writer

- [x] Run focused CoreDObject reflection/lifecycle, Archive, AssetPackage v3/v4
  reference, Material/default, StaticMesh/default-subobject, changed-document,
  and full `all` build validation through DurinDevTool.
- [x] Run `DevTool asset baseline`, verify every tracked package SHA-256 against
  the activation manifest, and prove no `.dasset` change.
- [x] Record production Default Material delta-plan counts, complete reference
  bytes, section budget, digest, comparison work, ledger allocation state, and
  repeated/reverse-order determinism.
- [x] Move lasting struct-default, tri-state identity, baseline, delta-plan,
  provenance, path, and no-delta contracts into owning Runtime documentation.
- [x] Complete this roadmap milestone and activate only the deterministic v4
  writer child. The writer boundary must consume the logical plan and frozen
  wire without absorbing reader, registry, or migration work.
- [x] Complete plan status/checklists and final handoff with baseline commits,
  working sets, symbols, decisions, open questions, validation, corpus hashes,
  measured budgets, and next-plan boundary.

#### Stage 4 Handoff

- Baseline: the activated-plan commit `752d68e8`; all five stages, lasting
  documentation, roadmap transition, qualification evidence, and plan closure
  are part of the same squashed implementation commit.
- Working set: `ReflectionSystem.md` owns immutable defaults, tri-state identity,
  class/default-subobject pairing, logical planning, override paths, provenance,
  and no-delta contracts. `AssetPackages.md` owns the measured v4 reference
  evidence. The compact-serialization roadmap completes this milestone and the
  new `DASTV4DeterministicWriter.md` is the sole activated child. No production
  writer/reader, registry policy, version constant, or asset content changed.
- Default Material reference: 6,275 complete bytes; section sizes 79-byte
  envelope, 1,803-byte Name, 62-byte Type, 107-byte Schema, 5-byte Object, and
  4,219-byte Value; 105 Names, 21 Types, 6 Schemas, 1 Object, and 1 top-level
  override. Planning visits 785 fields, emits 554, omits 231, performs 1,275
  comparisons at maximum depth 5, and produces digest
  `0xC4111B7609C78D4F`. The size margins are 10,109 bytes against the 16,384-byte
  target and 14,384 bytes against the 20,659-byte hard gate; parse/allocation
  work is 134/133. Repeated and reverse-registration/order runs are identical.
  The source v3-loaded object owns no override ledger; loaded-explicit and forced
  provenance change only the expected logical decisions.
- Asset evidence: `DevTool asset baseline --project Sandbox/Sandbox.dproject`
  reports 17 current DAST v3 packages. Independent SHA-256 comparison verifies
  all 17 entries against the activation manifest, and the worktree contains no
  changed `.dasset`.
- Validation: `CoreObjectTests` pass 91/91, `AssetPackageTests` pass 106/106,
  `MaterialTests` pass 78/78, and `StaticMeshTests` pass 49/49. Changed-document
  validation passes for four tracked changed documents, all-plan validation
  passes for 5 active, 8 completed, and 75 archived plans before this plan's
  final status transition, and the full `all` build passes with profile
  `windows-msvc-x64` and preset `Win64-Debug-DurinEditor-Tests`.
- Decisions and next boundary: the writer consumes only
  `BuildDefaultDeltaPlan(...)`, immutable `FDefaultDeltaPlan`, and the frozen
  wire contract. It owns canonical discovery/freeze and byte emission only.
  Reader activation, ledger population while loading, registry policy,
  migration, and content rollout remain out of scope. Open questions: none.

#### Acceptance Gate

- Focused suites, documentation validation, asset baseline, and full build pass
  from one coherent baseline with no authored-content changes.
- Lasting Runtime contracts and executable logical/reference goldens agree on
  every omission, provenance, unsupported, and no-delta rule.
- Production remains DAST v3-only. The next writer plan can limit itself to
  canonical discovery/freeze and the already frozen byte emission.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Struct eligibility | Every registered production struct has a stable state/reason; custom/incomplete/nondeterministic/re-entrant defaults fail before publication |
| Struct lifecycle | Eager atomic publication, exact aligned destruction, reference rooting, worker reads, repeated init/shutdown, and module teardown |
| Identity | Tri-state coverage for every logical kind, authoritative/reflected structs, arrays, maps, fixed arrays, references, float bits, bounds, cycles, and missing ops |
| Class baseline | Most-derived immutable CDO, inherited fields, fixed arrays, unavailable classes, and exact default-subobject graph pairing |
| Struct baseline | One immutable type default, recursive nested use, and class-specific member defaults distinguished from type defaults |
| Delta plan | Discovery freeze, canonical order, omit/emit explanation, zero mutation, reverse-order determinism, and stable diagnostics |
| Provenance | Loaded-explicit, forced, clear/reset, nested paths, arrays, maps, duplication, schema changes, and unchanged load/resave intent |
| No-delta | Complete supported field emission with forced provenance and no raw/custom fallback |
| Compatibility | Unknown values remain separate, removed paths fail or ignore deterministically as specified, and v3 behavior is untouched |
| Size and cost | Default Material complete test-reference bytes remain within 16,384 and 20,659 with section/cardinality/comparison diagnostics |
| Boundary | No production v4 bytes, reader, migration, registry policy, compressed block, or tracked package rewrite |
| Qualification | Focused native suites, changed/all documentation validation, asset baseline/hashes, and full `all` build |

## Definition of Done

- CoreDObject publishes deterministic immutable defaults for every eligible
  `DStruct` and reports typed unavailability for every other struct.
- Every authored property comparison returns identical, different, or
  unsupported with a stable bounded path and no destination mutation.
- Object fields compare against class defaults, Struct fields compare against
  type defaults, and default-subobject references compare through one bounded
  stable graph mapping.
- One wire-neutral logical delta planner produces deterministic omission and
  emission decisions for reflected and native Archive fields.
- Loaded-explicit and forced known-field intent survive unchanged logical
  round-trips through a pointer-free canonical ledger; no-delta forces complete
  supported state.
- The test-only v4 codec consumes production decisions and keeps Default Material
  within both frozen size gates without compression.
- Current production packages remain v3-only and byte-stable; all focused and
  full qualification gates pass.
- Lasting contracts reside in Runtime documentation and the roadmap activates
  only the deterministic v4 writer next.

## Deferred Follow-Ups

- Production v4 table discovery, canonical byte emission, public writer API,
  and atomic package publication.
- Production v4 loading, ledger population from provenance, retained unknown
  descriptor closures, compatibility inspection, and rollback.
- Mixed v3/v4 registry/cache policy and explicit migration.
- Editor UI for viewing or changing forced override state.
- Container edit notifications or semantic Array-element identity beyond the
  frozen positional policy.
- Custom struct asset codecs unless a later audit activates the evidence-gated
  milestone.

## Related Documentation

- [Compact Asset Serialization Roadmap](../Roadmaps/Archive/2026-08/CompactAssetSerialization.md)
- [DAST V4 Measurement and Wire Contract Plan](DASTV4MeasurementAndWireContract.md)
- [Class Default Object Lifecycle Plan](ClassDefaultObjectLifecycle.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/StructOps.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Property.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Property.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h`
- `Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV4ReferenceModelTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV4FeasibilityTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialSchemaAndEditingTests.cpp`
