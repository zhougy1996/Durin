# Reflected Struct Operations Plan

Summary: Replace unconditional generated DStruct lifecycle callbacks with declarative capabilities and safe reflection, archive, GC, and authored-asset behavior.

Last reviewed: 2026-08-05

Status: Active
Completed:

## Current Status

- Stage 0 completed on 2026-08-05 from baseline commit `560907a0`. The audit
  covers 20 DHT-generated structs, six externally registered Core math structs,
  and every current registration, lifecycle, equality, Archive, authored-asset,
  snapshot, editor-draft, and GC consumer.
- Twenty-four structs have complete reflected authored state. The two
  exceptions, `FImportRecordOutput` and
  `FImportRecordDetachedTombstone`, keep parsed `FAssetPath` values derived from
  reflected path text and are classified as reflected state plus repairable
  derived state. No current struct requires an authored custom codec.
- The only object references inside current reflected structs are the reflected
  `TextureValue` and `DefaultMaterial` properties. No current hidden strong
  reference requires a collector callback before GC migration.
- The version-1 `FDStructOps`, `TDStructOpsTraits<T>`, post-deserialize context,
  callback signatures, capability/failure rules, and registration lifetime are
  frozen in the Stage 0 contract below.
- Stage 1 completed on 2026-08-05 from baseline commit `ec85a7b2`. CoreDObject
  now owns the immutable versioned operation table, generated and intrinsic
  descriptors register through the common trait builder, and `FVector3`
  explicitly provides deterministic zero construction.
- DHT still instantiates property-level default construction and destruction
  helpers for non-struct generated `DPROPERTY` declarations. Struct-valued
  properties now dispatch through their `DStruct` operation table, including
  nested array/map metadata and detached editor storage.
- Stage 2 completed on 2026-08-05 from baseline commit `f7be54d8`. Managed
  reflected-value storage, explicit copy modes, recursive logical equality,
  capability-aware containers, hidden-reference GC traversal, and snapshot
  rooting are implemented and validated.
- Stage 3 is next. Runtime Archive still has no sticky error channel, and
  Archive/AssetCore decoding still updates live aggregate destinations after
  capability preflight. Stage 3 must make decode and post-deserialize failure
  transactional while preserving current bytes and mismatch categories.

## Goal

Make `DSTRUCT()` mean only that a C++ value type participates in reflection.
Each reflected struct must separately expose accurate, queryable operations so
generic callers can construct, destroy, copy, compare, serialize, repair, and
trace the value only when the type declares and supports those semantics.

Unsupported operations must produce deterministic capability failures rather
than invalid generated C++, uninitialized memory, silent field loss, or
partially valid values.

## Scope

- A public `TDStructOpsTraits<T>` customization point and a versioned runtime
  `FDStructOps` capability/callback table.
- Compiler-checked generation of lifecycle operations without unconditionally
  instantiating invalid constructors or destructors.
- Separate copy-construction and copy-assignment semantics with documented
  destination preconditions.
- Optional logical equality, runtime Archive serialization, post-deserialize
  repair/validation, and hidden object-reference collection operations.
- Capability-aware generic struct storage and existing reflection consumers.
- One external intrinsic-registration path for Core-owned value types, with
  `FVector3` as the required reference implementation.
- Fail-closed integration with current field-walk object-graph and authored-
  asset serialization.
- Focused DHT, CoreDObject, AssetCore, GC, and compatibility tests plus lasting
  reflection and asset-contract documentation.

## Non-Goals

- DAST v3 table layout, default-relative value encoding, class default objects,
  or package compression; those belong to the
  [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md).
- A general opaque authored-asset codec framework. It becomes a separate plan
  only if the struct audit identifies durable state that reflected fields plus
  post-deserialize repair cannot represent.
- Network delta serializers, text import/export, hot reload, script
  construction, arbitrary editor visitors, or parity with every UE StructOps
  trait.
- Treating raw byte copy, zeroing, padding comparison, or host ABI layout as a
  valid default for reflected values.
- Replacing GLM-backed math types or introducing a Durin math API facade. The
  operation contract must keep that future implementation change possible, but
  this plan does not perform it.
- Silently changing the DAST v2 wire representation of existing structs.

## Design Decisions and Invariants

### Declaration and Registration Model

- `DSTRUCT()` remains syntax-only reflection marking. It does not imply any C++
  lifecycle or serialization capability.
- CoreDObject owns `TDStructOpsTraitsBase<T>` and the specialization point
  `TDStructOpsTraits<T>`. The default trait derives only mechanically provable
  lifecycle facts from standard C++ type traits; semantic opt-ins are false by
  default.
- A generated C++ helper asks a CoreDObject template builder for `FDStructOps`.
  The Python writer no longer emits direct `new T()`, `~T()`, or `T(const T&)`
  expressions for every struct.
- The template builder uses `if constexpr` and constrained thunks so an absent
  operation yields a null callback and a false capability instead of a compile
  error. A trait that explicitly claims an unsupported member signature fails
  with a focused `static_assert` naming the type and operation.
