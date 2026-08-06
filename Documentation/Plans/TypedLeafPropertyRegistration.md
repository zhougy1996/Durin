# Typed Leaf Property Registration Plan

Summary: Replace positional leaf-property aggregates with concise typed descriptors and make built-in value size, lifecycle, relationships, and access follow one authoritative contract.

Last reviewed: 2026-08-07

Status: Active
Completed:

## Current Status

Stage 0 is ready to begin. The current generated leaf-property records are
aliases of `FPropertyParamsBase`, so every String, Name, Guid, Enum, Object,
Bool, and Numerical property is emitted as one positional aggregate containing
unrelated relationship pointers, container slots, wrapper state, accessors,
metadata, size/alignment, and lifecycle callbacks.

The representative generated `FTextureCubeSourceImportData::DecoderId` String
record currently supplies 21 positional values. Its authored facts are only
name, flags, array dimension, byte offset, and optional metadata; the String
kind, C++ storage size/alignment, and value operations are all derivable. Enum
and Numerical records use the same legacy layout. Array, Map, Struct, and
SoftObject records already have typed descriptors and establish the migration
pattern.

The source audit also found these contract gaps:

- `FStringPropertyParams`, `FEnumPropertyParams`, every Numerical parameter
  name, `FBoolPropertyParams`, `FNamePropertyParams`, `FGuidPropertyParams`, and
  `FObjectPropertyParams` remain aliases of the common base rather than distinct
  authoring types.
- `Inner`, `Key`, `Value`, and `ReferencedStructFunc` remain in the base after
  typed Array, Map, and Struct registration stopped using them. They are read
  only as forbidden-slot checks in SoftObject validation.
- DurinHeaderTool emits `ValueSize`, `ValueAlignment`, and direct
  `InitializePropertyValue<T>` / `DestroyPropertyValue<T>` callbacks for every
  legacy leaf record even though `FProperty` already derives built-in size,
  alignment, default construction, and destruction from the property kind.
- Generated leaf records do not emit copy construction or copy assignment
  callbacks. Consequently `CanCopyConstructValue()` and
  `CanCopyAssignValue()` report false for String, Enum, Numerical, Bool, Name,
  Guid, and hard Object properties even though those supported value types are
  copyable.
- `FObjectProperty` currently reaches a `TObjectPtr<T>` value through an
  `FObjectPtr*` view. Typed Object registration needs storage-specific logical
  accessors and exact value operations so runtime code does not depend on the
  wrapper's member layout.

No active plan owns this work.

## Goal

Make each supported leaf-property family author only the facts that distinguish
that family, while preserving one base-pointer property array and one runtime
dispatch entrypoint. Generated source must be concise, runtime registration must
reject mismatched typed records before casting, and every built-in leaf type
must expose correct default, destroy, copy-construct, and copy-assign behavior.

## Scope

- Bool and the fixed-width signed, unsigned, and floating-point Numerical kinds.
- `std::string`, `FName`, and `FGuid` built-in value kinds.
- Reflected Enum fields for every supported underlying width and signedness.
- Raw `DObject`-derived pointers and `TObjectPtr<T>` hard-object fields.
- Direct fields, fixed C arrays, Array inner values, Map keys, Map values,
  metadata-bearing fields, and accessor-backed intrinsic fields.
- The common parameter base, generic escape hatch, runtime property
  construction, leaf value operations, DurinHeaderTool output, handwritten
  descriptors, tests, and lasting reflection documentation.
- Mechanical relocation of SoftObject-specific state out of the common base
  when required by the base cleanup; SoftObject behavior and generated syntax
  otherwise remain unchanged.

## Non-Goals

- Replacing the implemented `FArrayPropertyParams`, `FMapPropertyParams`,
  `FStructPropertyParams`, their operation tables, or `DStruct` capabilities.
- Redesigning soft-reference identity, loading, serialization, editing, or GC
  behavior.
- Adding new reflected C++ types, containers, enum rules, property flags, or
  metadata keys.
- Changing serialized property identities, asset formats, archive formats, or
  compatibility hashes except where tests prove that the same logical schema is
  encoded from the new descriptors.
- Changing editor widgets or user-visible property semantics.
- Preserving source compatibility for handwritten aggregate initialization of
  `FPropertyParamsBase`; repository-owned call sites migrate in this plan.
