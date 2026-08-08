# DAST V4 Default-Relative Reflection Plan

Summary: Establish deterministic production default-relative reflection, explicit override provenance, and no-delta policy without activating the DAST v4 writer or reader.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

- Activated on 2026-08-08 from baseline `a0b67a4e` after the
  [DAST V4 Measurement and Wire Contract Plan](DASTV4MeasurementAndWireContract.md)
  completed every exit gate. The frozen v4 contract, test-only codec, and
  10,869-byte Default Material golden are established inputs; this plan does
  not reopen their bytes.
- The [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
  selects this as the next required child. Deterministic v4 writer, reader,
  migration, and content rollout remain separately gated later children.
- All 25 eligible production classes already own immutable class default
  objects, and all 38 production reflected classes have an explicit default
  disposition. Existing property identity recursively covers scalar, string,
  Name, Guid, enum, struct, fixed array, Array, Map, hard reference, and soft
  reference values.
- The missing production layer is semantic rather than wire-level: reflected
  equality cannot distinguish `Different` from `Unsupported`; `DStruct` owns no
  immutable type-default value; there is no class-default/default-subobject
  baseline resolver; Archives cannot request or suppress delta behavior; and a
  live object cannot retain loaded-explicit or forced override intent by stable
  logical path.

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

- [ ] Record activation baseline, any worktree exclusions, exact build/test
  profile, production class/struct counts, current default feasibility, and the
  symbols that own identity and lifecycle.
- [ ] Introduce tri-state property identity and stable bounded diagnostics while
  retaining the existing bool wrapper for current callers.
- [ ] Cover every logical kind, authoritative struct identity, reflected fallback,
  fixed arrays, Arrays, Maps, hard/soft references, bit-exact floats, unavailable
  operations, depth, and descriptor cycles.
- [ ] Audit every registered production `DStruct` for default construction,
  destruction, complete authored fields, custom serializers, hidden state,
  identity support, references, and deterministic construction side effects.
- [ ] Freeze exact struct-default eligibility and failure reasons. Activate the
  custom-codec roadmap milestone instead of proceeding if any current durable
  struct cannot be represented safely.
- [ ] Record the stage handoff with baseline, initial five-file working set,
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

### Stage 1: Publish immutable DStruct defaults and class baselines

- [ ] Add `DStruct` default state/reason, const access, aligned owned storage,
  eager two-candidate construction/identity validation, atomic batch
  publication, and fail-closed rollback.
- [ ] Compile recursive default eligibility after ops and properties freeze;
  reject custom serializers, incomplete authored fields, unsupported nested
  values, invalid size/alignment, re-entrancy, nondeterminism, and publication
  side effects.
- [ ] Root references reachable from published struct defaults and release them
  before module-owned ops unload. Prove clean repeated initialize/shutdown and
  partial registration failure.
- [ ] Implement bounded `FDefaultObjectGraphMap` pairing for the four supported
  actor default-subobject graphs, including duplicate/missing/extra/class/Outer/
  cycle failures.
- [ ] Implement top-level class-default property resolution and recursive
  struct type-default resolution with exact inherited/fixed-array behavior.
- [ ] Add production-wide class and struct default determinism tests, worker
  const-read tests, GC/rooting tests, and derived-first module teardown tests.
- [ ] Record the stage handoff and any audit-driven scope correction before the
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

### Stage 2: Build the canonical logical delta planner

- [ ] Introduce wire-neutral delta-plan types, stable ordering, exact baseline
  kinds, typed failures, deterministic counts, and discovery/value-pass freeze.
- [ ] Adapt reflected and native unified Archive fields into the same plan without
  duplicating logical type inference in AssetCore.
- [ ] Implement object omission against class defaults and recursive Struct
  omission against type defaults across nested fixed arrays, Arrays, and Map
  values; keep containers atomic.
- [ ] Prove empty explicit Struct blocks, class-specific struct defaults,
  inherited fields, default-subobject hard references, soft paths, canonical
  Maps, signed zero, NaNs, and custom authoritative identity.
- [ ] Add repeated and reverse-discovery determinism, depth/count bounds,
  unsupported-operation rollback, and zero-mutation tests.
- [ ] Connect the production logical plan to the test-only v4 reference model
  without adding a production encoder. Record new Default Material omitted/
  explicit counts, section sizes, digest, and comparison work.
- [ ] Preserve the 16,384-byte and 20,659-byte gates; investigate any regression
  before proceeding rather than increasing the frozen budgets.
- [ ] Record the stage handoff with exact plan cardinalities and reference-model
  changes.

#### Acceptance Gate

- One deterministic logical tree explains every omitted and emitted field and
  fails before output on every unavailable semantic.
- The same planner handles non-Material synthetic types and current Default
  Material without asset-specific logic.
- Test-only complete v4 bytes remain within both size gates with every byte
  owned by the frozen contract.

### Stage 3: Preserve authored override intent and no-delta behavior

- [ ] Add lazy object-owned ledger storage, canonical logical path tokens,
  schema/path validation, deterministic sorting, exact/subtree clear, reset,
  duplication, and lifecycle behavior.
- [ ] Cover top-level and nested Struct fields, fixed arrays, positional Arrays,
  canonical-key Map values, invalid routes, excessive depth, duplicate entries,
  schema changes, removed fields, and unavailable key tokens.
- [ ] Implement `LoadedExplicit` and `Forced` precedence in the logical planner.
  Prove explicit-equals-current-default, changed defaults, clear/revert, nested
  intent, and repeated unchanged load/resave models.
- [ ] Add explicit `Enabled` and `NoDelta` modes. Prove no-delta emits every
  supported authored field as forced without requiring published defaults and
  still fails closed for incomplete/custom/unsupported values.
- [ ] Prove current v3 load creates no ledger and current v3 save never observes
  it; retain exact production v3 byte goldens and tracked package hashes.
- [ ] Exercise duplication, GC, object destruction, class/struct teardown, worker
  reads, and container path persistence without storing object pointers in the
  ledger.
- [ ] Update the v4 reference fixtures to cover ledger-driven provenance `00`
  and `01` while leaving retained unknown provenance `02` under the frozen test
  model.
- [ ] Record the stage handoff and the exact writer-facing API boundary.

#### Acceptance Gate

- An unchanged loaded-explicit field remains explicit even when equal to the
  current default; forced state always wins; clearing intent never changes the
  value.
- Path state is deterministic, schema-validated, pointer-free, and logically
  safe under supported container changes, duplication, GC, and teardown.
- No-delta is complete logical emission, not raw-memory fallback, and production
  v3 behavior remains byte-identical.

### Stage 4: Qualify and hand off to the deterministic v4 writer

- [ ] Run focused CoreDObject reflection/lifecycle, Archive, AssetPackage v3/v4
  reference, Material/default, StaticMesh/default-subobject, changed-document,
  and full `all` build validation through DurinDevTool.
- [ ] Run `DevTool asset baseline`, verify every tracked package SHA-256 against
  the activation manifest, and prove no `.dasset` change.
- [ ] Record production Default Material delta-plan counts, complete reference
  bytes, section budget, digest, comparison work, ledger allocation state, and
  repeated/reverse-order determinism.
- [ ] Move lasting struct-default, tri-state identity, baseline, delta-plan,
  provenance, path, and no-delta contracts into owning Runtime documentation.
- [ ] Complete this roadmap milestone and activate only the deterministic v4
  writer child. The writer boundary must consume the logical plan and frozen
  wire without absorbing reader, registry, or migration work.
- [ ] Complete plan status/checklists and final handoff with baseline commits,
  working sets, symbols, decisions, open questions, validation, corpus hashes,
  measured budgets, and next-plan boundary.

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

- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
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