- Intrinsic structs registered outside DHT use the same operation-table model;
  they do not retain a second callback convention.
- `DStruct` publishes immutable operations after registration. Runtime code may
  query capabilities but may not mutate them after reflection finalization.

### Intrinsic and Externally Registered Structs

- Core-owned types remain free of CoreDObject headers and reflection macros.
  CoreDObject owns their external `DStruct` descriptors, reflected field
  accessors, and operation-table registration.
- DHT's built-in symbol entry only makes the stable reflected identity available
  to property resolution. It does not create a separate lifecycle or
  serialization policy.
- Generated and intrinsic structs use the same `FDStructOps` type, capability
  queries, managed storage, equality traversal, Archive dispatch, GC traversal,
  and diagnostics. Only descriptor authorship differs.
- `Durin::FVector3` is the required reference intrinsic with the stable logical
  descriptor `x: Double`, `y: Double`, and `z: Double`. Member accessors, not
  offsets or contiguous-byte assumptions, expose those fields.
- `FVector3` registers explicit deterministic default construction as
  `(0, 0, 0)`, trivial destruction, and compiler-checked copy construction and
  copy assignment. Mechanical C++ default constructibility alone does not
  establish a deterministic serialization baseline.
- The three reflected fields completely define current logical and authored
  state. `FVector3` therefore uses recursive field equality and field-walk
  Archive/DAST v2 serialization; it declares no custom serializer,
  post-deserialize callback, hidden-reference collector, or custom-asset-codec
  requirement.
- `sizeof(FVector3)` and `alignof(FVector3)` remain runtime storage facts only.
  Replacing GLM later may change the C++ ABI and require a full rebuild, but it
  must not change the qualified reflection identity or the three-component
  authored representation.
- A future DAST v3 `Vector3F64` codec is an AssetCore wire optimization guarded
  by descriptor validation. It is not a StructOps serializer and remains owned
  by the Compact Asset Serialization roadmap.

### Lifecycle Contract

The first operation-table version distinguishes:

- default construction into uninitialized, correctly aligned storage;
- destruction of one live value;
- copy construction into uninitialized, non-overlapping storage;
- copy assignment into one already-live value; and
- semantic zero construction only through an explicit opt-in.

Trivial destruction may be represented by a no-destroy capability rather than
a callable no-op. Copy construction and assignment are never conflated.
Callers must check the exact capability before allocating or mutating storage.
Failure before an operation begins leaves caller-owned storage unchanged.

Generic ownership uses one CoreDObject RAII storage helper that tracks whether a
value is live and pairs every successful construction with exactly one required
destruction. Ad hoc placement-new sequences are not added to AssetCore or
future package formats.

### Logical and Reference Semantics

- Reflected-field comparison remains the default logical-equality path.
- A struct may explicitly opt into an `Identical` callback when reflected
  fields do not completely define equality. Padding and container backing
  storage are never compared.
- A struct with object references outside reflected properties must opt into a
  reference-collector callback. GC and detached-value storage combine reflected
  schema traversal with that callback; declaring neither path for a known
  hidden strong reference is invalid.
- Logical equality and reference collection are independent of copy and
  serialization capabilities.

### Serialization Boundary

- Current reflected-field serialization remains the default for both the
  transient object-graph Archive and authored DAST v2 packages.
- A semantic runtime-Archive serializer is an explicit trait opt-in. It is
  dispatched only by `FArchive`-based paths and does not automatically replace
  the authored `.dasset` representation.
- `FArchive` gains sticky error state and bounded read-failure reporting before
  user-defined struct serializers are enabled. A serializer cannot report
  success after a truncated or otherwise failed nested operation.
- A post-deserialize callback runs only after the complete struct value has been
  decoded successfully. It receives a format-neutral context containing the
  source kind, source version, and an error channel; it may rebuild derived
  caches, normalize values, or reject an invalid invariant.
- AssetCore continues to encode reflected struct fields with tags and invokes
  post-deserialize repair after a successful field load. This preserves
  inspection and unknown-field behavior while supporting derived state.
- A trait may declare that reflected fields are not a complete authored
  representation. AssetCore saving then fails with a stable
  `CustomStructCodecRequired` diagnostic; loading cannot silently fabricate the
  missing state. No runtime Archive callback is treated as an authored codec.
- Field rename and mismatched-type migration remain AssetCore schema concerns.
  This plan preserves the current bounded mismatch failure; it does not pass
  format-specific tags into CoreDObject operations.

### Failure and Compatibility Policy

- Reflection registration succeeds for a type even when it has no generic
  constructor, copy operation, or serializer. Failure occurs only when a caller
  requests an unavailable operation.
- Saving rejects a struct before emitting bytes when its declared authored
  representation is unsupported or its required equality/reference semantics
  are unavailable.
- Loading decodes into temporary managed storage when post-deserialize repair
  can fail; the destination is updated only after validation succeeds.
- Existing simple data structs retain field-walk serialization and equivalent
  lifecycle behavior without requiring handwritten specializations.
