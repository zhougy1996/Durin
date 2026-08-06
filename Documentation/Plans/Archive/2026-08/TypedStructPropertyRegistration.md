# Typed Struct Property Registration Plan

Summary: Replace the positional FStructPropertyParams alias with concise, type-specific registration metadata that delegates struct value semantics to DStruct operations.

Last reviewed: 2026-08-06

Status: Archived
Completed: 2026-08-06

## Current Status

- Stage 0 completed on 2026-08-05 and Stages 1 through 4 completed on
  2026-08-06. Their intermediate stage commits were intentionally squashed into
  one implementation commit and rebased onto `dev` baseline `0321326e`
  (`refactor(renderer): separate static mesh base pass lighting`).
- `FStructPropertyParams` is now a concrete final record with concise offset
  construction and `WithAccessors`; its constructors fix the struct kind and
  layout while the defaulted base layout preserves every non-struct aggregate.
- Runtime validates the layout/kind pair before casting, resolves only the
  derived struct resolver inside the struct arm, rejects invalid descriptor,
  accessor, and metadata facts with the frozen stable codes, and never applies
  generic lifecycle slots to a struct property.
- `FStructProperty` now fixes its kind and derives its stride from the resolved
  descriptor. Struct construction, destruction, copying, alignment, and
  capability failures continue through `DStruct`/`FDStructOps` and the existing
  managed-storage facade.
- DHT emits the concise typed constructor for all struct descriptors and exits
  the struct branch before assembling generic layout, placeholder, or lifecycle
  inputs. Exact fixtures now cover direct, unavailable-operation,
  nontrivial-destructor, metadata, fixed-array, Array-inner, Map-key, and
  Map-value forms.
- A full Editor build regenerated the 44 inventoried production records: 36
  direct and eight Array-inner records, with zero identity, resolver, ordering,
  or forbidden-token audit failures. Production reflection declarations remain
  unchanged from the Stage 0 baseline.
- The intrinsic `FTransform` records, all nine handwritten CoreDObject and
  AssetCore registration records, and direct `FStructProperty` consumers use
  the descriptor-backed ABI. No runtime read of the legacy base struct-resolver
  slot remains.
- Intrinsic, snapshot, duplication, Archive object-graph, DAST v2, and GC
  qualification passed with explicit compatibility assertions for property
  identity, flags, dimensions, offsets, descriptor links, ordering, and exact
  authored floating-point bits.
- The representative generated initializer contracted from 535 to 159 bytes
  while the compiled typed record grew from 160 to 168 bytes because the
  retained legacy base resolver and the derived typed resolver coexist during
  the non-struct ABI transition.
- Reflection System documentation now owns the schema-registration versus
  referenced-struct value-semantics boundary and the DHT/external-intrinsic
  typed authoring path. Final DHT, CoreObject, AssetPackage, duplication, and
  full-build validation passed from one generated-code baseline, so the plan is
  complete and awaits normal monthly archival.

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
  continued absence of invalid template instantiation are required; binary-size
  changes are measured evidence, not an assumed outcome.

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

- `FStructPropertyParams` is a `final` concrete type derived from
  `FPropertyParamsBase` so existing class/struct property arrays retain
  `const FPropertyParamsBase*` entries. The derived record owns a
  `DStruct* (*)()` resolver; the legacy base resolver slot remains physically
  present for other positional records during this bounded migration but is
  never read for a typed struct record.
- `EPropertyParamLayout` has `Legacy` and `Struct` values. A defaulted
  `Layout = Legacy` field is appended to `FPropertyParamsBase`, preserving all
  existing positional non-struct initializers, while each typed constructor
  sets `Layout = Struct`. Runtime validates `Layout` together with `Kind`
  before casting, so a legacy base record tagged as `Struct` cannot be read as
  a derived object.
- The direct-member constructor is `constexpr`:
  `FStructPropertyParams(NameUTF8, Flags, ArrayDim, Offset, StructResolver,
  MetaData = nullptr, NumMetaData = 0)`. It fixes
  `EPropertyGenFlags::Struct`, zero-initializes all irrelevant base slots, and
  accepts no element size, alignment, class/enum/container pointer, wrapper
  flag, or lifecycle callback.