- Treating generated parameter layout as a stable binary ABI.

## Design Decisions and Invariants

### Descriptor Families

The public parameter names remain stable, but they stop aliasing the common
base:

| Family | Typed record | Author-supplied facts |
| --- | --- | --- |
| Bool and Numerical | Aliases of a constrained `TPlainPropertyParams<TValue, Kind>` specialization | name, flags, array dimension, offset or accessors, optional metadata |
| String, Name, Guid | Aliases of the same constrained plain-value pattern | name, flags, array dimension, offset or accessors, optional metadata |
| Enum | `FEnumPropertyParams` | common facts plus one `DEnum` resolver |
| Hard Object | `FObjectPropertyParams` | common facts, one `DClass` resolver, and a typed Raw/ObjectPtr factory |
| Generic | `FGenericPropertyParams` | explicit custom size/alignment/operations required only by intentional `None`/generic tests or external registration |

The plain-value template is an implementation mechanism, not an invitation to
form arbitrary type/kind pairs. Compile-time constraints admit only the exact
supported mapping. The familiar aliases such as `FFloatPropertyParams` and
`FStringPropertyParams` remain the author-facing API and are distinct from
unrelated property families.

Every typed record fixes its own `Kind` and parameter `Layout`. Callers cannot
pass either value. The common base is a runtime dispatch header, not a public
aggregate construction surface.

### Common Versus Type-Specific State

`FPropertyParamsBase` retains only facts needed before type dispatch or shared
by every descriptor:

- name, property flags, array dimension, and offset;
- fixed kind/layout discriminants;
- an optional paired mutable/const value accessor;
- an optional metadata pointer/count pair.

Type-specific resolvers, storage modes, operation tables, value accessors, and
custom lifecycle callbacks live in the corresponding derived record. Legacy
`Inner`, `Key`, `Value`, and `ReferencedStructFunc` slots are removed instead of
being retained as null padding. Array/Map/Struct/SoftObject descriptors continue
to carry their existing relationships in their own typed records.

Owner property arrays remain `const FPropertyParamsBase* const*`. Runtime code
reads only the common discriminants before validating the expected layout and
casting to a record that actually owns family-specific state. A generic/base
record falsely tagged as Enum, Object, Array, Map, Struct, or SoftObject must
fail deterministically without an invalid derived-object read.

### Size, Alignment, and Value Operations

Generated leaf source does not author `sizeof`, `alignof`, member `decltype`, or
free lifecycle callback addresses.

- Bool, Numerical, String, Name, and Guid size/alignment and operations come
  from the fixed C++ type associated with their typed parameter alias.
- Enum element size and alignment come from the resolved `DEnum` underlying
  type contract. Registration validates a supported, nonzero underlying size
  before publishing the property.
- Raw and `TObjectPtr<T>` Object records use separate typed factories. The
  factory installs exact storage size/alignment, lifecycle operations, and
  logical get/set accessors for the declared value type while retaining the
  resolved expected class and the raw-versus-wrapper policy bit required by
  Archive, AssetCore, and GC.
- Array and Map container size/lifecycle continue to come from their operation
  tables; Struct size/lifecycle continue to come from `DStruct`; SoftObject
  continues to use its typed descriptor.
- The generic escape hatch owns its explicitly supplied value facts in its
  derived record. Those fields do not return to the common base.

For every supported built-in leaf kind, `FProperty` must report and implement
default construction, destruction, copy construction, and copy assignment.
Trivial scalar/enum/raw-pointer paths may use audited trivial operations;
String, Name, Guid, and wrapper paths use their exact C++ operations. Fixed
arrays apply operations one element at a time through the existing property
stride contract.

The implementation must not merely hide the current long aggregate behind a
constructor while keeping multiple competing authorities. Generated metadata,
runtime kind dispatch, resolved type descriptors, and typed record factories
must have one documented authority for each size, alignment, relationship, and
operation fact.

### Object Storage Access

`FObjectPropertyParams::Raw<TObject>(...)` and
`FObjectPropertyParams::ObjectPtr<TObject>(...)` are the selected generated
factories. Both retain the generated class resolver; the template argument owns
the exact C++ value access and lifecycle instantiation, including the existing
forward-declared target contract.