- Capability flags and diagnostics are runtime contracts, not wire identities;
  adding the operation table does not itself change DAST v2 bytes.

## Current Foundations and Gaps

### Foundations

- DHT already generates one registration block per reflected struct.
- `DStruct` already owns size, alignment, reflected fields, and three lifecycle
  callback slots.
- Property serializers already recurse through structs, arrays, and maps and
  skip `Transient` fields.
- GC compiles reusable reflected reference schemas for nested structs.
- Property snapshots already own detached bytes and root reflected object
  references.

### Gaps

- Direct generated expressions make deleted, private, or absent lifecycle
  operations fail during generated-code compilation.
- Generated `FProperty` metadata independently instantiates
  `InitializePropertyValue<T>` and `DestroyPropertyValue<T>` for struct-valued
  properties, so the same invalid requirement survives even if only the
  descriptor-level callbacks are removed.
- There is no immutable capability table or distinction between construction
  and assignment.
- No generic RAII owner pairs struct construction and destruction.
- `FArchive` has no error state for safe custom callbacks.
- Field-walk load has no struct-level post-deserialize validation point.
- GC cannot discover strong references hidden outside reflected fields.
- AssetCore cannot distinguish a complete field representation from a struct
  that requires an authored custom codec.

## Stage 0 Frozen Contract and Audit

### Version 1 Operation ABI

- The public ABI names are `DStructOpsVersion`, `EDStructOpsFlags`,
  `FDStructOps`, `EDStructDeserializeSource`,
  `FDStructPostDeserializeContext`, `TDStructOpsTraitsBase<T>`,
  `TDStructOpsTraits<T>`, and `GetDStructOps<T>()`. Version 1 has the numeric
  value `1`; registration rejects a table whose version is not supported.
- `EDStructOpsFlags` contains `DefaultConstruct`, `TriviallyDestructible`,
  `Destroy`, `CopyConstruct`, `CopyAssign`, `ZeroConstruct`, `Identical`,
  `Serialize`, `PostDeserialize`, `CollectReferences`, and
  `AuthoredFieldsComplete`. Except for `TriviallyDestructible` and
  `AuthoredFieldsComplete`, a set flag requires the same-named callback to be
  non-null. The builder is the only producer of flag/callback combinations.
- `FDStructOps` stores `Version`, `Flags`, and the following type-erased
  callbacks. The corresponding trait method is the compiler-checked typed
  customization point.

| Callback | Erased signature | Trait method signature |
| --- | --- | --- |
| `DefaultConstruct` | `void(void* Destination)` | `static void DefaultConstruct(void* Destination)` |
| `Destroy` | `void(void* Value)` | `static void Destroy(T& Value)` |
| `CopyConstruct` | `void(void* Destination, const void* Source)` | `static void CopyConstruct(void* Destination, const T& Source)` |
| `CopyAssign` | `void(void* Destination, const void* Source)` | `static void CopyAssign(T& Destination, const T& Source)` |
| `ZeroConstruct` | `void(void* Destination)` | `static void ZeroConstruct(void* Destination)` |
| `Identical` | `bool(const void* Left, const void* Right)` | `static bool Identical(const T& Left, const T& Right)` |
| `Serialize` | `void(FArchive& Archive, void* Value)` | `static void Serialize(FArchive& Archive, T& Value)` |
| `PostDeserialize` | `bool(void* Value, FDStructPostDeserializeContext& Context)` | `static bool PostDeserialize(T& Value, FDStructPostDeserializeContext& Context)` |
| `CollectReferences` | `void(void* Value, FReferenceCollector& Collector)` | `static void CollectReferences(T& Value, FReferenceCollector& Collector)` |

- `TDStructOpsTraitsBase<T>` mechanically enables default construction,
  destruction, copy construction, and copy assignment from the corresponding
  standard type traits. It exposes `bWithDefaultConstruct`, `bWithDestroy`,
  `bIsTriviallyDestructible`, `bWithCopyConstruct`, and `bWithCopyAssign`.
  `bWithZeroConstruct`, `bWithIdentical`, `bWithSerializer`,
  `bWithPostDeserialize`, and `bWithReferenceCollector` default to false.
  `bHasCompleteAuthoredFields` defaults to true to preserve the current tagged
  field representation; a specialization sets it false only to fail closed.
- `TDStructOpsTraits<T>` derives from the base unchanged. A specialization may
  disable a mechanically available operation or replace its typed method. If it
  enables an optional operation, `GetDStructOps<T>()` uses `requires` checks and
  a focused `static_assert` to require the exact signature above. No disabled
  method is instantiated.
- `DStruct` publishes `GetOps()`, `CanDefaultConstruct()`, `CanDestroy()`,
  `NeedsDestroy()`, `CanCopyConstruct()`, `CanCopyAssign()`,
  `CanZeroConstruct()`, `HasIdentical()`, `HasSerializer()`,
  `HasPostDeserialize()`, `HasReferenceCollector()`, and
  `HasCompleteAuthoredFields()`. `CanDestroy()` is true when either `Destroy` or
  `TriviallyDestructible` is present; `NeedsDestroy()` is true only for the
  callback case.