- The `static constexpr` factory
  `FStructPropertyParams::WithAccessors(NameUTF8, Flags, ArrayDim,
  StructResolver, MutableAccessor, ConstAccessor, MetaData = nullptr,
  NumMetaData = 0)` is the only accessor-based factory. Both accessors must be
  present; the factory fixes the stored offset to zero. Nested Array/Map
  descriptors use the direct constructor with offset zero and no accessors.
- Owner property arrays upcast typed records to the common base. Runtime
  dispatch reads only common schema, `Layout`, and `Kind` before casting to
  `FStructPropertyParams`; it resolves and validates the derived resolver only
  inside the `Struct` switch arm.
- Parameter records, resolver functions, metadata arrays, and accessor
  functions have program lifetime. A property copies names and metadata into
  runtime-owned forms during registration and stores only the resolved
  program-lifetime `DStruct` relationship.

### Storage and Lifecycle

- DHT continues not to emit `InitializePropertyValue<T>` or
  `DestroyPropertyValue<T>` for a struct property, including nested array/map
  descriptors and fixed arrays.
- The typed record carries no duplicate element size or alignment. After
  resolving the descriptor, runtime registration requires
  `1 <= DStruct::PropertiesSize <= std::numeric_limits<uint16>::max()`, requires
  a nonzero power-of-two `DStruct::MinAlignment`, and initializes
  `FProperty::ElementSize` from the descriptor. Direct and fixed-array indexing
  therefore uses the descriptor size as its stride; detached storage continues
  using the descriptor's full size and alignment.
- Generic temporary or detached struct values use the managed storage supplied
  by the prerequisite plan and check the exact `FDStructOps` capability before
  construction, copying, assignment, or destruction.
- Registration itself succeeds when a referenced struct lacks default
  construction or copy capability. A failure occurs only when a consumer asks
  for an unavailable operation.
- Generated metadata is trusted program data, so invalid registration is a
  fail-fast `checkf` before the property is linked into its owner. Diagnostics
  use the stable codes `StructPropertyRegistration.KindMismatch`,
  `.MissingResolver`, `.NullDescriptor`, `.InvalidSize`, `.InvalidAlignment`,
  `.AccessorPairMismatch`, and `.MetadataMismatch`, followed by the owner and
  property names. Unsupported value operations retain the prerequisite's
  existing consumer-time capability diagnostics and do not fail registration.

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
- The legacy struct-property alias and base `ReferencedStructFunc` fallback are
  removed only after generated and handwritten registrations use the typed
  form. Removing the now-unused physical base slot is deferred to a broader
  typed-property migration because changing every positional non-struct record
  is outside this plan.
- Other property kinds retain their current parameter contracts during this
  plan, even if the typed struct result suggests a later generalization.

## Current Foundations and Gaps

### Foundations

- DHT centralizes property parameter emission in one writer function and
  already distinguishes `Struct` from other property kinds.
- Generated owner descriptors already hold pointers to property parameter
  records, allowing a concrete struct-parameter object to participate through
  a common registration entry.
- `ConstructGeneratedProperty` already resolves a referenced descriptor before
  constructing `FStructProperty`; the typed path can narrow that resolution to
  the `Struct` switch arm without changing owner registration order.
- `FStructProperty` already stores the resolved `DStruct` used by Archive and GC
  field traversal.
- The prerequisite plan already established capability-aware `DStruct`
  operations, managed storage, transactional decode, and struct-property
  delegation for size, alignment, lifecycle, copying, equality, Archive, and
  GC.

### Gaps

- The alias exposes one large positional layout and cannot enforce which fields
  are meaningful for a struct property.
- Generated struct records still repeat size and alignment expressions and all
  irrelevant positional placeholders even though their construction and
  destruction callbacks are already null.
- Runtime registration reads unrelated common-record fields before dispatch,
  accepts a missing/null struct resolver, and applies generic size/alignment
  slots after constructing a struct property.
- Direct, fixed-array, nested-container, and handwritten intrinsic struct
  properties lack one tested registration contract.
- Positional aggregate changes create broad generated-source churn and poor
  diagnostics when fields are inserted or reordered.

## Stage 0 Frozen Contract and Audit

### Registration Inventory

