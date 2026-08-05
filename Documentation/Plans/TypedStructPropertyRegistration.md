# Typed Struct Property Registration Plan

Summary: Replace the positional FStructPropertyParams alias with concise, type-specific registration metadata that delegates struct value semantics to DStruct operations.

Last reviewed: 2026-08-05

Status: Active
Completed:

## Current Status

- `FStructPropertyParams` is currently an alias of `FPropertyParamsBase`, so a
  generated struct-property initializer supplies the fields for every property
  kind as one long positional aggregate with many irrelevant null entries.
- DHT emits `sizeof(T)`, `alignof(T)`,
  `InitializePropertyValue<T>`, and `DestroyPropertyValue<T>` for every
  property, including properties whose value type is a reflected struct.
- Those property-level template instantiations independently require a usable
  constructor and destructor even after struct registration stops generating
  unconditional lifecycle functions.
- `ConstructGeneratedProperty` switches on `EPropertyGenFlags` but reads every
  kind-specific input from the common parameter record. `FStructProperty`
  stores the referenced `DStruct`, while its detached-value lifecycle is still
  populated through the generic `FProperty` callback slots.
- Handwritten intrinsic math descriptors use the same common positional
  aggregate and therefore do not have a distinct struct-property registration
  contract.
- This plan is ordered after
  [Reflected Struct Operations](ReflectedStructOperations.md). Implementation
  must not begin until that plan is completed and its `FDStructOps` and managed
  struct-storage contracts are available as the baseline.
- No implementation stage has started.

## Goal

Give reflected struct properties a small, type-specific registration contract
that describes schema and member access without duplicating the referenced
struct's storage or lifecycle semantics.

Generated metadata for a struct property must not instantiate that value
type's constructor or destructor. Runtime construction, destruction, copying,
alignment, equality, reference collection, and serialization must continue
through the referenced `DStruct` and its established operations and reflection
schema.

## Scope

- Replace the `using FStructPropertyParams = FPropertyParamsBase` alias with a
  real struct-property parameter type and a concise construction API.
- Separate struct-only registration data from irrelevant class, enum,
  container, and generic lifecycle fields.
- Derive struct storage size, alignment, and supported value operations from
  the referenced `DStruct` instead of generated `T` lifecycle thunks.
- Update `ConstructGeneratedProperty` and `FStructProperty` to consume and
  validate the typed parameter contract.
- Update DHT emission for direct struct members, fixed C++ arrays of structs,
  and struct descriptors nested beneath array and map properties.
- Migrate handwritten CoreDObject intrinsic descriptors to the same typed
  struct-property path.
- Preserve property metadata, direct-offset access, explicit accessor-based
  access, GC schema traversal, Archive traversal, and DAST v2 behavior.
- Add focused generation, registration, lifecycle-capability, nested-container,
  serialization, and GC tests plus lasting reflection documentation.

## Non-Goals

- Defining `FDStructOps`, struct traits, managed struct storage, logical
  equality, custom Archive serialization, or post-deserialize behavior. Those
  belong to the prerequisite Reflected Struct Operations plan.
- Replacing the parameter aliases for numeric, string, name, GUID, enum,
  object, array, or map properties. Their migration may follow after the struct
  path demonstrates the typed-registration pattern.
- Replacing the runtime `FProperty` class hierarchy, removing
  `EPropertyGenFlags`, or introducing a general reflection metadata ABI.
- Changing reflected names, offsets, array dimensions, member accessor
  behavior, property flags, metadata, or qualified type resolution.
- Changing transient object-graph Archive bytes, authored DAST v2 bytes, or
  defining DAST v3 codecs.
- Replacing GLM-backed math types or changing intrinsic math descriptors'
  logical component schemas.
- Optimizing generated binary size without measurement. Source readability and
  removal of invalid template instantiation are required; binary-size changes
  are measured evidence, not an assumed outcome.

## Design Decisions and Invariants

### Dependency and Ownership Boundary

- This plan consumes the completed `FDStructOps` and managed struct-storage
  contracts without redefining or extending them.
- `DStruct` is the single source of truth for a struct value's size, alignment,
  lifecycle capabilities, logical equality, hidden references, and optional
  serialization operations.
- `FStructProperty` owns only the relationship between an owning reflected
  type, a member location or accessor, and the referenced `DStruct` schema.
- A struct property never publishes a second constructor, destructor, copy, or
  serializer for the same value type.

### Typed Parameter Contract

- `FStructPropertyParams` becomes a distinct parameter type rather than an
  alias. Its public construction API accepts only common field schema plus the
  referenced-struct resolver and optional metadata/accessors.
