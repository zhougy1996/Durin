# Reflection Metadata, Evolution, and Type Coverage Plan

Summary: Add typed property metadata, versioned reflected-schema migration, weak object properties, and explicit precision math intrinsics across generation, runtime reflection, serialization, and editor workflows.

Last reviewed: 2026-08-21

Status: Active
Completed:

## Current Status

The plan is ready for Stage 0. Durin already has generated property trees,
untyped string metadata, property and type `LegacyNames`, immutable class and
struct defaults, authored override tracking, field-tagged DAST v4 packages,
custom-version transport, transactional reflected-value loading, hard and soft
object properties, and stable weak object handles. These foundations should be
extended rather than replaced.

The selected work closes four related gaps. Property metadata is not validated
or represented as a typed contract. Compatibility aliases cannot express a
field type change, split, merge, or value transformation. `TWeakObjectPtr<T>`
is not a generated property kind. Precision-specific math aliases are
incomplete at the reflection boundary: float vectors are supported, while the
explicit `FVector*d` spellings, a float quaternion, and the float 4x4 matrix do
not have a complete authored-property path.

Implementation has not started. Stage 0 must freeze the two compatibility
decisions that affect persistent identity: whether explicit `FVector*d`
spellings canonicalize to the existing `FVector*` identities, and whether
schema migration selects its source version through a persisted per-type
version or an existing schema/custom-version identity. No later stage may
silently choose either policy in generated code.

## Goal

Make reflected authoring contracts precise enough for the editor to present and
validate values consistently, old authored packages to migrate through explicit
deterministic transformations, non-owning runtime references to participate in
reflection without becoming GC roots or accidental durable identities, and
precision-specific math values to be declared directly as reflected fields.

The finished system must preserve native C++ layout, transactional loading,
canonical saves, existing `FVector*` and `FQuat` asset identities, and the
distinction between hard, weak, and soft object ownership.

## Scope

- A versioned first-party vocabulary for numeric and presentation property
  metadata, including display name, tooltip, category, units, editor step,
  decimal precision, hard authoring bounds, and optional UI bounds.
- DHT parsing, type applicability checks, duplicate detection, canonical
  generated records, runtime typed access, and generic Details-panel
  consumption for the selected metadata vocabulary.
- A reflected-schema migration substrate for class and struct fields that can
  rename, remove, add, split, merge, and transform detached logical values
  before they are committed to live objects.
- Deterministic migration selection, chaining, diagnostics, bounds, rollback,
  unknown-field interaction, canonical resave behavior, compatibility audit,
  and fixtures representing at least two historical schemas.
- `TWeakObjectPtr<T>` as a generated reflected leaf kind for direct fields,
  fixed arrays, Array elements, Map values, and nested reflected structs.
- Weak-reference behavior for property snapshots, editable copies,
  duplication, default comparison, object-graph Archives, GC, and authored
  package attempts; weak Map keys remain excluded.
- DHT acceptance of explicit `FVector2d`, `FVector3d`, and `FVector4d`
  property spellings without changing the established default double-vector
  persistence contract.
- A Core-owned `FQuatf` alias and reflected intrinsic, plus reflection for the
  existing `FMatrix4f` alias. Add an explicit `FQuatd` alias if required to
  make the existing `FQuat` default-precision relationship symmetrical, while
  keeping `FQuat` canonical.
- Float quaternion component metadata and a column-major `FMatrix4f` schema
  built from four `FVector4f` columns; serialization must never use padding or
  an opaque raw-byte fallback.
- Runtime, DHT, AssetCore, editor, compatibility-audit, and documentation
  coverage for the new contracts.

## Non-Goals

- Function reflection, script binding, RPC, hot reload, general archetype
  graphs, or editable class-default authoring.
- A free-form metadata language, arbitrary callbacks in metadata, or
  cross-property edit conditions in the first typed-metadata version.
- Automatically guessing semantic migrations from C++ conversions or silently
  narrowing, clamping, or repairing loaded values.
- Replacing DAST v4, removing retained unknown fields, or creating a second
  package serializer beside the existing Archive and AssetCore paths.