| Form | Current production inventory | Migration route |
| --- | --- | --- |
| Generated direct struct member | 36 declarations across AssetImportCore and Engine, including source paths, import payloads, camera settings, colors, transforms, spline/material/static-mesh/texture records, and cooked-payload descriptors | Emit the direct typed constructor with the existing name, flags, dimension, offset, metadata, and resolved helper |
| Generated fixed C++ array | None | Add a focused DHT fixture; preserve `ArrayDim` and use descriptor size as the runtime stride |
| Generated Array inner struct | Eight: four import-record vectors, material definitions, material overrides, spline points, and static-mesh material slots | Emit the same direct typed constructor with offset zero beneath the existing Array record |
| Generated Map key/value struct | None | Preserve the existing recursive writer path and add both key and value fixtures; Map ownership remains outside this plan |
| Handwritten production struct property | Three direct fields in intrinsic `FTransform`: `Rotation`, `Translation`, and `Scale3D` | Replace the common positional aggregates with the direct typed constructor |
| Handwritten test metadata | Nine records in CoreDObject and AssetCore tests covering direct, Array-inner, Map-value, Archive, package, and GC behavior | Migrate to the typed constructor and add accessor/failure coverage rather than retaining a test-only fallback |

The six intrinsic math descriptors remain external registrations. `FVector2`,
`FVector3`, `FVector4`, `FQuat`, and `FLinearColor` contain only scalar
properties; only `FTransform` contributes handwritten struct-property records.
The current DHT fixture covers direct intrinsic structs, an Array inner with a
deleted default constructor, and a Map value with deleted copy operations. It
does not yet cover fixed arrays, struct Map keys, or metadata on a struct
property; those are explicit Stage 2 fixtures rather than assumed production
coverage.

### Value-Consumer Inventory

| Consumer | Current struct behavior | Migration requirement |
| --- | --- | --- |
| `FProperty` value facade and `FReflectedValueStorage` in `Property.cpp` | Size/alignment and all supported value operations already dispatch through `DStruct`/`FDStructOps`; managed storage owns liveness | Keep the API and behavior; construct struct properties without applying generic lifecycle slots |
| Array/Map mutation in `Property.cpp` | Checks nested `FProperty` capabilities and uses managed struct operations | Only the nested registration record changes; no container-operation redesign belongs here |
| Runtime Archive in `Archive.cpp` | Decodes into managed storage and copy-assigns on success | No code migration expected; retain direct/nested round-trip tests |
| AssetCore load in `AssetSystem.cpp` | Uses managed temporary storage and `CopyAssignValue` for transactional commit | No code migration expected; retain DAST v2 compatibility and failure tests |
| Editor draft storage in `PropertyValueDraft.h` | Queries lifecycle, size, and alignment through the `FProperty` facade | No code migration expected; retain snapshot/edit regression coverage |
| Archive, GC schema, snapshots, duplication, and reflected views | Traverse the resolved `FStructProperty::Struct` schema rather than registration parameter storage | Preserve the single resolved descriptor and qualify identities, ordering, references, and authored bytes in Stage 3 |

### Generated-Source Baseline

The representative direct `FCurvePoint::Position` record currently has this
semantic token sequence (line wrapping added only for readability):

```cpp
const Durin::DurinCodeGen::FStructPropertyParams
Z_Construct_DStruct_Fixture_FCurvePoint_Statics::NewProp_Position = {
    "Position", Durin::EPropertyFlags::None, 1,
    static_cast<Durin::uint16>(STRUCT_OFFSET(Fixture::FCurvePoint, Position)),
    static_cast<Durin::uint16>(sizeof(Durin::FVector3)),
    Durin::DurinCodeGen::EPropertyGenFlags::Struct,
    nullptr, nullptr, nullptr, nullptr, nullptr, false, nullptr, nullptr,
    Z_Construct_DStruct_Durin_FVector3, nullptr, nullptr, nullptr, 0,
    sizeof(std::remove_extent_t<decltype(((Fixture::FCurvePoint*)0)->Position)>),
    alignof(std::remove_extent_t<decltype(((Fixture::FCurvePoint*)0)->Position)>),
    nullptr, nullptr
};
```