`FObjectProperty` stores logical read/write callbacks supplied by the typed
record and uses them in `GetObjectPropertyValue(...)` and
`SetObjectPropertyValue(...)`. Runtime code must not cast a
`TObjectPtr<TObject>` object to `FObjectPtr`. The wrapper-policy query remains
available because serialization and GC intentionally distinguish raw from
strong wrapper references.

### Generated Source Contract

Representative direct and nested output should have these semantic forms
(exact names may follow the final code style, but no extra facts may reappear):

```cpp
const Durin::DurinCodeGen::FStringPropertyParams ...::NewProp_DecoderId = {
    "DecoderId", Durin::EPropertyFlags::None, 1,
    static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::FTextureCubeSourceImportData, DecoderId))
};

const Durin::DurinCodeGen::FFloatPropertyParams ...::NewProp_Intensity = {
    "Intensity", Durin::EPropertyFlags::Edit, 1,
    static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::DLightComponent, Intensity))
};

const Durin::DurinCodeGen::FEnumPropertyParams ...::NewProp_Policy = {
    "Policy", Durin::EPropertyFlags::None, 1,
    static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::AssetImport::FImportRecordOutput, Policy)),
    Z_Construct_DEnum_Durin_AssetImport_EImportRecordOutputPolicy
};

const Durin::DurinCodeGen::FStringPropertyParams ...::NewProp_NamedScores_Key = {
    "NamedScores_Key", Durin::EPropertyFlags::None, 1, 0
};

const Durin::DurinCodeGen::FObjectPropertyParams ...::NewProp_DestinationObject =
    Durin::DurinCodeGen::FObjectPropertyParams::ObjectPtr<Durin::DObject>(
        "DestinationObject", Durin::EPropertyFlags::None, 1,
        static_cast<Durin::uint16>(STRUCT_OFFSET(Durin::Asset::DAssetRedirector, DestinationObject)),
        Z_Construct_DClass_Durin_DObject);
```

Optional metadata is the final pointer/count pair. Fixed-array records change
only `ArrayDim`; nested records use offset zero. Plain, Enum, and Object records
must not contain explicit kind/layout tokens, unrelated null placeholders,
element/value `sizeof` or `alignof`, member `decltype`, or direct initialize,
destroy, copy-construct, or copy-assign callbacks.

### Validation and Failure Policy

Generated descriptors and repository-owned handwritten descriptors are trusted
program data, so malformed registration remains fail-fast before a property is
linked to its owner. Stable diagnostics cover:

- kind/layout mismatch;
- missing or null Enum/Class resolver and unsupported Enum underlying data;
- invalid Object storage mode or missing object logical/value operations;
- one-sided accessor pairs or nonzero offsets combined with accessors;
- one-sided metadata pointer/count pairs;
- invalid generic size/alignment/operation contracts.

Registration validation applies uniformly to direct, fixed-array, nested, and
accessor-backed records. No consumer silently substitutes a different type or
operation when a typed descriptor is malformed.

### Compatibility

Runtime `FProperty` identity remains name, kind, flags, array dimension,
location/access, element stride, and resolved referenced type. Parameter-record
layout and generated C++ spelling are not serialized identities. Asset schema
hashes and compatibility checks must remain logically unchanged for an
equivalent reflected declaration, including their deliberate raw-versus-wrapper
Object distinction.

## Current Foundations and Gaps

| Area | Current foundation | Gap closed by this plan |
| --- | --- | --- |
| Typed complex descriptors | Array, Map, Struct, and SoftObject fix kind/layout and expose concise constructors/factories | Leaf aliases still expose the legacy positional base |
| Runtime dispatch | `ConstructGeneratedProperty` already validates typed complex layouts before casting | `Legacy` combines all leaf families and permits unrelated slots |
| Built-in lifecycle | `FProperty` derives leaf alignment and has fallback initialization/destruction by kind | Generated records duplicate those facts and built-in copy capability is absent |
| Enum metadata | `DEnum` owns underlying kind and size | Each Enum property redundantly emits element/value size, alignment, and lifecycle callbacks |
| Hard Object metadata | `FObjectProperty` retains expected class and raw/wrapper policy | Parameter state is generic and wrapper value access relies on `FObjectPtr` layout interpretation |
| DHT type model | Parser already distinguishes every supported leaf kind, Enum resolver, Object target, fixed arrays, and nesting | Writer funnels all remaining kinds through one long generic aggregate path |
| Handwritten descriptors | Intrinsic math and native tests cover offset- and accessor-backed fields | They directly aggregate-initialize the base and must migrate to the same typed contract |
| Lasting documentation | Reflection docs describe typed Struct/Array/Map/SoftObject registration and runtime property nodes | Leaf registration ownership and built-in copy semantics are not documented |