- Giving `TWeakObjectPtr` durable asset identity, allowing it to retain a GC
  target, or treating it as a substitute for `TSoftObjectPtr`.
- Weak-reference Map keys. Their hash/equality would change as objects retire
  and would violate container invariants.
- Reflecting every GLM type. Float/double 2x2 and 3x3 matrices, integer vectors,
  generic template vectors/matrices, and precision-generic reflected identities
  require demonstrated authored use and remain follow-ups.
- Changing math storage, alignment, component order, matrix convention, or the
  established double-precision `FVector*`, `FQuat`, and `FMatrix` aliases.

## Design Decisions and Invariants

### Typed metadata

- DHT owns spelling, parsing, applicability, and cross-key validation. Invalid
  numbers, unknown units, duplicate keys, impossible ranges, or metadata on an
  unsupported property kind are generation errors, not runtime strings that an
  editor may interpret differently.
- Generated metadata is canonical and runtime metadata is immutable after
  registration. Existing raw metadata lookup may remain as a compatibility
  view for unselected keys, but first-party consumers use typed accessors.
- `ClampMin` and `ClampMax` are authoring constraints. Editor proposals and
  explicit validation report violations; package loading does not silently
  clamp historical data. `UIMin` and `UIMax` affect presentation only and must
  remain inside hard bounds when both exist.
- Numeric metadata converts through the property's exact signed, unsigned,
  float, double, enum, or vector-component representation. It must not route
  every value through `double` and lose 64-bit integer precision.
- Category, tooltip, display name, units, step, precision, and bounds compose
  predictably for fixed arrays, containers, and nested structs. Container-level
  metadata does not accidentally become element metadata unless the contract
  says so explicitly.
- All user-visible edits still enter the existing property transaction and
  authored-intent path. A widget must not mutate an object merely because it
  parsed metadata successfully.

### Schema evolution

- `LegacyNames` remains the zero-code path for identity-preserving renames.
  Migration is selected only when aliases and exact logical compatibility
  cannot produce the current schema.
- Migration operates on bounded detached logical values and retained unknown
  fields before live-object assignment. A failed, ambiguous, incomplete, or
  over-budget migration publishes no object mutation and preserves the prior
  package or in-memory destination.
- Every migration step has an explicit source identity, destination identity,
  deterministic order, and diagnostic name. Chains advance monotonically and
  cycles, gaps, duplicate registrations, and multiple matching paths fail
  closed.
- Migration callbacks cannot inspect arbitrary live worlds, load assets,
  create objects, depend on registration order, or retain pointers into Archive
  storage. Reference transformations use logical hard/soft tokens supplied by
  the migration context.
- Successful load records the current canonical field names, logical types,
  schema/version identity, and authored override paths. A subsequent save emits
  only the current schema while preserving still-unknown values that were not
  claimed by the migration.
- Existing package custom versions and schema descriptors are reused where
  they can provide an unambiguous source identity. Stage 0 chooses the smallest
  persisted extension that can distinguish every supported historical schema;
  it must not add both a per-type version and a schema fingerprint without a
  demonstrated need.
- Removal of a migration step follows the same authored-content baseline,
  canonical-resave, and compatibility-policy gates as removal of a legacy
  reflection alias.

### Weak references

- `FWeakObjectProperty` stores typed `TWeakObjectPtr<T>` operations and an
  expected reflected class. It never reinterprets wrapper memory as another
  handle type.
- GC schema compilation ignores weak properties and weak values nested in
  structs or containers. Property snapshots and drafts may copy the weak handle
  but must not add the target to their rooted reference set.
- Process-local object-graph and duplication paths may remap a weak target only
  when the target is already present for structural or hard-reference reasons;
  traversing a weak property never enlarges the graph. An unresolved target
  becomes null with a typed diagnostic or policy selected by the Archive
  purpose, never a new strong edge.
- A weak reference has no durable asset identity. DHT requires weak properties
  intended only for runtime caches to be `Transient`; a non-transient weak
  property is rejected unless Stage 0 selects and documents a stricter authored
  null-only contract. Non-null weak handles are never encoded as soft paths.
