# Reflection Metadata, Evolution, and Type Coverage Plan

Summary: Add typed property metadata, versioned reflected-schema migration, weak object properties, and explicit precision math intrinsics across generation, runtime reflection, serialization, and editor workflows.

Last reviewed: 2026-08-22

Status: Completed
Completed: 2026-08-22

## Current Status

All five implementation stages are complete. Durin now has generated
property trees,
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

The metadata inventory found three first-party runtime consumers. `DisplayName`
is selected for the typed vocabulary; `HideAlpha` and `DefaultCollapsed` remain
validated extension keys owned by their specialized widgets. Test-only `Role`
remains an unrestricted extension key, while raw `Category` remains readable
for compatibility and new first-party declarations use the typed specifier.

Two compatibility decisions are frozen.
Explicit `FVector*d` spellings canonicalize to the existing `FVector*`
persistent identities. Reflected field migration reuses DAST v4 custom versions
and version-gated `_DEPRECATED` fields rather than adding per-type schema
versions or a second general migration registry. Deprecated semantics require
an explicit `DPROPERTY(Deprecated, ...)` annotation and then require the
`_DEPRECATED` member suffix; the suffix alone has no behavior. Migration
directly reuses the existing GUID-keyed `FArchiveCustomVersion` and DAST v4
`FCustomVersion` records; it adds no name-to-GUID registry.

Stage 1 added the `FQuatf`/`FQuatd` aliases, canonical DHT mappings for explicit
double vector and quaternion spellings, and distinct `FQuatf` and `FMatrix4f`
intrinsics. The matrix schema is four accessor-backed `FVector4f` columns, and
transactional struct loading now detaches accessor-backed nested fields before
commit. DHT property tests, `CoreObjectTests`, and focused DAST v4 round-trip
coverage pass; the complete `AssetPackageTests` target is the final stage
validation receipt.

Stage 2 added source-located typed metadata parsing and canonical generated
records, runtime-owned exact numeric channels with defensive registration
validation, and generic Details presentation plus detached hard-bound
validation before transaction creation. Presentation strings remain valid on
non-edit reflected fields because existing declarations already use that
contract; numeric metadata requires `Edit`. Decimal float metadata rounds once
to its declared precision, while overflow and nonzero underflow are rejected.
Validation receipts are DHT `179 passed`, `CoreObjectTests` `76 passed`,
`CorePropertyChangeTests` `2 passed`, `CorePropertyValueSnapshotTests`
`16 passed`, `EditorPropertyTests` `29 passed`, `AssetPackageTests` `96 passed`,
and changed-document validation over three files.

Stage 3 added explicit generated and runtime deprecation descriptors, stable
GUID-keyed version bounds, automatic current-version emission, load-only class
and detached-struct routing, transactional `PostLoad`/`PostDeserialize`
conversion, and split/merge authored-intent projection. Compatibility and
canonical-resave reports now carry structured deprecated-route evidence.
Fixtures cover reused names, numeric type and unit conversion, scalar splits,
two-source merges, missing/current/future versions, incompatible signatures,
class and struct rejection, and canonical resave. Validation receipts are DHT
`184 passed`, `CoreObjectTests` `76 passed`, and `AssetPackageTests` `97 passed`.

Stage 4 added typed transient weak-object properties across direct, fixed-array,
Array, Map-value, and nested-struct forms. GC, snapshots, duplication,
process-local Archives, and Details inspection preserve non-owning semantics;
weak Map keys and durable weak persistence fail closed. Validation receipts are
DHT `192 passed`, `CoreObjectTests` `78 passed`, property snapshots `16 passed`,
`EditorPropertyTests` `30 passed`, and `AssetPackageTests` `97 passed`.

Stage 5 adopted typed metadata on production camera, collision-shape,
quaternion, transform, and matrix fields; added a checked-in legacy DAST v4
migration fixture and a production nested transient weak mutation type; and
aligned the asset-audit consumer with report Schema v3. The final qualification
passed DHT `192`, `CoreObjectTests` `78`, property snapshots `16`, property
changes `2`, `EditorPropertyTests` `30`, `AssetPackageTests` `97`, and
`WorldTests` `106`, plus `fast-all`, full `all` build, complete native `all`,
29-package compatibility audit, DAST v4 baseline, changed-document validation,
and a no-op repeat `all` build for generated-output determinism. No compatibility
shim met its evidence-based retirement gate, so existing aliases and deprecated
routes remain intentionally available.

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
- Custom-version-directed loading of historical fields into reflected C++
  members whose identifiers end in `_DEPRECATED`, followed by bounded
  `PostDeserialize` or `PostLoad` conversion into current fields.