- The construction API fixes `EPropertyGenFlags::Struct` internally. Generated
  code does not repeat the kind or populate irrelevant class, enum, array, map,
  object-wrapper, or generic value-lifecycle slots.
- Generated initialization uses a named constructor or factory with a stable
  semantic argument order; it does not expose the physical layout of
  `FPropertyParamsBase` as a positional ABI. Stage 0 freezes the exact API name
  and representation after the prerequisite baseline is available.
- Existing class and struct property arrays may continue to hold base parameter
  pointers, but runtime dispatch must type-check the `Struct` tag before reading
  `FStructPropertyParams` fields.
- Direct members retain offsets. Externally described or otherwise non-offset
  fields retain explicit mutable and const accessors. The typed contract does
  not assume standard layout or public data members.

### Storage and Lifecycle

- DHT does not emit `InitializePropertyValue<T>` or
  `DestroyPropertyValue<T>` for a struct property, including nested array/map
  descriptors and fixed arrays.
- The runtime derives detached-value size and alignment from the resolved
  `DStruct`. Any duplicate element-size field required temporarily for owner
  layout is validated against the descriptor and is not treated as an
  independent semantic fact.
- Generic temporary or detached struct values use the managed storage supplied
  by the prerequisite plan and check the exact `FDStructOps` capability before
  construction, copying, assignment, or destruction.
- Registration itself succeeds when a referenced struct lacks default
  construction or copy capability. A failure occurs only when a consumer asks
  for an unavailable operation.
- A missing resolver, null resolved descriptor, kind/type mismatch, unsupported
  size range, or incompatible duplicate layout fact fails registration with a
  stable diagnostic before the property becomes visible.

### Generated Source Contract

- For an ordinary direct struct member, generated registration names the field,
  flags, array dimension, owner-relative location, and referenced-struct
  resolver, plus metadata only when present.
- The initializer contains no placeholder entries for unrelated property kinds
  and no direct lifecycle-template address for the member type.
- Nested struct descriptors beneath arrays and maps use the same typed
  parameter object as direct members; there is no anonymous fallback record
  with different lifecycle behavior.
- Qualified type resolution and the referenced `Z_Construct_DStruct_*` helper
  remain stable, so the change is registration-source and runtime metadata ABI,
  not reflection identity.
- DHT output remains deterministic and covered by exact focused assertions that
  check both required tokens and forbidden lifecycle/template tokens.

### Compatibility and Rollout

- Runtime metadata is rebuilt atomically with generated code; mixed old/new
  parameter layouts within one binary are unsupported.
- No serialized representation is derived from the physical layout of
  `FStructPropertyParams`, so the parameter change carries no package-format
  version.
- Field traversal, unknown-field handling, object-reference discovery, and
  property ordering remain unchanged.
- The legacy struct-property alias and fallback parsing path are removed only
  after generated and handwritten registrations use the typed form.
- Other property kinds retain their current parameter contracts during this
  plan, even if the typed struct result suggests a later generalization.

## Current Foundations and Gaps

### Foundations

- DHT centralizes property parameter emission in one writer function and
  already distinguishes `Struct` from other property kinds.
- Generated owner descriptors already hold pointers to property parameter
  records, allowing a concrete struct-parameter object to participate through
  a common registration entry.
- `ConstructGeneratedProperty` already resolves `ReferencedStructFunc` before
  constructing `FStructProperty`.
- `FStructProperty` already stores the resolved `DStruct` used by Archive and GC
  field traversal.
- The prerequisite plan establishes capability-aware `DStruct` operations and
  managed storage needed to replace property-local lifecycle thunks.

### Gaps

- The alias exposes one large positional layout and cannot enforce which fields
  are meaningful for a struct property.
- Every generated property repeats size, alignment, construction, and
  destruction templates even when a referenced struct descriptor owns those
  facts.
- A reflected struct with unavailable default construction can still fail to
  compile when used as a property because DHT instantiates the generic property
  initializer template.
- Runtime registration does not validate that struct element size and
  alignment agree with the referenced descriptor.
- Direct, fixed-array, nested-container, and handwritten intrinsic struct
  properties lack one tested registration contract.
- Positional aggregate changes create broad generated-source churn and poor
  diagnostics when fields are inserted or reordered.

## Implementation Stages

### Stage 0: Freeze the Post-StructOps Property Contract

- [ ] Verify the completed Reflected Struct Operations baseline, lasting
  documentation, `FDStructOps` access API, managed storage owner, and removal of
  legacy lifecycle callbacks before starting implementation.
- [ ] Inventory all generated and handwritten struct-property registrations,
  covering direct members, fixed arrays, nested arrays, map keys, map values,
  `FTransform`, and externally registered intrinsic descriptors.