- `FStructParams` carries `const FDStructOps* Ops`. Generated code and the
  intrinsic bridge pass the address of the function-local static table returned
  by `GetDStructOps<T>()`. `ConstructDStruct` installs that pointer exactly once
  before GC schema finalization. The table has program lifetime, `DStruct`
  exposes it as const, and re-registration with a different pointer is an
  assertion failure.
- `EDStructDeserializeSource` has `RuntimeArchive` and `AuthoredAsset`.
  `FDStructPostDeserializeContext` contains `Source`, `uint32 SourceVersion`, an
  error string channel, and `Fail(std::string_view) -> bool`. Runtime Archive
  calls use version zero; AssetCore supplies the authored format version.

### Memory, Failure, Equality, and Reference Rules

- `DefaultConstruct`, `CopyConstruct`, and `ZeroConstruct` require correctly
  aligned uninitialized storage. `CopyAssign` requires a live destination.
  Source and destination may not overlap. A successful construction makes one
  value live; a failed capability preflight makes no value live and changes no
  caller storage.
- Generic ownership calls `Destroy` exactly once only when `NeedsDestroy()` is
  true. Trivially destructible live values require no callback. A type that is
  neither trivially destructible nor equipped with `Destroy` cannot be owned by
  generic storage.
- Lifecycle callbacks do not report semantic failure. Unsupported requests use
  the stable diagnostic identifier `DStructOperationUnavailable` plus the
  operation name and qualified struct name, and return before allocation or
  mutation. Archive failure uses sticky `ArchiveFailure`; post-deserialize
  rejection uses `PostDeserializeRejected`. AssetCore uses
  `CustomStructCodecRequired` when `AuthoredFieldsComplete` is absent.
- Default logical equality recursively visits every non-transient reflected
  field. Integers, booleans, enums, names, strings, GUIDs, and object references
  compare by value, with object value defined as pointer identity. Arrays are
  ordered. Maps compare key/value associations independently of iteration
  order. `float` and `double` compare their complete bit patterns: positive and
  negative zero are distinct, and NaNs compare equal only when sign, exponent,
  and payload bits match. This matches the current wire-significant scalar
  representation and preserves NaN payload policy.
- An `Identical` callback, when declared, is authoritative for the complete
  struct value; it is not combined with an additional automatic field walk.
  No current struct requires one.
- GC and detached-value rooting first traverse the reflected schema and then
  call `CollectReferences` exactly once. The callback contract is to enumerate
  only strong references outside reflected properties; authors must not repeat
  reflected references. No current struct requires this callback.
- A runtime `Serialize` callback is dispatched exactly once instead of the
  Archive field walk. It cannot clear a sticky Archive error. It has no effect
  on AssetCore's authored representation. `PostDeserialize` runs only after the
  complete value succeeds, and a rejecting callback is evaluated on temporary
  managed storage so the live destination remains unchanged.

### Reflected Struct Inventory

`D/Cc/Ca/X` below means publicly default constructible, copy constructible,
copy assignable, and destructible. All 20 generated structs have that mechanical
capability set today. `Complete` means all durable logical and authored state is
represented by non-transient reflected fields and needs neither a custom
equality callback nor an authored codec.