## Implementation Stages

### Stage 0: Freeze the Leaf Descriptor and Value-Operation Contract

- [ ] Inventory all generated leaf forms: every Bool/Numerical kind, String,
  Name, Guid, each Enum underlying width/signedness, raw Object pointer,
  `TObjectPtr<T>`, fixed arrays, Array/Map nesting, and metadata.
- [ ] Inventory repository-owned handwritten `FPropertyParamsBase` and leaf
  alias aggregates, distinguishing intentional generic tests from supported
  typed registrations.
- [ ] Confirm the common-base field set and assign every removed field to a
  typed owner or runtime-derived authority.
- [ ] Freeze the allowed `TPlainPropertyParams<TValue, Kind>` mappings, Enum
  constructor, Object Raw/ObjectPtr factories, accessor forms, metadata order,
  kind/layout values, and fail-fast diagnostic codes.
- [ ] Freeze built-in default/destroy/copy behavior for every leaf kind,
  including enum zero/default behavior, String/Name/Guid exact operations,
  raw-null Object initialization, and exact `TObjectPtr<T>` access.
- [ ] Record representative current and required generated token sequences for
  direct, fixed-array, nested, metadata, Enum, and both Object storage modes.
- [ ] Confirm no current serialized identity or compatibility hash depends on
  `FPropertyParamsBase` physical layout or generated callback addresses.

#### Acceptance Gate

- Every current leaf family and handwritten descriptor has one migration path.
- Every base field has exactly one retained, relocated, derived, or deleted
  disposition.
- Type/kind mapping, layout-safe dispatch, Object access, value operations, and
  failure semantics have no open design question.
- The required generated forms contain only irreducible authoring facts.

#### Stage 0 Handoff

- Baseline commit:
- Working set:
- Key symbols and decisions:
- Open questions:
- Validation:

### Stage 1: Implement Typed Runtime Descriptors and Built-In Operations

- [ ] Make `FPropertyParamsBase` a non-authorable common dispatch header and
  remove legacy family-specific/container/struct/value-operation slots.
- [ ] Add the constrained plain-value descriptor specializations and preserve
  the existing public Bool/Numerical/String/Name/Guid parameter names as typed
  aliases.
- [ ] Implement `FEnumPropertyParams`, `FObjectPropertyParams`, and the explicit
  `FGenericPropertyParams` escape hatch with family-owned state.
- [ ] Relocate any SoftObject-only size, lifecycle, resolver, or validation
  state from the common base without changing SoftObject behavior.
- [ ] Extend layout validation and runtime construction so each kind resolves
  element size, alignment, relationships, accessors, and value operations from
  its selected authority before property publication.
- [ ] Give all supported built-in leaf properties correct
  `CanDefaultConstructValue`, `CanDestroyValue`, `CanCopyConstructValue`, and
  `CanCopyAssignValue` answers and implementations.
- [ ] Add typed hard-Object logical accessors and remove `TObjectPtr<T>` to
  `FObjectPtr` storage reinterpretation from `FObjectProperty`.
- [ ] Add focused runtime tests for direct, fixed-array, and detached storage
  lifecycle/copy behavior plus malformed layout/resolver/accessor/metadata
  descriptors.

#### Acceptance Gate

- The common base contains no leaf-, container-, struct-, or SoftObject-specific
  relationship/operation field.
- Runtime dispatch never casts to a typed record before validating its layout.
- Every built-in leaf kind passes default/destroy/copy tests in detached storage
  and preserves fixed-array element stride.
- Raw and wrapper Object get/set, GC visibility, and policy queries work without
  wrapper layout reinterpretation.
- Malformed typed descriptors fail before owner publication with deterministic
  diagnostics.

#### Stage 1 Handoff

- Baseline commit:
- Working set:
- Key symbols and decisions:
- Open questions:
- Validation:

### Stage 2: Emit Concise Typed DurinHeaderTool Records