- Weak Map values are permitted after container operations and snapshot
  semantics are qualified. Weak Map keys are rejected at parse and runtime
  registration boundaries.
- Comparison uses stable handle identity while both handles are representable;
  expired handles compare as null according to the frozen property contract.
  Retirement must not mutate container structure or authored override tokens.

### Precision math identities

- Existing `FVector2`, `FVector3`, `FVector4`, `FQuat`, and `FMatrix` meanings
  and serialized identities do not change.
- Explicit `FVector*d` spellings use double components and must be accepted by
  DHT. Stage 0 decides whether they resolve to the existing canonical vector
  descriptors or distinct alias descriptors; the choice must avoid accidental
  schema churn for existing fields and must be covered by round-trip fixtures.
- `FQuatf` is a distinct float quaternion descriptor with the established
  `(w, x, y, z)` field order. `FMatrix4f` exposes four `FVector4f` columns in
  column-major order with stable names. Editor presentation may be specialized,
  but generic reflected-field editing and serialization are mandatory.
- Intrinsic math defaults are deterministic, finite, and documented. Runtime
  registration uses `FDStructOps` and accessor-backed fields where GLM aliases
  do not expose a safe standard-layout offset.
- Logical identity and serialization walk components in the declared schema;
  no `memcmp`, ABI-dependent padding, or target-specific GLM type name enters a
  package.

## Current Foundations and Gaps

| Area | Existing foundation | Gap selected by this plan |
| --- | --- | --- |
| Metadata | Generated key/value pairs, `FField` lookup, `DisplayName`, `HideAlpha`, and `DefaultCollapsed` consumers | No typed vocabulary, applicability validation, exact numeric parsing, common editor constraints, or shared validation result |
| Compatibility | Type/property `LegacyNames`, captured alias catalog, field-tagged logical descriptors, retained unknown values, custom versions, transactional loading | No deterministic reflected-field transform for type changes, splits, merges, or value conversion |
| Weak handles | Stable index/generation `TWeakObjectPtr`, immediate invalidation on pending kill | No DHT property, runtime node, containers, snapshots, Archive policy, or Details presentation |
| Math | Float vectors, default double vectors/quaternion/matrix, float matrix alias, intrinsic struct registration | Explicit double-vector spellings, `FQuatf`, and `FMatrix4f` are not complete reflected authored types |

## Implementation Stages

### Stage 0: Freeze syntax, identity, and persistence contracts

- [ ] Inventory every current metadata key and consumer; classify each as a
  selected typed key, retained extension key, deprecated key, or invalid key.
- [ ] Freeze DPROPERTY syntax and exact applicability rules for display name,
  tooltip, category, units, step, precision, hard bounds, and UI bounds.
- [ ] Decide canonical versus alias descriptors for explicit `FVector*d`
  spellings and record save/load consequences with old/new schema examples.
- [ ] Select the schema migration source identity using current DAST v4 schema
  records and custom versions; specify wire changes only if existing identities
  cannot disambiguate historical schemas.
- [ ] Freeze weak-reference behavior by Archive purpose, including DHT's
  persistability rule, expired-handle comparison, and duplication remapping.
- [ ] Add fixture-only contract tests that fail for every selected unsupported
  case before production implementation begins.


#### Acceptance Gate

- The four contracts above have no unresolved identity or persistence decision;
  baseline tests demonstrate current failures without changing production
  behavior; the plan and owning runtime documents agree.

### Stage 1: Complete precision-specific math intrinsics

- [ ] Add the selected `FQuatf`/optional `FQuatd` aliases at the Core math
  boundary without changing existing aliases or ABI.
- [ ] Teach DHT to resolve explicit `FVector2d/3d/4d`, `FQuatf`, and
  `FMatrix4f` source spellings according to the Stage 0 identity policy.
- [ ] Register intrinsic descriptors, float component properties, deterministic
  defaults, matrix column accessors, and complete reflected struct operations.
- [ ] Cover direct/fixed-array/container declarations, snapshots, default
  comparison, Archive and DAST v4 round trips, and generic Details editing.
- [ ] Update the math and reflection contracts with canonical identity and
  component/column ordering.