| Generated type | Lifecycle and default notes | State classification | Reference and current-use classification |
| --- | --- | --- | --- |
| `AssetImport::FImportRecordPayload` | `D/Cc/Ca/X`; ordinary member defaults | Complete | No references; nested authored/Archive import-record state |
| `AssetImport::FImportRecordSource` | `D/Cc/Ca/X`; ordinary member defaults | Complete | No references; nested authored/Archive import-record state |
| `AssetImport::FImportRecordOutput` | `D/Cc/Ca/X`; parsed path starts empty | Reflected fields plus repairable `AssetPath` derived from `AssetPathText` | No references; nested authored/Archive import-record state; requires post-deserialize repair, not a codec |
| `AssetImport::FImportRecordDetachedTombstone` | `D/Cc/Ca/X`; parsed path starts empty | Reflected fields plus repairable `LastAssetPath` derived from `LastAssetPathText` | No references; nested authored/Archive import-record state; requires post-deserialize repair, not a codec |
| `AssetImport::FImportRecordDiagnostic` | `D/Cc/Ca/X`; ordinary member defaults | Complete | No references; nested authored/Archive import-record state |
| `Durin::FSourcePath` | `D/Cc/Ca/X`; empty path is the defined default | Complete | No references; shared nested source-provenance state |
| `Durin::Asset::FCookedPayloadDescriptor` | `D/Cc/Ca/X`; zero/empty descriptor default | Complete | No references; authored properties on mesh, texture, and environment assets plus cooked-container use |
| `Durin::FTextureSourceFile` | `D/Cc/Ca/X`; empty source default | Complete | No references; nested Texture2D/TextureCube provenance |
| `Durin::FTexture2DSourceImportData` | `D/Cc/Ca/X`; empty source default | Complete | No references; authored Texture2D property |
| `Durin::FTextureCubeSourceImportData` | `D/Cc/Ca/X`; six-face layout with empty sources | Complete | No references; authored TextureCube property |
| `Durin::FSplinePoint` | `D/Cc/Ca/X`; default construction creates a new GUID | Complete | No references; nested in `FSplineCurve` |
| `Durin::FSplineCurve` | `D/Cc/Ca/X`; default construction creates the established two-point curve | Complete; evaluation data is owned by a separate immutable snapshot, not the struct | No references; authored/editable spline-component property |
| `Durin::FStaticMeshImportSettings` | `D/Cc/Ca/X`; Durin-axis defaults | Complete | No references; nested static-mesh provenance |
| `Durin::FStaticMeshSourceImportData` | `D/Cc/Ca/X`; empty source default | Complete | No references; authored static-mesh property |
| `Durin::FStaticMeshMaterialSlotDefinition` | `D/Cc/Ca/X`; empty slot default | Complete | Reflected `DefaultMaterial` is the only reference; nested authored static-mesh array |
| `Durin::FCameraProjectionSettings` | `D/Cc/Ca/X`; established perspective defaults | Complete | No references; authored/editable camera property |
| `Durin::FMaterialStaticProperties` | `D/Cc/Ca/X`; established opaque/lit defaults | Complete | No references; authored/editable material property |
| `Durin::FMaterialParameterValue` | `D/Cc/Ca/X`; scalar/vector alternatives start at zero | Complete; inactive alternatives remain deliberately durable | Reflected `TextureValue` is the only reference; nested material definition/override state |
| `Durin::FMaterialParameterDefinition` | `D/Cc/Ca/X`; ordinary member defaults | Complete | References only through reflected nested value; authored material definition array |
| `Durin::FMaterialParameterOverride` | `D/Cc/Ca/X`; ordinary member defaults | Complete | References only through reflected nested value; authored material-instance override array |

The six Core-owned structs are externally described by `MathStructs.cpp`; none
currently receives legacy lifecycle callbacks. All are mechanically copy
constructible, copy assignable, and trivially destructible. Their fields are the
complete logical, Archive, GC, and authored representation and contain no object
references.

| Intrinsic type | Stable reflected fields | Version-1 operation policy |
| --- | --- | --- |
| `Durin::FVector2` | `x`, `y` as `Double` | Mechanical default/copy operations; default is not advertised as semantic zero |
| `Durin::FVector3` | `x`, `y`, `z` as `Double` member accessors | Explicit specialization constructs `(0, 0, 0)`; copy construct/assign; trivial destruction; no optional callbacks |
| `Durin::FVector4` | `x`, `y`, `z`, `w` as `Double` | Mechanical default/copy operations; default is not advertised as semantic zero |
| `Durin::FQuat` | `w`, `x`, `y`, `z` as `Double` | Mechanical default/copy operations; default is not advertised as identity or semantic zero |
| `Durin::FTransform` | `Rotation`, `Translation`, `Scale3D` | Existing default remains identity rotation, zero translation, and unit scale; mechanical copy operations |
| `Durin::FLinearColor` | `R`, `G`, `B`, `A` as `Float` | Mechanical default/copy operations; the existing default constructor is not advertised as semantic zero |

No current type is in the authored-custom-codec class, so Stage 0 creates no
follow-up codec plan and adds no blocker to the compact-serialization roadmap.

### Generic Consumer Inventory

| Consumer | Current behavior | Required migration |
| --- | --- | --- |
| DHT struct registration | Emits direct placement default construction, explicit destruction, and placement copy construction for every generated struct | Emit only `GetDStructOps<T>()` registration and let the builder instantiate available operations |
| DHT property registration | Emits direct `InitializePropertyValue<T>` and `DestroyPropertyValue<T>` for every property, including struct-valued properties | Stop instantiating struct lifecycle directly; route struct property ownership through its `DStruct` capabilities |
| `ConstructDStruct` / `DStruct` | Installs three nullable mutable callbacks through `SetCppOps`; production code does not call them | Install one immutable versioned table and expose capability queries |
| `MathStructs.cpp` | Publishes six field schemas through `FStructParams` but no lifecycle callbacks | Register the same operation table as generated structs; specialize only `FVector3` default construction |
| `FPropertyValueDraft` | Allocates aligned detached storage, calls generated property initialize/destroy callbacks, restores a serialized snapshot, and tracks liveness locally | Use the single managed struct-value owner for struct roots and report unavailable capabilities before allocation |
| Array and map helpers | Array resize default-constructs elements; Archive and AssetCore map load create, insert/copy-assign, and destroy temporary key/value objects through generated container helpers | Keep scalar/container helpers, but preflight nested struct capabilities and use explicit construct/assign semantics where reflection owns the value |
| Runtime Archive | Recursively field-walks structs in object graphs, duplication, editable-property copy, and snapshots; readers have no sticky error | Dispatch `Serialize`, retain field walk by default, add sticky errors, and run post-deserialize transactionally |
| Property snapshots | Serialize detached values into bytes, separately root reflected object references, and compare property identity, bytes, and reference-vector order | Use recursive logical equality and include declared hidden references while preserving detached rooting |
| AssetCore DAST v2 | Tags and recursively serializes struct fields directly into live storage; unknown fields skip and mismatches fail | Preserve bytes, preflight `AuthoredFieldsComplete`, and run post-deserialize on temporary managed storage |
| GC schema | Compiles reflected object, struct, array, and map operations and recursively visits reflected fields | Add one struct collector operation after reflected traversal; no current callback migration is required |
| Tests and hand-authored descriptors | DHT string fixtures and CoreDObject/AssetCore test descriptors reproduce generated/intrinsic registration and nested GC/serialization behavior | Convert fixtures to the common builder and add unavailable/malformed operation coverage |