The typed result must retain the first four schema/location facts, metadata when
present, and `Z_Construct_DStruct_Durin_FVector3`. It must not contain an
explicit `Struct` tag, `sizeof`/`alignof` of the member type, unrelated null
placeholders, `InitializePropertyValue<T>`, or `DestroyPropertyValue<T>`.

## Implementation Stages

### Stage 0: Freeze the Post-StructOps Property Contract

- [x] Verify the completed Reflected Struct Operations baseline, lasting
  documentation, `FDStructOps` access API, managed storage owner, and removal of
  legacy lifecycle callbacks before starting implementation.
- [x] Inventory all generated and handwritten struct-property registrations,
  covering direct members, fixed arrays, nested arrays, map keys, map values,
  `FTransform`, and externally registered intrinsic descriptors.
- [x] Inventory every consumer of `FProperty::GetValueSize`,
  `GetValueAlignment`, `HasValueLifecycle`, `InitializeValue`, and
  `DestroyValue` and classify struct versus non-struct requirements.
- [x] Freeze the concrete `FStructPropertyParams` constructor/factory shape,
  common-base relationship, tag validation, metadata/accessor inputs, and
  registration lifetime.
- [x] Remove duplicate struct size/alignment inputs from the typed contract;
  derive member stride and detached storage facts from the descriptor, while
  deferring removal of unused physical legacy-base slots.
- [x] Define stable registration diagnostics for wrong kinds, resolver and
  descriptor failures, unsupported size/alignment, accessor pairs, and
  metadata pairs while retaining consumer-time capability failures.
- [x] Record representative generated source before implementation so source
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

#### Stage 0 Handoff

- Baseline commit: `f2c4f5b4` (`docs(asset): plan soft asset references`). Stage
  0 working set: this plan plus targeted inspection of `DObjectGlobals.h/.cpp`,
  `Property.h/.cpp`, `reflection_source_writer.py`, `reflection_parser.py`,
  `MathStructs.cpp`, and the focused DHT/CoreDObject/AssetCore registration
  fixtures.
- Key implementation symbols: `FPropertyParamsBase`,
  `FStructPropertyParams`, `ConstructGeneratedProperty`, `FStructProperty`,
  `FProperty::GetValueSize`, `FProperty::GetValueAlignment`,
  `FReflectedValueStorage`, `_make_property_from_spelling`,
  `_property_definitions`, and `Z_Construct_DStruct_Durin_FTransform`.
- Decisions: one derived typed record with an offset constructor and
  `WithAccessors` factory; an appended defaulted layout discriminator preserves
  legacy aggregates and makes the derived cast checkable; the constructor fixes
  the kind and exposes no size/alignment or lifecycle facts; descriptor size
  supplies the runtime stride; invalid static metadata fails fast with stable
  registration codes; unsupported value capabilities remain consumer-time
  failures; non-struct positional records remain untouched.
- Open questions: none for Stage 1. Fixed-array and struct Map-key/value cases
  have no production instances and therefore require explicit generation
  fixtures before the migration can be considered complete.
- Validation: the targeted audit found 44 generated production declarations,
  three handwritten production records, nine handwritten test records, and six
  direct value-API consumer files. The recorded baseline agrees with the
  centralized writer and the completed StructOps handoff; all-plan document
  validation passed after this handoff update.

### Stage 1: Implement Typed Runtime and Generated Registration

- [x] Introduce the concrete `FStructPropertyParams` type and concise
  constructor/factory while preserving the common registration entry required
  by owner property arrays and appending the defaulted layout discriminator.
- [x] Update `ConstructGeneratedProperty` to read only common schema before
  dispatch, cast and resolve only inside the `Struct` arm, validate the frozen
  contract, and avoid applying generic lifecycle slots to the result.
- [x] Make `FStructProperty` fix its kind internally and derive its element
  stride from the validated descriptor while preserving the existing
  `DStruct`-backed value facade and managed storage.
- [x] Update DHT atomically with the runtime ABI so direct, fixed-array,
  Array-inner, Map-key, and Map-value structs call the concise typed API without
  `sizeof(T)`, `alignof(T)`, a kind argument, or unrelated placeholders.
- [x] Preserve qualified resolver identity, property names, flags, dimensions,
  offsets, metadata, ordering, and all non-struct generated records.