- Deterministic historical-name/type/version matching, diagnostics, rollback,
  retained-unknown interaction, authored-intent remapping, canonical resave
  behavior, compatibility audit, and fixtures representing at least two
  historical schemas.
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

- Selected metadata is authored directly as `DisplayName="..."`,
  `ToolTip="..."`, `Category="..."`, `Units="..."`, `Step="..."`,
  `Precision=<integer>`, `ClampMin="..."`, `ClampMax="..."`,
  `UIMin="..."`, and `UIMax="..."`. A selected key may not also appear in
  `MetaData`; unrelated extension keys remain in its semicolon-delimited view.
- Presentation strings apply to reflected direct and fixed-array fields;
  numeric presentation and constraint keys additionally require `Edit`.
  Numeric keys apply to scalar integer/float fields and editable components of
  float/double vectors and quaternions. They are rejected on bool, string,
  enum, reference, container, matrix, and opaque struct fields and never flow
  from a container or containing struct into nested values.
- `Units` accepts `Unitless`, `Percent`, `Degrees`, `Radians`, `Seconds`,
  `Milliseconds`, `Meters`, `Centimeters`, `Millimeters`, or `Kilometers`.
  `Step` is finite, positive, and representable without overflow by the target
  kind. Decimal float spellings are rounded once into their declared
  float/double channel, so useful values such as `0.1` are not rejected merely
  because their binary expansion is repeating.
  `Precision` is 0..9 for float components and 0..17 for double components.

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

- The frozen route spelling is `DPROPERTY(Deprecated, CustomVersion =
  <DomainType>, DeprecatedBefore = <DomainType>::<Version>, MigratesTo =
  "CurrentA;CurrentB")`; `DeprecatedName = "OldName"` optionally overrides
  suffix stripping. `<DomainType>` exposes `static constexpr FGuid Guid` and
  `static constexpr int32 LatestVersion`. Generated C++ references those
  expressions directly, so the existing GUID custom-version identity remains
  authoritative and no parallel registry is introduced.

- `LegacyNames` remains the zero-code path for identity-preserving renames.
  `_DEPRECATED` is selected only when an old field must remain readable while
  its historical name is reused, its logical type or units change, or one or
  more current fields require explicit conversion.
- `DPROPERTY(Deprecated, ...)` is the only source of deprecated-field semantics;
  the `_DEPRECATED` suffix alone never changes reflection, loading, saving, or
  editor behavior. Once explicitly annotated `Deprecated`, the C++ member must
  end in `_DEPRECATED` or DHT rejects it. A normally reflected field whose name
  happens to end in that suffix remains ordinary when the annotation is absent.
- Deprecated fields are load-only compatibility state. They do not appear in
  Details, current saves, default-delta output, or newly authored override
  paths. Their historical serialized name defaults to the member name with the
  `_DEPRECATED` suffix removed; an explicit old name is allowed only for a
  genuine earlier spelling.
- Every deprecated route is gated by one stable custom-version GUID and an
  exclusive upper version bound. Matching uses declaring type, stored field
  name, stored logical type signature, and stored custom version. A missing
  custom-version tag means `BeforeCustomVersionWasAdded`, never `Latest`.
- Custom versions are owned by stable feature or asset domains, not the engine
  product/build version and not individual fields. Version enum values only
  increase, are never reordered or reused, and are emitted automatically on a
  current save when the owning reflected schema declares that domain.
- Historical routing runs before ordinary exact-current-name matching, but
  only when its version range and old type signature match. This permits old
  `A` to load into `A_DEPRECATED` while current `A` remains the canonical field.
  Duplicate, overlapping, ambiguous, or type-incompatible routes fail closed.
- Struct conversion runs through bounded `PostDeserialize` on detached storage.
  Class conversion runs before package publication through `PostLoad` or an
  equivalently transactional reflected migration hook. A failure publishes no
  package graph and no partial canonical resave.
- A successful conversion claims the historical field so it is not retained as
  unknown data. Unclaimed unknown fields retain their existing behavior. Save
  emits only current field names/types and the current custom-version value.
- Migration declares which current authored paths inherit explicit/forced
  intent from each consumed historical path. One-to-one, split, and merge
  mappings are validated before ledger publication; ambiguous or incomplete
  intent mappings fail closed rather than silently changing default-delta
  behavior.
- `MigratesTo` names current, non-deprecated properties on the same reflected
  type. A split propagates historical provenance to every target. A merge
  combines contributing provenance with `Forced` above `Explicit` above
  absent. Missing targets and incomplete or ambiguous mappings fail before
  ledger publication.