## Implementation Stages

### Stage 0: Freeze the Operations Contract and Audit Structs

- [x] Inventory every DHT-generated and intrinsic `DStruct`, its constructors,
  destructor, copy behavior, reflected and unreflected state, object references,
  and current serialization use.
- [x] Record the intrinsic `FVector3` contract: stable qualified name and
  component fields, explicit zero default, trivial destruction, copy support,
  complete reflected authored state, and no raw-layout dependency.
- [x] Inventory every current call site that constructs, destroys, copies,
  snapshots, compares, serializes, or GC-traces struct storage.
- [x] Freeze the names, signatures, flags, ABI version, registration lifetime,
  callback failure semantics, and post-deserialize context for `FDStructOps` and
  `TDStructOpsTraits<T>`.
- [x] Classify every current struct as complete reflected-field state,
  reflected fields plus derived repairable state, or requiring an authored
  custom codec.
- [x] Decide whether any current hidden reference requires the reference-
  collector operation before changing GC execution.
- [x] Record any custom-codec requirement as a separate bounded plan and a
  blocking dependency of the compact-serialization roadmap.

#### Acceptance Gate

- Every current reflected struct and generic consumer has one recorded
  capability classification.
- The operation API can represent deleted copy, non-default construction,
  trivial destruction, custom equality, post-deserialize failure, and hidden
  references without ambiguous memory preconditions.
- No unresolved authored-representation or GC requirement is hidden behind
  ordinary reflected-field fallback.

#### Stage 0 Handoff

- Baseline commit: `560907a0` (`docs(reflection): include intrinsic math
  bridge`). Stage 0 working set: this plan only.
- Key implementation symbols: `DStruct`, `FStructParams`,
  `InitializePropertyValue<T>`, `DestroyPropertyValue<T>`, `ConstructDStruct`,
  `_struct_definitions`, `_property_definition`, `MakeStruct`,
  `SerializePropertyValue`, `FGCReferenceSchemaRegistry`,
  `FPropertyValueSnapshot`, and `FPropertyValueDraft`.
- Decisions: one static immutable version-1 table; mechanical lifecycle facts;
  explicit semantic opt-ins; bit-exact floating equality; transactional
  post-deserialize; reflected traversal followed by hidden-reference collection;
  tagged authored fields remain the default.
- Open questions: none for Stage 1. The two import-record path caches are
  assigned to Stage 3 post-deserialize migration, and no current type activates
  the authored-codec dependency.
- Validation: targeted source inventory found 20 generated plus six intrinsic
  descriptors and no production invocation of the legacy `DStruct` lifecycle
  callbacks; `DevTool doc plan validate --scope all` passed for three active,
  zero completed, and 59 archived plans.

### Stage 1: Add Declarative Operations and Generated Registration

- [x] Add the trait base, specialization point, immutable operation table,
  capability queries, and compiler-checked thunk builder to CoreDObject.
- [x] Replace generated direct lifecycle definitions with one operation-table
  registration reference.
- [x] Emit focused compile diagnostics when an explicit semantic trait lacks
  its required method or has the wrong signature.
- [x] Convert intrinsic math struct registration to the same contract.
- [x] Register `FVector3` through the common operation builder with an explicit
  deterministic `(0, 0, 0)` initializer and no custom serialization hooks.
- [x] Preserve qualified-name registration, reflected property generation, and
  GC schema finalization ordering.
- [x] Add DHT generation tests for ordinary, move-only, deleted-default,
  trivial, nontrivial, custom-equality, and malformed-trait fixtures.
- [x] Add CoreDObject tests proving accurate runtime flags and null callbacks.
- [x] Add `FVector3` bridge tests for field identities and accessors, storage
  size/alignment metadata, deterministic initialization, copy construction,
  copy assignment, trivial destruction, and absent optional callbacks.

#### Acceptance Gate

- A reflected type with deleted default or copy construction compiles and
  registers when no declared consumer requires that operation.
- Every published capability has a callable operation with the documented
  preconditions, and every absent capability is safely queryable.
- Existing simple reflected structs retain equivalent behavior without manual
  traits.