- [x] Convert the three intrinsic `FTransform` records and all handwritten
  CoreDObject/AssetCore test metadata in the same ABI change, then remove the
  alias and runtime read of the base `ReferencedStructFunc` slot.
- [x] Add focused generation and CoreDObject tests for valid direct/accessor
  registration, each stable metadata failure, and unavailable lifecycle
  capabilities.

#### Acceptance Gate

- Runtime registration accepts only a valid typed struct-parameter record and
  publishes one resolved `DStruct` relationship.
- No struct property stores or invokes a lifecycle operation that competes with
  its `FDStructOps`.
- A property referencing a non-default-constructible or non-copyable struct can
  register, and unsupported use fails without changing destination storage.
- Existing non-struct property registration remains behaviorally unchanged.

#### Stage 1 Handoff

- Durable rebase baseline: `0321326e` (`refactor(renderer): separate static mesh
  base pass lighting`). Logically, Stage 1 continued from the completed Stage 0
  checkpoint before the stage commits were intentionally squashed. Working set:
  `DObjectGlobals.h/.cpp`, `DurinPropertyTypes.h`, `Property.cpp`,
  `MathStructs.cpp`, `AssetSystem.cpp`, the centralized DHT writer and generation
  test, CoreDObject registration/snapshot tests, AssetCore package tests, and
  this plan.
- Key implementation symbols: `EPropertyParamLayout`,
  `FStructPropertyParams`, `FStructPropertyParams::WithAccessors`,
  `ConstructGeneratedProperty`, `FStructProperty::FStructProperty`,
  `_property_definition`, `FAssetPackageField::TryReadStruct`, and the typed
  registration capability/diagnostic fixtures.
- Decisions: implementation follows the frozen Stage 0 contract without an ABI
  exception. Metadata arguments are omitted entirely when absent; the physical
  legacy base resolver remains for non-struct positional stability but has no
  runtime reader. The AssetCore inspection bridge now rejects zero-sized or
  invalidly aligned descriptors before constructing its transient property.
- Open questions: none for Stage 2. Production still has no fixed struct array
  or struct Map-key/value declaration, so Stage 2 must add explicit DHT fixtures
  before claiming exhaustive form coverage.
- Validation: all 163 DHT tests passed; the `Win64-Debug-DurinEditor-Tests`
  `all` target built successfully; all 60 CoreObjectTests and all 37
  AssetPackageTests passed. The regenerated Editor metadata contained all 44
  inventoried production struct records and zero forbidden layout, lifecycle,
  or placeholder tokens. Source audit found zero legacy struct aggregates and
  zero runtime base-resolver reads.

### Stage 2: Complete Form Coverage and Generated Comparison

- [x] Add generation fixtures for ordinary, deleted-default, deleted-copy,
  private/nontrivial-destructor where representable, metadata, fixed-array,
  Array-inner, Map-key, and Map-value struct properties.
- [x] Add exact assertions for the concise constructor/factory arguments and
  negative assertions for kind, layout expressions, irrelevant placeholders,
  and direct lifecycle-template addresses.
- [x] Audit generated and handwritten output to prove every struct record uses
  the derived contract and no alias or runtime base-resolver fallback remains.
- [x] Regenerate representative engine reflection output and compare property
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

#### Stage 2 Handoff

- Durable rebase baseline: `0321326e` (`refactor(renderer): separate static mesh
  base pass lighting`). Logically, Stage 2 continued from the completed Stage 1
  checkpoint before the stage commits were intentionally squashed. Working set:
  the centralized DHT reflection source writer, its parser/writer integration
  fixture, and this plan.
- Key symbols: `_property_definition`, `FStructPropertyShapes`,
  `test_all_struct_property_forms_use_concise_typed_registration`, and
  `test_struct_property_shape_order_and_resolver_identities_are_stable`.
- Decisions: the writer returns from the typed struct branch before constructing
  generic referenced-kind, element-layout, placeholder, or lifecycle arguments.
  The focused fixture covers ordinary, deleted-default, deleted-copy, public
  nontrivial-destructor, metadata, fixed-array, Array-inner, struct Map-key, and
  struct Map-value records. A private destructor is not modeled as an owning
  field because that C++ declaration is itself ill-formed; the representable
  nontrivial-destructor case carries the registration-boundary coverage.