- [ ] Inventory every consumer of `FProperty::GetValueSize`,
  `GetValueAlignment`, `HasValueLifecycle`, `InitializeValue`, and
  `DestroyValue` and classify struct versus non-struct requirements.
- [ ] Freeze the concrete `FStructPropertyParams` constructor/factory shape,
  common-base relationship, tag validation, metadata/accessor inputs, and
  registration lifetime.
- [ ] Decide whether the transitional common base retains duplicate struct
  size fields for owner-layout compatibility; if retained, define exact
  descriptor validation and removal ownership.
- [ ] Define stable registration diagnostics for missing descriptors, wrong
  kinds, layout disagreement, unsupported size/alignment, and unavailable
  requested value operations.
- [ ] Record representative generated source before implementation so source
  contraction and forbidden template removal can be verified objectively.

#### Acceptance Gate

- The prerequisite plan is completed and no design in this plan depends on a
  legacy three-callback struct lifecycle path.
- Every struct-property form and every value-lifecycle consumer has one recorded
  migration route.
- The typed parameter API has one selected representation with explicit
  ownership, tag, lifetime, and failure contracts.
- The baseline example and forbidden generated constructs are recorded for DHT
  regression tests.

### Stage 1: Add Typed Runtime Registration

- [ ] Introduce the concrete `FStructPropertyParams` type and concise
  constructor/factory while preserving the common registration entry required
  by owner property arrays.
- [ ] Move the referenced-struct resolver and other struct-only inputs behind
  the typed contract.
- [ ] Update `ConstructGeneratedProperty` to validate the `Struct` tag, read the
  typed parameters, resolve the descriptor once, and reject invalid metadata
  before publishing the property.
- [ ] Update `FStructProperty` and generic struct-value storage to obtain size,
  alignment, and lifecycle capabilities from `DStruct`/`FDStructOps`.
- [ ] Prevent registration from requiring unavailable construction or copy
  operations while preserving deterministic errors when consumers request
  them.
- [ ] Add focused CoreDObject tests for valid registration, null/mismatched
  descriptors, descriptor layout checks, direct/accessor fields, fixed arrays,
  and unavailable lifecycle capabilities.

#### Acceptance Gate

- Runtime registration accepts only a valid typed struct-parameter record and
  publishes one resolved `DStruct` relationship.
- No struct property stores or invokes a lifecycle operation that competes with
  its `FDStructOps`.
- A property referencing a non-default-constructible or non-copyable struct can
  register, and unsupported use fails without changing destination storage.
- Existing non-struct property registration remains behaviorally unchanged.

### Stage 2: Emit Concise Generated Struct Metadata

- [ ] Update DHT to instantiate `FStructPropertyParams` through the concise API
  for direct, fixed-array, array-inner, map-key, and map-value struct
  descriptors.
- [ ] Stop emitting struct-property `sizeof(T)`, `alignof(T)`,
  `InitializePropertyValue<T>`, and `DestroyPropertyValue<T>` when those facts
  are provided by the referenced descriptor.
- [ ] Stop emitting unrelated class, enum, container, wrapper, and generic
  lifecycle placeholders in struct-property initializers.
- [ ] Preserve field name, flags, dimensions, offsets or accessors, metadata,
  and referenced qualified struct identity.
- [ ] Add generation fixtures for ordinary, deleted-default, deleted-copy,
  private/nontrivial-destructor where representable, fixed-array, and nested
  container struct properties.
- [ ] Add exact negative assertions proving generated struct metadata contains
  no direct constructor/destructor template addresses.
- [ ] Regenerate representative engine reflection output and compare property
  ordering and identities against the Stage 0 baseline.

#### Acceptance Gate

- The representative `FStructPropertyParams` initializer contains only schema,
  member-location/accessor, metadata, and referenced-struct inputs, with no
  irrelevant null placeholders.
- A reflected struct with an unavailable default or copy constructor compiles
  when used as a reflected property and no consumer requests that operation.
- All direct and nested struct-property forms resolve the same descriptors and
  preserve deterministic generated output.
- Generated non-struct property metadata is unchanged except for unavoidable
  declarations required by the common typed-registration entry.

### Stage 3: Migrate Intrinsics and Qualify Consumers

- [ ] Convert handwritten CoreDObject math and other intrinsic struct-property
  descriptors to the typed parameter API.
- [ ] Verify `FVector3` direct fields and its use inside `FTransform`, arrays,
  and maps resolve storage and operations from the intrinsic `DStruct` rather
  than C++ layout thunks.
- [ ] Run property snapshot, duplication, Archive object-graph, AssetCore DAST
  v2, and GC tests for direct and nested struct properties.
- [ ] Add compatibility assertions for property names, flags, offsets/accessor
  behavior, array dimensions, reflected field order, and authored bytes.