#### Acceptance Gate

- Every selected math spelling generates and compiles as a `DPROPERTY`, reports
  the intended component kinds and sizes, round-trips without raw bytes, and
  preserves existing double-precision asset fixtures byte-for-byte where the
  schema is unchanged.

### Stage 2: Publish typed property metadata

- [ ] Add typed DHT model fields and canonical generated parameter records for
  the selected metadata vocabulary while preserving required extension
  metadata compatibility.
- [ ] Reject malformed, duplicate, inapplicable, non-finite, out-of-order, or
  non-representable metadata with source-located diagnostics.
- [ ] Publish immutable runtime typed metadata and exact numeric conversion and
  validation helpers without using `double` as a universal channel.
- [ ] Integrate generic Details widgets, labels, tooltips, categories, units,
  steps, precision, UI ranges, and hard authoring validation through property
  edit sessions and transactions.
- [ ] Ensure multi-object editing, fixed arrays, containers, nested structs,
  reset-to-default, undo/redo, and authored override intent retain their
  existing atomicity.
- [ ] Migrate first-party raw-key consumers covered by the vocabulary and keep
  a bounded compatibility path for unrelated extension metadata.

#### Acceptance Gate

- DHT diagnostics and runtime registration reject every invalid combination;
  exact 64-bit integer and floating-point boundary tests pass; editor proposals
  honor presentation and hard constraints without silent package-load clamping;
  existing untyped extension metadata remains readable.

### Stage 3: Add transactional reflected-schema migration

- [ ] Add generated/runtime schema identity and migration registration selected
  in Stage 0, with deterministic lookup, duplicate rejection, monotonic chains,
  cycle/gap detection, and module-lifetime ownership.
- [ ] Define a bounded detached logical migration value API that can inspect,
  claim, rename, remove, add, split, merge, and replace fields without exposing
  live object pointers or Archive storage.
- [ ] Run alias resolution and exact logical compatibility first, selected
  migrations second, post-deserialize repair third, and live assignment only
  after the complete candidate succeeds.
- [ ] Reconcile retained unknown fields and authored override paths: claimed old
  fields disappear, unclaimed unknowns remain, transformed paths target current
  schema, and ambiguous intent fails closed.
- [ ] Add class and struct fixtures for rename-only, numeric type change,
  scalar-to-vector split, two-field merge, removal with retained unknown data,
  chained migration, malformed input, missing step, duplicate path, and
  callback failure.
- [ ] Extend compatibility audit and canonical-resave reporting with migration
  usage, remaining debt, and safe retirement criteria.

#### Acceptance Gate

- Every supported historical fixture loads transactionally into the current
  schema and canonical-resaves without old claimed fields; every malformed,
  ambiguous, unavailable, or failed migration leaves the destination and
  published asset state unchanged with a stable logical diagnostic.

### Stage 4: Add weak object properties

- [ ] Add DHT recognition and diagnostics for typed `TWeakObjectPtr<T>` direct,
  fixed-array, Array, Map-value, and nested-struct forms; reject weak Map keys
  and disallowed persistence forms.
- [ ] Add the generated parameter family, property kind/layout, exact wrapper
  operations, `FWeakObjectProperty`, expected-class access, and runtime
  registration validation.
- [ ] Integrate construction, copying, comparison, canonical map-value tokens,
  snapshots, drafts, property changes, default planning, and editor inspection
  without rooting targets.
- [ ] Integrate process-local Archives and duplication so weak edges never
  enlarge discovery and remap only already-included targets; enforce the
  authored/cooked persistence policy selected in Stage 0.
- [ ] Prove GC schema exclusion for direct and nested weak values, pending-kill
  invalidation, slot-generation reuse safety, module unload safety, and
  container behavior after target retirement.
- [ ] Update garbage collection, reflection, serialization, asset-package, and
  property-editing contracts.

#### Acceptance Gate

- Direct and nested weak properties generate, edit, copy, compare, and
  duplicate according to contract; GC never retains their targets; expired or
  reused handles cannot resolve incorrectly; prohibited authored persistence
  and weak Map keys fail before mutation.

### Stage 5: Integrate, qualify, and close compatibility debt