- Generated comparison: the refreshed Editor output matches the Stage 0 form
  inventory with 36 direct and eight Array-inner records. All 44 have matching
  parameter-symbol/property names, valid qualified struct resolvers, stable
  owner-array order, and no struct kind, `sizeof`/`alignof`, lifecycle-template,
  or null-placeholder token. No production reflection macro changed from the
  Stage 0 baseline; source audit also found no parameter alias or runtime read
  of the retained legacy base resolver slot.
- Open questions: none for Stage 3. Consumer behavior and compatibility
  qualification remain intentionally scoped to that stage.
- Validation: all 165 DHT tests passed; the
  `Win64-Debug-DurinEditor-Tests` `all` target built successfully; all three
  focused typed-registration native tests passed; generated and handwritten
  contract audits reported zero failures.

### Stage 3: Qualify Consumers and Compatibility

- [x] Verify `FVector3` direct fields and its use inside `FTransform`, arrays,
  and maps resolve storage and operations from the intrinsic `DStruct` rather
  than C++ layout thunks.
- [x] Run property snapshot, duplication, Archive object-graph, AssetCore DAST
  v2, and GC tests for direct and nested struct properties.
- [x] Add compatibility assertions for property names, flags, offsets/accessor
  behavior, array dimensions, reflected field order, and authored bytes.
- [x] Measure representative generated source size and compiled metadata size;
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

#### Stage 3 Handoff

- Durable rebase baseline: `0321326e` (`refactor(renderer): separate static mesh
  base pass lighting`). Logically, Stage 3 continued from the completed Stage 2
  checkpoint before the stage commits were intentionally squashed. Changed
  working set: CoreDObject reflection and property-snapshot tests, AssetCore
  package tests, and this plan. Read-only qualification also used the existing
  spline duplication fixture and the public property/parameter declarations.
- Key symbols: `BuiltInMathStructsExposeNestedFieldMetadataAndOperations`,
  `TypedStructPropertyCompiledMetadataFootprintIsRecorded`,
  `RestoresIntrinsicStructsDirectlyAndThroughTransformAndArray`,
  `MathStructRegistrationPreservesDirectAndNestedSchemaIdentity`,
  `DuplicateObjectGraphPreservesIdsAndPublishesSnapshot`, and
  `PreservesMathStructBitsAcrossDastV2AndObjectGraphs`.
- Decisions: no runtime consumer change was necessary. Compatibility assertions
  bind direct `FVector3`, nested `FTransform`, Array-inner, and Map-value
  properties to their intrinsic descriptors and preserve flags, array
  dimensions, offsets, owner relationships, field order, and value access.
  Snapshot and DAST/object-graph tests use signed zero and payload NaN bit
  patterns so authored-byte preservation is observable rather than inferred.
- Source measurement: running the Stage 0-frozen writer and the final writer
  against the same representative `FCurvePoint::Position` model produced
  652-byte and 276-byte definition lines. The initializer alone contracted from
  535 to 159 bytes, a 376-byte or 70.3% reduction. The typed initializer retains
  only name, flags, dimension, offset, and qualified struct resolver; metadata
  remains an optional final pair.
- Compiled metadata measurement under `Win64-Debug-DurinEditor-Tests`: MSVC
  reports `sizeof(FPropertyParamsBase) == 160` and
  `sizeof(FStructPropertyParams) == 168`. The 8-byte increase is the appended
  typed resolver while the physical legacy base resolver is retained for
  non-struct positional ABI stability; across the 44 production records this
  represents 352 bytes and is explicitly not a reduction acceptance condition.
- Open questions: none for Stage 4. Removing physical legacy base fields or
  generalizing typed registration to all property kinds remains outside this
  plan.
- Validation: all 62 CoreObjectTests, all 38 AssetPackageTests, and the focused
  spline duplication test passed. These cover snapshots, transactional Archive
  dispatch, object-graph serialization/duplication, compiled GC schemas,
  supported DAST v2 data, compatibility categories, and exact math-struct bits.

### Stage 4: Document and Complete Cross-Module Validation

- [x] Update Reflection System documentation with the lasting distinction
  between property schema registration and referenced-struct value operations.
- [x] Document the typed struct-parameter authoring path for DHT and external
  intrinsic descriptors without copying active-plan details into runtime docs.