- [ ] Split the generic leaf writer path into plain, Enum, and Object typed
  record emission while leaving Array/Map/Struct/SoftObject emission on their
  existing typed paths.
- [ ] Stop emitting explicit kind/layout, element/value size, alignment,
  unrelated resolver/container/struct slots, wrapper booleans, member
  `decltype`, and direct lifecycle callbacks for leaf properties.
- [ ] Emit Object `Raw<T>` and `ObjectPtr<T>` factories with the resolved target
  type and generated class resolver, including nested and forward-declared
  targets.
- [ ] Preserve final optional metadata arguments, fixed-array dimensions,
  nested offset zero, and deterministic formatting.
- [ ] Expand exact-output tests across all leaf families and forms. Each test
  checks both the required concise sequence and forbidden legacy tokens.
- [ ] Verify generated declarations still upcast typed records into owner
  property arrays without changing registration order or referenced-helper
  collection.

#### Acceptance Gate

- The representative `DecoderId` String record contains only name, flags,
  array dimension, and offset.
- Numerical/Bool/String/Name/Guid output contains no `sizeof`, `alignof`, kind,
  resolver, placeholder null, member `decltype`, or lifecycle thunk.
- Enum output adds only its enum resolver; Object output adds only its typed
  factory, target, and class resolver; metadata is present only when authored.
- Direct, fixed-array, Array-inner, Map-key/value, metadata, raw Object, and
  ObjectPtr fixtures match exact expected generated source.
- All DurinHeaderTool tests pass from a cold generated-output baseline.

#### Stage 2 Handoff

- Baseline commit:
- Working set:
- Key symbols and decisions:
- Open questions:
- Validation:

### Stage 3: Migrate Handwritten Registrations and Qualify Consumers

- [ ] Convert intrinsic math fields to typed Numerical descriptors, using the
  paired accessor factory where GLM-backed members cannot use byte offsets.
- [ ] Convert CoreDObject, AssetCore, Engine, and editor native-test descriptors
  to typed leaf records; use `FGenericPropertyParams` only for tests that
  intentionally exercise generic behavior.
- [ ] Remove repository-owned direct aggregate initialization of
  `FPropertyParamsBase` and obsolete helper fields/functions left with no owner.
- [ ] Re-run Archive, AssetCore schema/compatibility, GC, snapshot, reflected
  property editing, Array, and Map coverage for each migrated leaf family.
- [ ] Confirm raw Object properties retain their deliberate serialization/GC
  exclusions and `TObjectPtr<T>` properties retain strong-reference behavior.
- [ ] Update the lasting reflection contract with the typed leaf families,
  generated-source ownership, built-in copy semantics, and Object logical
  access contract.

#### Acceptance Gate

- Targeted source search finds no repository-owned base aggregate used to
  describe a supported leaf type.
- All intrinsic and test-only descriptors use the same constructors/factories
  as generated records or the explicit generic escape hatch.
- Asset schema identities and compatibility results for unchanged reflected
  declarations remain unchanged.
- Archive, GC, snapshots, detached values, editor drafts, Array, and Map tests
  preserve behavior and exercise the new copy capabilities.
- Lasting documentation, runtime code, and generated examples describe one
  authority for each property fact.

#### Stage 3 Handoff

- Baseline commit:
- Working set:
- Key symbols and decisions:
- Open questions:
- Validation:

### Stage 4: Complete Cross-Module Validation

- [ ] Regenerate representative engine reflection output from a cold DHT cache
  and compare String, Numerical, Enum, raw Object, ObjectPtr, fixed-array, and
  nested records with the Stage 0 baseline.
- [ ] Run the focused DurinHeaderTool, CoreDObject, AssetCore, Engine, and editor
  native tests selected in the validation matrix.
- [ ] Complete one successful full `all` build from the configured Agent Build
  Profile using the repository build entrypoint.
- [ ] Search generated and repository-owned source for forbidden legacy leaf
  aggregate patterns and unexplained direct lifecycle callback emission.
- [ ] Record final validation evidence and move lasting rules out of this plan
  before marking it complete.

#### Acceptance Gate

- Cold generation, all focused tests, and the full `all` build succeed.
- No supported leaf record uses the positional base layout or carries redundant
  size/alignment/lifecycle facts in generated source.
- Existing reflection, serialization, asset compatibility, GC, editor, and
  container behavior remains green across module boundaries.