- [ ] Adopt typed metadata in representative numeric, vector, quaternion, and
  matrix authored fields without broad unrelated annotation churn.
- [ ] Add one repository-owned asset fixture that exercises a real schema
  migration and one transient runtime type that exercises nested weak values.
- [ ] Run focused DHT, CoreDObject, AssetCore, and editor suites, then
  `fast-all`, a full `all` build, and the complete native test gate because the
  property schema and serialization substrate are shared across modules.
- [ ] Run changed documentation validation, compatibility audit, canonical
  resave checks, and generated-output determinism checks.
- [ ] Publish final contracts, record any intentionally deferred metadata keys
  or math types, and remove superseded compatibility shims only when their
  retirement gates pass.

#### Acceptance Gate

- All selected types and property forms are usable in production reflected
  headers; old supported assets either load directly or through an observable
  selected migration; weak ownership remains non-retaining; typed metadata is
  the common first-party editor path; full build/test/documentation and
  compatibility gates pass.

## Validation Matrix

| Contract | Focused validation | Required evidence |
| --- | --- | --- |
| DHT syntax and generation | DurinHeaderTool Python suite | Positive direct/container forms, source-located negative diagnostics, deterministic generated output |
| Runtime property metadata and math | `CoreObjectTests`, `CorePropertyValueSnapshotTests` | Exact kinds/sizes/identities, struct operations, defaults, snapshots, comparisons, no raw-layout persistence |
| Property transactions and editor | `CorePropertyChangeTests`, `EditorPropertyTests` | Bounds, units/presentation, multi-edit, undo/redo, reset, arrays/maps/nested structs, authored intent |
| Schema migration and packages | `AssetPackageTests` plus compatibility/canonical-resave audit | Old fixtures, chains, rollback, unknown fields, override paths, malformed and missing migration diagnostics |
| Weak references and GC | `CoreObjectTests`, `CorePropertyValueSnapshotTests`, bounded reflection-domain tests | No reachability edge, invalidation, slot reuse, copy/duplicate/remap rules, nested containers, unload safety |
| Integration | `fast-all`, full `all` build, complete native `all` test gate | No generated, serialization, editor, module, or runtime regression |
| Documentation | `.\DevTool.bat doc validate --scope changed` | Reflection, serialization, GC, asset-package, math, and editor contracts agree |

## Definition of Done

- The selected DPROPERTY syntax, runtime API, persistence identity, failure
  behavior, and ownership semantics are documented and covered by tests.
- Explicit double-vector spellings, `FQuatf`, and `FMatrix4f` compile and
  round-trip as reflected properties without changing existing canonical math
  assets.
- First-party typed metadata no longer depends on ad hoc string parsing in
  editor consumers, and invalid metadata fails during generation.
- At least two historical schema generations demonstrate deterministic chained
  migration, canonical resave, unknown-field handling, authored-intent
  reconciliation, and transactional rollback.
- Weak properties work through the selected direct/container paths, never root
  targets, never acquire soft identity, and cannot be used as Map keys.
- Compatibility audits expose remaining aliases and migrations as explicit
  debt with retirement gates.
- Focused and broad validation passes, all plan checkboxes are complete, and
  lasting behavior lives in authoritative contracts rather than only here.

## Deferred Follow-ups

- Cross-property edit conditions and conditional visibility.
- User-defined metadata schemas for plugins or project modules.
- Automated migration code generation from a reviewed declarative transform
  language; this plan starts with an explicit bounded runtime API.
- Weak references with a separately authored fallback soft path.
- Reflected float/double 2x2 and 3x3 matrices, integer vectors, and additional
  precision-specific transform types.
- Function reflection, scripting, RPC, and hot reload.

## Related Documentation

- [Generated Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Garbage Collection](../Runtime/Core/GarbageCollection.md)
- [Core Math](../Runtime/Core/Math.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)

## Related Code

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/property_parser.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/resolver/reflection_resolver.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_property_writer.py`
- `Engine/Source/Runtime/Core/Public/Math/MathFwd.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DurinPropertyTypes.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/MathStructs.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/PropertyView.cpp`