- [x] Run focused DHT generation and CoreDObject property tests plus relevant
  AssetCore, GC, snapshot, and duplication suites under the documented Agent
  Build Profile.
- [x] Complete a successful full `all` build because generated metadata and the
  CoreDObject registration ABI change together.
- [x] Record the final baseline, working set, symbols, decisions, validation,
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

#### Stage 4 Handoff

- Durable rebase baseline: `0321326e` (`refactor(renderer): separate static mesh
  base pass lighting`). Logically, Stage 4 continued from the completed Stage 3
  checkpoint before the stage commits were intentionally squashed. Changed
  working set: the lasting Reflection System contract and this plan.
  Implementation and test sources were read-only during this stage.
- Key symbols: `DurinCodeGen::FStructPropertyParams`,
  `FStructPropertyParams::WithAccessors`, `ConstructGeneratedProperty`,
  `FStructProperty`, the DHT `_property_definition` struct branch, and the
  external intrinsic `Z_Construct_DStruct_Durin_FTransform` registration.
- Decisions: lasting documentation separates property schema/location from
  referenced-struct storage and operations. DHT direct, fixed-array, and nested
  container descriptors and handwritten intrinsic descriptors use the same
  typed resolver contract; accessor-backed authoring requires a paired
  mutable/const accessor and offset `0`. No runtime or test change was required
  in the documentation/validation stage. Completion is recorded in place and
  physical archival remains the normal monthly maintenance operation.
- Generated-source comparison: the Stage 3 representative remains the final
  comparison—652-byte legacy versus 276-byte typed definition lines, with the
  initializer contracting from 535 to 159 bytes (376 bytes, 70.3%). The final
  Editor generated tree contains the same 44 typed records (36 direct and eight
  nested), with zero lifecycle/layout placeholders or invalid resolvers.
- Generalization: concrete typed records for enum, object, container, scalar,
  and other property kinds require a separate plan. That work should migrate
  one kind's authoring, validation, runtime arm, and compatibility tests
  together; common legacy fields should be removed only after their remaining
  consumers and compiled-size effect are measured. No such migration is hidden
  unfinished scope here.
- Open questions: none. The physical legacy base resolver and broader typed
  metadata migration remain explicit deferred follow-ups.
- Validation: all 165 DurinHeaderTool tests, all 62 CoreObjectTests, all 38
  AssetPackageTests, and the focused spline duplication test passed under the
  `windows-msvc-x64` / `Win64-Debug-DurinEditor-Tests` Agent Build Profile. The
  complete `all` target built successfully from the same profile; changed-scope
  documentation validation, the all-plan validator, and final
  generated/handwritten source audits also passed.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Dependency | Completed Reflected Struct Operations baseline and documented `FDStructOps`/managed-storage API |
| Parameter API | Concrete typed record, concise construction, layout/kind validation, stable lifetime, and no irrelevant inputs |
| DHT generation | Direct, fixed-array, array-inner, map-key, and map-value structs emit no property-local lifecycle thunks |
| Compile capability | Deleted-default and deleted-copy struct property fixtures register without unrequested template instantiation |
| Registration failures | Layout/kind mismatch, missing/null descriptors, invalid descriptor size/alignment, and malformed accessor/metadata pairs fail before publication with stable diagnostics |
| Intrinsic bridge | `FVector3` and `FTransform` use the same typed property contract without Core depending on CoreDObject |
| Lifecycle | Detached and temporary values use `FDStructOps` and managed storage with unchanged destinations on failure |
| Reflection behavior | Names, flags, dimensions, offsets/accessors, metadata, ordering, and qualified identities remain stable |
| Serialization and GC | Object graph, DAST v2, snapshots, duplication, hidden references, arrays, and maps retain logical behavior |
| Integration | DHT regeneration, focused suites, generated-source comparison, and full `all` build |

Build and test execution follows [Build and Run](../../../Development/Build/BuildAndRun.md)
and [Native Tests](../../../Development/Build/NativeTests.md).

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
- [Reflection System](../../../Runtime/Core/ReflectionSystem.md)
- [Garbage Collection](../../../Runtime/Core/GarbageCollection.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Tests](../../../Development/Build/NativeTests.md)

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