- [ ] Remove the `FStructPropertyParams` alias and any transitional
  struct-property lifecycle fallback after all generated and handwritten sites
  migrate.
- [ ] Measure representative generated source size and compiled metadata size;
  record the results without making binary reduction an acceptance condition.

#### Acceptance Gate

- Generated and intrinsic registrations use one typed struct-property contract
  with no legacy alias or direct value-type lifecycle thunk.
- Snapshot, duplication, serialization, and GC consumers obtain identical
  logical behavior through the referenced descriptor and capability-aware
  storage.
- Existing supported DAST v2 fixtures and transient object graphs retain their
  successful behavior and incompatibility categories.
- The recorded source comparison demonstrates removal of irrelevant positional
  arguments and identifies any remaining unavoidable common fields.

### Stage 4: Document and Complete Cross-Module Validation

- [ ] Update Reflection System documentation with the lasting distinction
  between property schema registration and referenced-struct value operations.
- [ ] Document the typed struct-parameter authoring path for DHT and external
  intrinsic descriptors without copying active-plan details into runtime docs.
- [ ] Run focused DHT generation and CoreDObject property tests plus relevant
  AssetCore, GC, snapshot, and duplication suites under the documented Agent
  Build Profile.
- [ ] Complete a successful full `all` build because generated metadata and the
  CoreDObject registration ABI change together.
- [ ] Record the final baseline, working set, symbols, decisions, validation,
  generated-source comparison, and any proposed all-property generalization in
  the stage handoff.

#### Acceptance Gate

- Lasting documentation owns the implemented typed struct-property contract
  and clearly identifies `DStruct` as the value-semantics authority.
- Focused suites and the full build pass from one coherent generated-code
  baseline.
- No generated or handwritten struct-property registration can accidentally
  reintroduce unconditional value-type construction or destruction.
- Any broader typed-property migration is separately justified and does not
  remain as hidden unfinished scope in this plan.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Dependency | Completed Reflected Struct Operations baseline and documented `FDStructOps`/managed-storage API |
| Parameter API | Concrete typed record, concise construction, tag validation, stable lifetime, and no irrelevant inputs |
| DHT generation | Direct, fixed-array, array-inner, map-key, and map-value structs emit no property-local lifecycle thunks |
| Compile capability | Deleted-default and deleted-copy struct property fixtures register without unrequested template instantiation |
| Registration failures | Missing or mismatched descriptors and invalid layout facts fail before publication with stable diagnostics |
| Intrinsic bridge | `FVector3` and `FTransform` use the same typed property contract without Core depending on CoreDObject |
| Lifecycle | Detached and temporary values use `FDStructOps` and managed storage with unchanged destinations on failure |
| Reflection behavior | Names, flags, dimensions, offsets/accessors, metadata, ordering, and qualified identities remain stable |
| Serialization and GC | Object graph, DAST v2, snapshots, duplication, hidden references, arrays, and maps retain logical behavior |
| Integration | DHT regeneration, focused suites, generated-source comparison, and full `all` build |

Build and test execution follows [Build and Run](../Development/Build/BuildAndRun.md)
and [Native Tests](../Development/Build/NativeTests.md).

## Definition of Done

- `FStructPropertyParams` is a concrete, type-specific registration contract
  rather than an alias of the all-purpose positional parameter record.
- Generated struct-property initializers contain only relevant schema,
  location/accessor, metadata, and referenced-descriptor inputs.
- DHT never instantiates a struct member type's construction or destruction
  template merely because that type appears as a reflected property.
- `FStructProperty` derives storage and supported operations from the referenced
  `DStruct` and its immutable `FDStructOps`.
- Direct, fixed-array, nested-container, and intrinsic registrations share one
  validated path.
- Reflection identities, GC traversal, snapshots, object graphs, and DAST v2
  authored behavior remain compatible.
- Focused tests and a full build pass, lasting reflection documentation is
  updated, and the legacy struct-property alias/fallback is removed.

## Deferred Follow-ups

- Concrete typed parameter records for enum, object, array, map, scalar, and
  other property kinds.
- Replacing the central kind switch with a registration visitor or per-kind
  factory after typed records exist for enough property families.
- Compact generated metadata tables or binary-size optimization based on
  measured cost.
- Wider offset elimination through generated member accessors where ABI or
  non-standard-layout types demonstrate a need.

## Related Documentation

- [Reflected Struct Operations](ReflectedStructOperations.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Garbage Collection](../Runtime/Core/GarbageCollection.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_generation.py`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/DObjectGlobals.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Property.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DurinPropertyTypes.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/MathStructs.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/GCReferenceSchema.cpp`
- `Engine/Tests/Native/CoreDObjectTests/`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