- Removal of a `_DEPRECATED` field or route follows the same authored-content
  baseline, canonical-resave, and compatibility-policy gates as removal of a
  legacy reflection alias.

### Weak references

- DHT accepts `TWeakObjectPtr<T>` only with `Transient`. Discovery ignores weak
  edges; duplication remaps a live target only when it is already mapped by
  structural or hard reachability, otherwise it writes null. Retired handles
  compare as null. Weak Map keys are rejected; direct, fixed-array, Array,
  Map-value, and nested-struct values share these rules.

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
- Explicit `FVector*d` spellings use double components, are accepted by DHT,
  and resolve to the existing canonical `FVector*` descriptors and persistent
  identities. Changing only the source spelling never creates schema churn.
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

- [x] Inventory every current metadata key and consumer; classify each as a
  selected typed key, retained extension key, deprecated key, or invalid key.
- [x] Freeze DPROPERTY syntax and exact applicability rules for display name,
  tooltip, category, units, step, precision, hard bounds, and UI bounds.
- [x] Canonicalize explicit `FVector*d` spellings to existing `FVector*`
  descriptors and persistent identities.
- [x] Reuse DAST v4 custom versions plus version-gated `_DEPRECATED` reflected
  fields; do not add per-type schema versions or a general migration registry.
- [x] Require explicit `DPROPERTY(Deprecated, ...)` annotation plus the
  `_DEPRECATED` suffix; suffix-only reflected fields remain ordinary.
- [x] Freeze deprecated route-version syntax, domain-version registration,
  automatic current-version emission, and missing-version behavior.
- [x] Freeze one-to-one, split, and merge authored-intent remapping from
  consumed historical paths to current paths.
- [x] Freeze weak-reference behavior by Archive purpose, including DHT's
  persistability rule, expired-handle comparison, and duplication remapping.
- [x] Assign every selected unsupported case to positive and negative fixtures
  in its owning implementation stage; committed expected-failure tests are not
  used as a stage gate.


#### Acceptance Gate

- The contracts above have no unresolved syntax or persistence decision; each
  implementation stage owns positive and negative fixtures for its selected
  cases, and the plan and owning runtime documents agree.

### Stage 1: Complete precision-specific math intrinsics

- [x] Add the selected `FQuatf`/optional `FQuatd` aliases at the Core math
  boundary without changing existing aliases or ABI.
- [x] Teach DHT to resolve explicit `FVector2d/3d/4d`, `FQuatf`, and
  `FMatrix4f` source spellings according to the Stage 0 identity policy.
- [x] Register intrinsic descriptors, float component properties, deterministic
  defaults, matrix column accessors, and complete reflected struct operations.
- [x] Cover direct/fixed-array/container declarations, snapshots, default
  comparison, Archive and DAST v4 round trips, and generic Details editing.
- [x] Update the math and reflection contracts with canonical identity and
  component/column ordering.

#### Acceptance Gate

- Every selected math spelling generates and compiles as a `DPROPERTY`, reports
  the intended component kinds and sizes, round-trips without raw bytes, and
  preserves existing double-precision asset fixtures byte-for-byte where the
  schema is unchanged.

### Stage 2: Publish typed property metadata

- [x] Add typed DHT model fields and canonical generated parameter records for
  the selected metadata vocabulary while preserving required extension
  metadata compatibility.
- [x] Reject malformed, duplicate, inapplicable, non-finite, out-of-order, or
  non-representable metadata with source-located diagnostics.
- [x] Publish immutable runtime typed metadata and exact numeric conversion and
  validation helpers without using `double` as a universal channel.
- [x] Integrate generic Details widgets, labels, tooltips, categories, units,
  steps, precision, UI ranges, and hard authoring validation through property
  edit sessions and transactions.
- [x] Ensure multi-object editing, fixed arrays, containers, nested structs,
  reset-to-default, undo/redo, and authored override intent retain their
  existing atomicity.
- [x] Migrate first-party raw-key consumers covered by the vocabulary and keep
  a bounded compatibility path for unrelated extension metadata.

#### Acceptance Gate

- DHT diagnostics and runtime registration reject every invalid combination;
  exact 64-bit integer and floating-point boundary tests pass; editor proposals
  honor presentation and hard constraints without silent package-load clamping;
  existing untyped extension metadata remains readable.

### Stage 3: Add custom-versioned `_DEPRECATED` field migration