- The plan has no deferred correctness or compatibility blocker.

#### Stage 4 Handoff

- Baseline commit:
- Working set:
- Key symbols and decisions:
- Open questions:
- Validation:

## Validation Matrix

| Area | Coverage | Required outcome |
| --- | --- | --- |
| Descriptor compile-time contract | Allowed/disallowed plain mappings, constructor/factory signatures, base non-authorability | Invalid type/kind pairs and direct base authoring do not compile in compile-contract fixtures |
| Runtime registration | Each leaf layout plus malformed layout/resolver/accessor/metadata/generic records | Valid records publish correct `FProperty` subclasses; invalid records fail before linking |
| Value operations | Bool, every Numerical width, String, Name, Guid, every Enum width, raw Object, ObjectPtr | Size/alignment/default/destroy/copy construct/copy assign are correct in detached and fixed-array storage |
| Object access and GC | Raw pointer and `TObjectPtr<T>`, direct and nested | Logical get/set is type-correct; strong wrappers are collected; raw pointers retain policy exclusions |
| DHT exact output | Direct, fixed, nested Array/Map, metadata, Enum, Raw/ObjectPtr | Concise expected strings match and all forbidden legacy tokens are absent |
| Intrinsic registration | FVector/FQuat accessor-backed Numerical fields and offset-backed color fields | Typed descriptors preserve member access, size, editing, serialization, and equality |
| CoreDObject | Reflection type, malformed registration, snapshot, archive, container, GC tests | Property identity and consumer behavior remain correct with built-in copy capability |
| AssetCore | Package, import-record, schema identity, compatibility, transactional load tests | Logical schema is unchanged and leaf values load/commit correctly |
| Editor/Engine | Reflected property view/edit/draft and representative asset settings | Existing leaf editing and snapshots remain correct |
| Cross-module build | Full `all` target | All generated modules compile and link against the typed descriptor API |

Configure, build, and native-test execution follows
[`Documentation/Development/Build/BuildAndRun.md`](../Development/Build/BuildAndRun.md).

## Definition of Done

- All supported leaf parameter names are typed descriptors rather than aliases
  of `FPropertyParamsBase`.
- The common base is non-authorable and contains only pre-dispatch/shared facts.
- Generated leaf records contain no unrelated placeholders or duplicate
  size/alignment/lifecycle metadata.
- Enum and Object relationships live only in their family descriptors; Array,
  Map, Struct, and SoftObject retain their typed owners.
- Built-in leaf default, destruction, copy construction, and copy assignment
  are capability-correct and covered by tests.
- Hard-object access no longer relies on interpreting `TObjectPtr<T>` as
  `FObjectPtr`, while raw/wrapper policy behavior is preserved.
- All handwritten supported leaf descriptors use the typed API; the generic
  escape hatch is explicit and narrowly used.
- Cold DHT generation, focused tests, and one full `all` build pass.
- Lasting reflection documentation owns the final contract and this plan is
  marked completed with evidence.

## Deferred Follow-ups

- Adding new leaf types or property kinds.
- Third-party/source compatibility shims for the removed base aggregate; add
  one only if a concrete external consumer is identified before implementation.
- Further compression of already-typed SoftObject generated syntax beyond the
  base-field relocation required here.
- Binary-size or compile-time benchmarking of generated reflection after the
  correctness migration; optimize only from measured evidence.

## Related Documentation

- [`Documentation/Runtime/Core/ReflectionSystem.md`](../Runtime/Core/ReflectionSystem.md)
- [`Documentation/Development/Build/BuildAndRun.md`](../Development/Build/BuildAndRun.md)
- [`Documentation/Plans/Archive/2026-08/TypedStructPropertyRegistration.md`](Archive/2026-08/TypedStructPropertyRegistration.md)
- [`Documentation/Plans/Archive/2026-08/ReflectedStructOperations.md`](Archive/2026-08/ReflectedStructOperations.md)

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Property.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DurinPropertyTypes.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/DObjectGlobals.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Property.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/MathStructs.cpp`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_generation.py`
- `Engine/Tests/Native/CoreDObjectTests/Private/ReflectionTypeTests.cpp`
- `Engine/Tests/Native/CoreDObjectTests/Private/ZPropertyValueSnapshotTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/ImportRecordTests.cpp`