- `FVector3` exposes the same operation-table contract as a generated struct
  without adding a CoreDObject dependency to Core.

#### Stage 1 Handoff

- Baseline commit: `ec85a7b2` (`docs(reflection): freeze struct operations
  contract`). Stage 1 working set: `StructOps.h`, `Class.h`,
  `DObjectGlobals.h/.cpp`, `reflection_source_writer.py`, `MathStructs.cpp`, and
  focused DHT and CoreDObject reflection tests.
- Key implementation symbols: `FDStructOps`, `TDStructOpsTraitsBase<T>`,
  `TDStructOpsTraits<T>`, `GetDStructOps<T>()`, `DStruct::InitializeOps`,
  `DStruct::Can*`, `DStruct::Has*`, `FStructParams::Ops`, and
  `_struct_definitions`.
- Decisions: mechanically valid lifecycle methods are emitted through
  constrained trait defaults; semantic operations require explicit flags and
  exact signatures; operation tables are function-local immutable statics;
  repeated descriptor initialization accepts only the same table; generated
  properties and GC-schema finalization retain their established ordering.
- Open questions: none for Stage 2. Property-level struct lifecycle helpers and
  all live/detached consumer preconditions are intentionally assigned to that
  stage.
- Validation: focused DHT generation tests, CoreDObject native tests, and the
  `Engine` target build passed; generated move-only/deleted-default fixtures,
  malformed semantic traits, capability/callback consistency, and the
  intrinsic `FVector3` bridge are covered.

### Stage 2: Make Lifecycle, Equality, and GC Consumers Capability-Aware

- [x] Add the single RAII struct-value storage helper and use it wherever
  reflection owns detached or temporary struct storage.
- [x] Replace ambiguous `CopyValue` usage with explicit copy construction or
  assignment and precondition checks.
- [x] Add recursive logical reflected-value equality with optional struct
  `Identical` dispatch and exact wire-significant scalar behavior.
- [x] Integrate optional hidden-reference collection with nested struct GC and
  detached-value rooting without duplicating reflected references.
- [x] Make unsupported operations return stable errors before changing live
  destinations or ownership state.
- [x] Add lifecycle-count, rollback, nested-container, custom-equality, and
  hidden-reference GC tests.
- [x] Prove `FVector3` equality is the recursive equality of its three `double`
  components, including the selected NaN and signed-zero rules, and never reads
  GLM padding or alignment bytes.

#### Acceptance Gate

- Generic storage never constructs twice, destroys an uninitialized value,
  leaks a live value, or copy-constructs over initialized storage.
- Logical equality observes all declared serialized state without reading
  padding.
- Reflected and declared hidden strong references remain reachable through
  collection and detached snapshot lifetimes.

#### Stage 2 Handoff

- Baseline commit: `f7be54d8` (`feat(reflection): add declarative struct
  operations`). Stage 2 working set: CoreDObject property/storage, Archive, and
  GC-schema code; DHT property/container generation; AssetCore map/array load;
  DurinEd property drafts; and focused DHT, CoreDObject, AssetCore, and editor
  tests.
- Key implementation symbols: `FReflectedValueStorage`,
  `FProperty::Can*Value`, `FProperty::CopyConstructValue`,
  `FProperty::CopyAssignValue`, `ArePropertyValuesIdentical`,
  `FArrayProperty::Resize`, `FMapProperty::Insert`,
  `FGCReferenceSchemaRegistry::Visit(const DStruct*)`, and
  `FGCReferenceSchemaRegistry::VisitProperty`.
- Decisions: one aligned RAII owner tracks liveness for detached values;
  struct-valued properties never carry generated direct lifecycle thunks;
  array/map mutation helpers report capability failure; map equality compares
  logical associations independently of iteration order; scalar floating
  equality compares complete bits; reflected GC traversal precedes one optional
  hidden-reference callback; snapshots deduplicate the combined reference set.
- Open questions: none for Stage 3. Runtime Archive early returns cannot yet
  propagate failure, and aggregate decode remains incremental; sticky Archive
  failure plus temporary managed decode are explicitly assigned to Stage 3.
- Validation: focused DHT generation, CoreDObject, AssetPackage, and editor
  property tests passed. A full `all` build passed under the documented
  `windows-msvc-x64` Agent Build Profile.

### Stage 3: Add Safe Struct Serialization Dispatch

- [ ] Add sticky Archive error state, bounded reader failure propagation, and
  tests for truncated custom payloads.
- [ ] Dispatch explicit runtime-Archive serializers while retaining reflected-
  field traversal as the default.
- [ ] Invoke post-deserialize repair only after successful complete field or
  custom Archive loading, and propagate rejection without committing a partial
  destination value.
- [ ] Apply post-deserialize repair to AssetCore's current tagged struct load
  without changing DAST v2 field bytes.
- [ ] Reject authored save/load paths that encounter a struct declaring an
  incomplete reflected representation, using the stable custom-codec-required
  diagnostic.