- [x] Add DHT/model/generated/runtime descriptors for explicitly annotated
  `DPROPERTY(Deprecated, ...)` load-only fields and version routes, including
  required suffix validation, derived/explicit historical names, domain GUID
  resolution, and exclusive version bounds; suffix-only fields stay ordinary.
- [x] Ensure saves declare and emit the current domain Custom Version, while a
  missing load tag resolves to `BeforeCustomVersionWasAdded` and newer unknown
  versions fail according to the existing package version policy.
- [x] Route historical fields by declaring type, stored name, logical type
  signature, and custom-version range before current-name matching; reject
  duplicate, overlapping, ambiguous, and incompatible routes.
- [x] Exclude deprecated fields from Details, current saves, default deltas,
  current authored ledgers, and canonical schema output while allowing bounded
  detached struct loading and pre-publication class conversion.
- [x] Claim consumed historical fields, preserve unclaimed unknown values, and
  transactionally remap explicit/forced authored intent to declared one-to-one,
  split, or merge current paths.
- [x] Add class and struct fixtures for name reuse, numeric type change,
  unit-only conversion, scalar-to-vector split, two-field merge, missing custom
  version, current version, overlapping routes, incompatible signature,
  conversion failure, and canonical resave.
- [x] Extend compatibility audit and canonical-resave reporting with deprecated
  route usage, remaining debt, and safe field/route retirement criteria.

#### Acceptance Gate

- Every supported historical fixture loads through the selected
  `_DEPRECATED` route, converts transactionally, remaps authored intent, and
  canonical-resaves without old claimed fields; malformed, ambiguous,
  unavailable, or failed routes leave published asset state unchanged with a
  stable logical diagnostic.

### Stage 4: Add weak object properties

- [x] Add DHT recognition and diagnostics for typed `TWeakObjectPtr<T>` direct,
  fixed-array, Array, Map-value, and nested-struct forms; reject weak Map keys
  and disallowed persistence forms.
- [x] Add the generated parameter family, property kind/layout, exact wrapper
  operations, `FWeakObjectProperty`, expected-class access, and runtime
  registration validation.
- [x] Integrate construction, copying, comparison, canonical map-value tokens,
  snapshots, drafts, property changes, default planning, and editor inspection
  without rooting targets.
- [x] Integrate process-local Archives and duplication so weak edges never
  enlarge discovery and remap only already-included targets; enforce the
  authored/cooked persistence policy selected in Stage 0.
- [x] Prove GC schema exclusion for direct and nested weak values, pending-kill
  invalidation, slot-generation reuse safety, module unload safety, and
  container behavior after target retirement.
- [x] Update garbage collection, reflection, serialization, asset-package, and
  property-editing contracts.

#### Acceptance Gate

- Direct and nested weak properties generate, edit, copy, compare, and
  duplicate according to contract; GC never retains their targets; expired or
  reused handles cannot resolve incorrectly; prohibited authored persistence
  and weak Map keys fail before mutation.

### Stage 5: Integrate, qualify, and close compatibility debt

- [x] Adopt typed metadata in representative numeric, vector, quaternion, and
  matrix authored fields without broad unrelated annotation churn.
- [x] Add one repository-owned asset fixture that exercises a real schema
  migration and one transient runtime type that exercises nested weak values.
- [x] Run focused DHT, CoreDObject, AssetCore, and editor suites, then
  `fast-all`, a full `all` build, and the complete native test gate because the
  property schema and serialization substrate are shared across modules.
- [x] Run changed documentation validation, compatibility audit, canonical
  resave checks, and generated-output determinism checks.
- [x] Publish final contracts, record any intentionally deferred metadata keys
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
| Schema migration and packages | `AssetPackageTests` plus compatibility/canonical-resave audit | Custom-version ranges, `_DEPRECATED` name reuse, rollback, unknown fields, override paths, canonical resave, malformed and ambiguous route diagnostics |
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
- At least two historical Custom Version generations demonstrate deterministic
  `_DEPRECATED` routing, name reuse, split/merge conversion, canonical resave,
  unknown-field handling, authored-intent reconciliation, and transactional
  rollback.
- Weak properties work through the selected direct/container paths, never root
  targets, never acquire soft identity, and cannot be used as Map keys.
- Compatibility audits expose remaining aliases and migrations as explicit
  debt with retirement gates.
- Focused and broad validation passes, all plan checkboxes are complete, and
  lasting behavior lives in authoritative contracts rather than only here.

## Deferred Follow-ups

- Cross-property edit conditions and conditional visibility.
- User-defined metadata schemas for plugins or project modules.
- A general detached logical-tree migration registry or declarative transform
  language beyond Custom Version, `_DEPRECATED`, and post-load conversion.
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