- [ ] Preserve unknown-field skipping, type-mismatch classification, object-
  reference handling, dependency discovery, and explicit data-loss policy.
- [ ] Add round-trip tests for default field walk, custom Archive serialization,
  derived-cache repair, invariant rejection, nested structs, arrays, maps, and
  authored-codec-required failures.
- [ ] Add unchanged DAST v2 and object-graph round trips for `FVector3` as a
  direct property and inside `FTransform`, arrays, and maps; cover zero,
  infinities, signed zero, and preserved NaN payload policy.

#### Acceptance Gate

- A custom runtime serializer is called exactly once per value and cannot mask
  archive failure.
- A failed decode or post-deserialize validation leaves the prior destination
  value valid and unchanged.
- Authored packages neither omit declared durable state nor become opaque to
  compatibility inspection without an explicit future codec contract.
- Existing supported DAST v2 fixtures retain identical successful behavior and
  stable incompatibility categories.

### Stage 4: Migrate Existing Structs, Document, and Qualify

- [ ] Add explicit traits only for audited structs whose semantics differ from
  the safe defaults.
- [ ] Remove the legacy three-callback `FStructParams` path and ambiguous
  `DStruct::CopyValue` API after all consumers use the operation table.
- [ ] Update Reflection System and Asset Packages documentation with the lasting
  capability, memory, GC, Archive, and authored-format boundaries.
- [ ] Run focused DHT, CoreDObject, AssetCore, GC, package, duplication, and
  snapshot suites under the documented Agent Build Profile.
- [ ] Complete a successful full `all` build because the generated reflection
  ABI and cross-module runtime contract change together.
- [ ] Record the final baseline, working set, symbols, decisions, validation,
  and any activated custom-codec follow-up in the stage handoff.

#### Acceptance Gate

- Every repository `DSTRUCT` and externally registered intrinsic struct
  compiles with its audited semantics and no unconditional generated lifecycle
  expression or legacy intrinsic callback path remains.
- Focused suites and the full build pass from one coherent generated-code
  baseline.
- Lasting documentation owns the implemented contract, and the compact-
  serialization roadmap can consume it without inventing alternate struct
  lifecycle or equality rules.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| DHT generation | Ordinary and unavailable operations generate valid code; invalid explicit traits produce focused diagnostics |
| Registration | Immutable flags and callbacks match compiler and specialization facts for generated and intrinsic structs |
| Intrinsic bridge | `FVector3` keeps its stable name and `x/y/z` Double fields, initializes deterministically, copies safely, and never serializes layout bytes |
| Lifecycle | Exactly-once construction/destruction, distinct copy modes, alignment, rollback, and nested storage |
| Equality | Scalar, enum, string, object reference, array, map, nested struct, custom identical, NaN, and signed-zero behavior |
| GC and snapshots | Reflected plus hidden strong references remain traced and detached values root exactly their live references |
| Archive | Default field walk, custom serializer dispatch, sticky errors, truncation, and post-deserialize repair |
| Authored assets | Unchanged v2 field representation, unknown retention, stable mismatch categories, and fail-closed custom-codec requirement |
| Integration | Duplication, editing snapshots, object graphs, packages, DHT regeneration, focused suites, and full build |

Build and test execution follows [Build and Run](../Development/Build/BuildAndRun.md)
and [Native Tests](../Development/Build/NativeTests.md).

## Definition of Done

- `DSTRUCT()` no longer forces default construction, copy construction, or
  destruction expressions into generated code.
- `DStruct` exposes one immutable, versioned operation table with unambiguous
  lifecycle preconditions and queryable optional semantics.
- Generic consumers check capabilities and use managed storage without invalid
  lifetime transitions.
- Logical equality, reference collection, Archive customization, and
  post-deserialize repair dispatch only when declared.
- Authored field-walk serialization remains inspectable and fails closed for a
  struct whose declared durable state needs an unavailable custom codec.
- Existing structs are audited and migrated, focused tests and the full build
  pass, and lasting runtime documentation is updated.
- `FVector3` proves that Core-owned types can use the complete reflected-struct
  contract without reflection macros, CoreDObject dependencies, or a second
  serialization model.
- The Compact Asset Serialization roadmap records this plan as a satisfied
  prerequisite or names the remaining custom-codec blocker.

## Deferred Follow-ups

- General versioned custom authored-struct codecs with dependency discovery,
  compatibility inspection, exact unknown retention, and migration hooks.
- Rename-aware or mismatched-field conversion independent of a new package
  format.
- Network and delta serialization, text import/export, script construction, and
  editor visitor operations.
- Move construction or relocation traits if a measured generic consumer needs
  them.

## Related Documentation

- [Compact Asset Serialization Roadmap](../Roadmaps/CompactAssetSerialization.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Garbage Collection](../Runtime/Core/GarbageCollection.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Class.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/MathStructs.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/GCReferenceSchema.cpp`
- `Engine/Source/Runtime/Core/Public/Math/MathFwd.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_generation.py`
- `Engine/Tests/Native/CoreDObjectTests/`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
