# Generated Reflection System

This document describes the reflection framework that is currently implemented in Durin. Object lifetime and collector semantics are documented separately in [Garbage Collection](GarbageCollection.md).

## Overview

Durin reflection uses selected C++ headers as the source of truth. A module lists reflected headers in its `.dmodule` file under `ReflectHeaders`. During configure/build, DurinHeaderTool scans those headers and writes generated files under:

```text
Engine/Intermediate/Build/<Platform>/<RuntimeVariant>/<Module>/DHT
```

Generated files are atomically replaced. DHT serializes commands that use the
same platform and runtime variant. Debug, Release, and Profiling presets in one
worktree share configuration-independent generated files.

The current system supports:

- `DCLASS()` classes with `GENERATED_BODY()`
- fully qualified runtime type identities, such as `Durin::AActor`
- namespace-safe generated helper names, such as `Z_Construct_DClass_Durin_AActor`
- module export files as thin reflected-symbol indexes
- module manifests for incremental reflection generation
- generated `.gen.h` and `.gen.cpp` files
- generated class registration into `DClass`
- generated enum registration into `DEnum`
- generated value-struct registration into `DStruct`
- generated property metadata for reflected `DPROPERTY()` fields
- primitive, `std::string`, enum, struct, object-pointer, fixed C array, `std::vector`, and `std::unordered_map` property nodes
- nested container property metadata, with recursive array/map inner property trees
- runtime `DObject::IsA` and `Cast<T>` based on the `DClass` hierarchy
- runtime `DObject` registration in `GDObjectArray`
- manual object destruction and minimal mark-sweep garbage collection
- reflected scalar/string/object-reference serialization through `FArchive`
- minimal in-memory object graph save/load helpers

The system does not currently implement CDO behavior, hot reload, function reflection, general template reflection, schema migrations, weak references, incremental/concurrent GC, or complete metadata specifier parsing.

`DSTRUCT()` value types generate `StaticStruct()` and `DStruct` metadata without changing normal C++ copy/move behavior. [Core math aliases](Math.md) cannot depend on `CoreDObject`, so `FVector3`, `FQuat`, and `FTransform` are registered externally as intrinsic structs and still appear as ordinary `FStructProperty` values.

## Parsing Scope And Generated-File Naming

DurinHeaderTool is a reflection metadata extractor, not a standalone C++ compiler
or a complete semantic analyzer. Each parse has a hermetic semantic boundary:
the current configured reflected header, the versioned DHT built-in prelude, and
canonical reflected-symbol exports. DHT blanks every ordinary `#include`
directive before invoking libclang while retaining its newline, so an included
project, third-party, generated, or system header cannot change extraction or
source locations.

Reflection marker ownership follows the physical source file. Each configured
reflected header is responsible for markers written directly in that header;
ordinary included headers are neither semantic inputs nor diagnostic targets. A
reflected header is checked by its own DHT parse. Reflection markers must be
written directly in that configured header rather than introduced indirectly by
another macro. DHT hot-path AST traversal preserves this boundary and does not
recursively lint another physical file.

The built-in prelude owns fundamental Durin aliases, supported parser macros,
container declarations, and intrinsic parser types. Reflected exports provide
synthetic class, struct, and enum declarations for cross-header references. A
type alias, conditional macro, base class, or `DPROPERTY()` type whose required
meaning exists only in an ordinary included header is rejected with a stable
non-hermetic-dependency diagnostic. Meanings declared directly in the current
header remain valid.

Clang translation units are parsed without function bodies. Reflection consumes
declaration signatures, fields, constructor and destructor declarations, enum
constants, and annotation attributes; inline implementation bodies are outside
the reflection model and must not be made an extraction dependency.

The parser may therefore use a partial Clang translation unit that contains
diagnostics unrelated to the reflection declarations being extracted. Such
diagnostics do not by themselves make reflection generation invalid, and
reflection generation does not require a diagnostic-free translation unit,
source locations for referenced types, or canonical fully qualified spellings
for every referenced type. DHT's contract is to extract the supported reflection
markers and the declaration information required by the generated metadata. A
failure is relevant when that required information cannot be extracted, not
merely when Clang cannot completely compile the surrounding header.

Generated reflection files are intentionally flat within each module's DHT
output directory. A reflected header named `Actor.h` generates `Actor.gen.h` and
`Actor.gen.cpp` regardless of the header's source subdirectory. This keeps the
generated include contract simple, but makes the reflected header basename part
of the module's generated-file identity. Consequently, reflected headers in the
same module must have unique basenames; directory qualification does not
disambiguate two reflected headers with the same filename. This is a deliberate
module-authoring constraint rather than a requirement to mirror source
directories in generated output.

## Build Flow

The build flow is:

```text
.dmodule and runtime-variant config
-> generated module CMake metadata
-> module export file
-> module reflection files and manifest
-> C++ compile/link
```

`add_durin_module(...)` wires two DurinHeaderTool commands for reflected modules:

- `generate_module_export_file`
- `generate_reflection_files`

Each command exposes a stamp as its primary build output. The export, generated
C++ files, and private manifests are declared CMake `BYPRODUCTS`; the stamp is
touched only after DHT completes successfully. This lets Ninja repair any missing
generated artifact while avoiding a repeated command when a semantic export or
generated source remains byte-for-byte unchanged. In particular, an unchanged
public `.export` keeps its timestamp so downstream modules are not regenerated.

These disposable outputs are reconstructed from a versioned per-header cache at
`<Project>/Intermediate/Build/<Platform>/<RuntimeVariant>/DHTCache/`. Export
entries retain the raw symbol projection required for deterministic module
resolution. Reflection entries retain generated header/source text,
class/property counts, and resolved-symbol dependency snapshots. Entries are
canonical checksummed JSON and are keyed by the current header plus the complete
phase context; reflection also hashes the complete canonical available-symbol
set. DHT validates an entry completely before publishing any cached output.

CMake clean does not own this cache, so a warm rebuild can rematerialize every
missing generated output with zero parser calls. Project purge removes the
enclosing runtime-variant intermediate root and intentionally makes the next
generation cold. Interrupted cache replacement leaves the previous complete
entry usable when possible, while interrupted output materialization is repaired
from already published entries on the next ordinary build.

CMake computes a stable tool fingerprint from the DHT Python implementation and
its pinned requirements. A tool input change triggers reconfiguration and both
generation stages. DHT records the fingerprint in its manifests, so parser or
writer changes invalidate the internal cache even when reflected headers have not
changed.

The export command runs before reflection generation so other modules can
resolve reflected base classes and object-pointer property types without
reparsing dependency headers. A module export command also depends on the
public exports of reflected dependency modules; reflection generation depends
on the same exports plus its owning module export.

Reflection generation also depends directly on the module's reflected headers. A whitespace-only header edit may leave the public `.export` symbol index unchanged, but it still must regenerate that header's `.gen.h/.gen.cpp` because `GENERATED_BODY()` macro names include source line numbers.

DurinHeaderTool logs key build progress at `INFO` level:

- export skip status, scan start/end, and symbol count
- aggregate export cache hits, misses, materializations, parses, and miss reasons
- export parse worker count when multiple headers require parsing
- reflection manifest preparation
- number of regenerated/skipped headers
- aggregate reflection cache hits, misses, materializations, parses, and miss reasons
- reflection parse worker count when multiple headers require regeneration
- dependency export loading
- module reflection completion time

Per-header timings and cache decisions are DEBUG-only. Invalid entry shape,
truncation, or checksum disagreement emits a warning and follows the normal
parser fallback; manual cache deletion is not required for recovery.

## Symbol Model

The runtime identity for every reflected C++ type is the fully qualified C++ name:

```text
Durin::DObject
Durin::AActor
Durin::DSceneComponent
```

Generated helper names are implementation details derived from the qualified name by replacing `::` with `_`:

```text
Durin::AActor -> Z_Construct_DClass_Durin_AActor
```

The current scheme is `qualified-underscore-v1`. Reflected namespace and type-name segments must not contain `_`; DurinHeaderTool rejects those symbols because the generated helper name would become ambiguous.

## Export Files

Each reflected module writes:

```text
<Module>.export
```

The export file currently uses schema v5 JSON:

```json
{
  "SchemaVersion": 5,
  "Module": "Engine",
  "Symbols": {
    "Durin::AActor": {
      "Kind": "class",
      "ShortName": "AActor",
      "Namespace": "Durin",
      "QualifiedName": "Durin::AActor",
      "GeneratedHelperName": "Z_Construct_DClass_Durin_AActor",
      "Header": "Public/Engine/Actor.h",
      "API": "ENGINE_API",
      "BaseQualifiedName": "Durin::DObject"
    }
  }
}
```

Export files are intentionally thin. They are used for symbol resolution during parsing/generation, not as the full runtime reflection database.

Export files should only change when the exported reflected-symbol contract changes. Whitespace-only edits in a reflected header may force the owning module's export command to run, but should not rewrite the public `.export` file if the symbol index is unchanged. This keeps downstream modules from regenerating purely because an upstream header timestamp changed.

DurinHeaderTool stores export-generation input fingerprints in a private sibling cache:

```text
<Module>.export.manifest
```

The module export manifest currently uses schema v7 and records the schema, tool
version, tool fingerprint, options, runtime variant, platform, dependency-export
content digests, reflected-header fingerprints, and a serializable raw symbol
projection for each header. It lets `generate_module_export_file` skip entirely
when no inputs changed. If only some headers changed, DurinHeaderTool reparses
only those headers and reuses the other raw projections, including an empty
projection for a header that exports no symbols. It then resolves bases against
the complete current-module projection and dependency exports in deterministic
header/name order before writing the unchanged thin public `.export` format.
The reflected-header content identity is the stored MD5; timestamp and size are
only a cheap guard that avoids
rehashing an unchanged file. Touching a header therefore refreshes its cached
filesystem metadata without invalidating export or reflection parsing when its
content hash is unchanged. When multiple headers require parsing, the export
generator parses them in a bounded worker pool and merges results in module
header order. Other modules should not depend on or read this private manifest
directly.

`CoreDObject` uses `DObject/MirrorExportTypes.h` under `_DHT_EXPORTS_PARSER` to publish intrinsic core types such as `Durin::DObject`, `Durin::DType`, `Durin::DStructBase`, and `Durin::DClass` without generating duplicate runtime class registration for those intrinsic types.

## Manifest Files

Each reflected module writes:

```text
<Module>.manifest
```

The manifest currently uses schema v6 JSON and is private to DurinHeaderTool. It records:

- `SchemaVersion`
- `ToolVersion`
- `ToolFingerprint`
- `SymbolNameScheme`
- `ModuleName`
- `RuntimeVariant`
- `Platform`
- `GeneratorOptionsHash`
- reflected header fingerprints
- `GeneratedOutputs`, the complete set of reflection outputs owned by the module
- content digests for generated outputs, used to repair damaged files from a
  valid persistent entry
- `PendingCleanupOutputs`, stale owned outputs whose deletion must be retried
- dependency export content digests
- resolved reflected-symbol dependencies per header

Changing the tool version, tool fingerprint, schema, symbol-name scheme, runtime variant,
platform, options hash, dependency exports, or reflected header fingerprints
invalidates generated reflection outputs.

Dependency export changes are filtered through resolved symbol dependencies. If an upstream export changes but a header does not reference the changed reflected symbols, that header can keep its existing generated files. Missing generated outputs still force regeneration for the affected header. A missing, truncated, or structurally invalid export or manifest is treated as a cache miss and regenerated.

After all new reflection outputs are committed, DHT writes the new manifest with
the difference between the old and new ownership sets in
`PendingCleanupOutputs`. It then deletes only those named files and clears the
pending list in a final atomic manifest write. An interrupted or failed deletion
is therefore retried by the next generation command, while a failed generation
never removes outputs belonging to the last successful manifest. Manifests
older than schema v5 derive their initial ownership set from `ReflectHeaders`
while upgrading to the current schema.

## Generated Header Contract

For each reflected header, DurinHeaderTool writes:

```text
<Header>.gen.h
```

The generated header provides declarations injected by `GENERATED_BODY()`. It includes:

- global construct helper declarations
- a generated statics forward declaration
- `GetPrivateStaticClass()`
- friend declarations for generated helpers
- `DECLARE_CLASS(...)`
- deleted copy and move constructors
- default constructor glue
- `CURRENT_FILE_ID`

`GENERATED_BODY()` is defined in `ObjectMacros.h` so that it uses `CURRENT_FILE_ID` at expansion time. This lets ordinary headers include `ObjectMacros.h` before their generated header while still expanding the generated body macro correctly.

## Generated Source Contract

For each reflected header, DurinHeaderTool writes:

```text
<Header>.gen.cpp
```

The generated source includes:

- `DObject/GeneratedCppIncludes.h`
- the original reflected header
- cross-module helper declarations
- `FClassRegistrationInfo`
- `FEnumRegistrationInfo`
- `T::GetPrivateStaticClass()`
- no-register and full construct helpers
- generated statics containing `FClassParams`
- generated enum value tables and `FEnumParams`
- generated property parameter records
- compiled-in registration records
- generated object-initializer constructor definitions when needed

Generated code uses fully qualified C++ type names for reflected C++ types.

Struct-valued fields use the concrete
`DurinCodeGen::FStructPropertyParams` record. DurinHeaderTool emits the field
name, flags, array dimension, top-level offset (or `0` for a nested container
descriptor), optional metadata, and a qualified resolver for the referenced
`DStruct`. Direct fields, fixed C++ arrays, vector inner descriptors, and map
key/value descriptors all use this typed record. It deliberately contains no
property-local `sizeof`/`alignof`, initialization, destruction, or copy thunk;
those facts belong to the referenced struct descriptor.

`DENUM()` declarations are explicit reflected enum opt-ins. `DENUM(DisplayName = "...")`
optionally supplies the editor-facing type label. An enumerator can similarly use
`DMETA(DisplayName = "...")` immediately after its identifier and before any
initializer. `DMETA` is valid only there; duplicate or unknown keys, malformed
strings, and annotations outside a reflected enum are DHT errors. DurinHeaderTool
exports reflected enums as `Kind: "enum"` symbols, records scoped/unscoped form
and underlying type metadata, and emits `Z_Construct_DEnum_*` helpers plus
generated value tables. Reflected enum fields also generate `FEnumProperty`
metadata that points at the corresponding `DEnum`.

## Runtime Type Data

`DurinCodeGen::FClassParams` passes generated class metadata into `DurinCodeGen::ConstructDClass(...)`:

- no-register class function
- qualified class name
- short class name
- optional editor display name from `DCLASS(DisplayName = "...")`
- optional default object name from `DCLASS(DefaultObjectName = "...")`
- property parameter array
- property count

`DClass` keeps runtime identity, C++ spelling, editor presentation, and instance naming separate. `QualifiedName` remains the stable serialized/type-lookup identity and `ShortName` remains the C++ class spelling. `DisplayName` is used by editor UI, while `DefaultObjectName` is used when an instance is created without an explicit name. When metadata is omitted, Durin removes the conventional `A`/`D` prefix when followed by an uppercase letter; display names additionally split CamelCase words. For example, `AStaticMeshActor` defaults to display name `Static Mesh Actor` and object name `StaticMeshActor`.

`ConstructDClass(...)` forces class registration, then creates `FProperty` nodes from generated property parameters and attaches top-level fields to `DStructBase::ChildProperties`. Container inner/key/value properties are constructed recursively and owned by their containing `FArrayProperty` or `FMapProperty`; they are not inserted into the class property chain.

### Leaf Property Registration

Built-in leaf records use typed authoring descriptors while owner property
arrays remain `const FPropertyParamsBase* const*`. The common base is a
non-authorable dispatch header containing only name, flags, array dimension,
offset, fixed kind/layout discriminants, an optional paired mutable/const value
accessor, and optional metadata. Runtime registration validates the layout and
common pairs before reading a family descriptor.

Bool, fixed-width signed/unsigned/floating-point Numerical, String, Name, and
Guid records are constrained `TPlainPropertyParams<TValue, Kind>` aliases. A
direct or nested generated record contains only name, flags, array dimension,
offset, and optional final metadata pointer/count; kind, layout, size,
alignment, and lifecycle callbacks are fixed by the alias. External intrinsic
registration uses the same constructor or `WithAccessors(...)` for a paired
accessor-backed field with offset zero.

Enum records add one `DEnum` resolver. The resolved descriptor is the authority
for the supported underlying kind and size. Object records use
`FObjectPropertyParams::Raw<T>(...)` or `ObjectPtr<T>(...)` and add one class
resolver. Those factories install exact `T*` or `TObjectPtr<T>` size,
alignment, value operations, and logical get/set callbacks. Consequently
`FObjectProperty` never interprets `TObjectPtr<T>` storage as `FObjectPtr`;
Archive, AssetCore, and GC still use `IsObjectPtrWrapper()` to preserve the
intentional raw-versus-strong-reference policy distinction.

Every built-in leaf property supports default construction, destruction, copy
construction, and copy assignment. Scalars and Enums use their exact fixed
type, String/Name/Guid use their C++ value type, raw Object defaults to null,
and ObjectPtr operations use the declared wrapper specialization. Fixed arrays
apply those operations per element through the property stride. Intentional
custom `None` registrations use `FGenericPropertyParams` and must explicitly
supply element/value size, alignment, and all four operations; those facts are
not part of the common base.

### Struct Property Registration

Struct-property registration describes schema and field access, while the
referenced `DStruct` describes the value stored there. The typed parameter
record fixes the generated kind/layout pair to `Struct`; runtime construction
validates that pair, the resolver and descriptor, descriptor size/alignment,
the optional accessor pair, and the optional metadata pair before publishing
the property. `FStructProperty` retains the resolved descriptor and derives its
element stride from that descriptor rather than from generated C++ layout or
lifecycle callbacks.

DurinHeaderTool authors direct and nested records automatically. External
intrinsic registration uses the same contract: construct
`FStructPropertyParams` with name, flags, array dimension, byte offset, and a
resolver returning the referenced `DStruct`. Use
`FStructPropertyParams::WithAccessors(...)` only when the containing type must
expose paired mutable/const accessors instead of a byte offset; accessor-backed
records keep offset `0`. Optional metadata is the final pointer/count pair.
Neither form supplies value construction, destruction, copying, size, or
alignment. The intrinsic struct's own `FStructParams` and `FDStructOps` remain
the authority for those facts.

`DurinCodeGen::ConstructDEnum(...)` forces enum registration for generated
`DEnum` singletons. `DEnum` stores qualified name, short name, display name,
scoped flag, underlying kind/size, and a read-only value table. Generated UTF-8
pointers are copied into process-lifetime runtime strings during construction.

`DStructBase` stores its superclass through `SuperStructBase`. `DClass::GetSuperClass()` exposes this as a `DClass*`.

`DObject::IsA(const DClass*)` walks the `DClass` superclass chain. `Cast<T>` uses `T::StaticClass()` and `IsA`.

## Reflected Struct Operations

`DSTRUCT()` opts a value type into metadata; it does not promise that generic
code can construct, copy, compare, serialize, repair, or trace that type. Each
`DStruct` instead owns one immutable, process-lifetime `FDStructOps` table. The
version-1 table advertises default construction, destruction (including the
separate trivially-destructible fact), copy construction, copy assignment, zero
construction, logical identity, runtime Archive serialization,
post-deserialize repair, hidden-reference collection, and completeness of the
authored reflected-field representation. `DStruct` exposes matching capability
queries and never changes the table after registration.

Generated structs register `GetDStructOps<T>()`; externally described intrinsic
structs use the same path. `TDStructOpsTraits<T>` derives safe mechanical
defaults from the C++ type and lets an audited specialization disable or replace
an operation or enable an optional callback. The specialization is
compile-checked against the callback signature before its operation is emitted.
Current non-mechanical specializations are the deterministic zero default for
`FVector3` and post-deserialize repair for the two import-record structs whose
parsed asset paths are derived from reflected path text. The former mutable
three-callback registration path and ambiguous struct copy operation no longer
exist.

Consequently, seeing a reflected struct as a direct property, fixed array
element, vector element, or map key/value never requests a C++ operation from
the property declaration itself. Construction, destruction, copying,
alignment, identity, serialization, repair, and hidden-reference traversal are
all capability-checked through the referenced `DStruct` and its `FDStructOps`.

Construction callbacks require aligned uninitialized storage; copy assignment
requires a live destination. `FReflectedValueStorage` is the common aligned RAII
owner used for detached reflected values and tracks whether exactly one value is
live. It destroys only a successfully constructed value. Generic storage,
container mutation, editor drafts, Archive loading, and authored struct loading
preflight the operations they require before allocation or mutation. An
unsupported request returns `DStructOperationUnavailable` with the operation
and qualified struct name.

Default struct identity recursively compares every non-`Transient` reflected
field. Arrays are ordered, maps compare key/value associations rather than
iteration order, object values use pointer identity, and `float`/`double`
compare complete bit patterns. Thus signed zero remains distinct and NaNs are
identical only when sign, exponent, and payload bits match. A declared
`Identical` callback is authoritative for the complete struct value and is not
combined with a field walk.

Reflected GC traversal visits the compiled schema first and then invokes one
optional `CollectReferences` callback for strong references outside reflected
fields. Callback authors must not repeat reflected references. Detached
property snapshots root the deduplicated union of schema and callback
references and compare materialized values through the same logical-identity
rules.

## Garbage Collection Integration

`NewObject<T>(Outer, Name)` constructs reflected runtime objects and registers them in `GDObjectArray`. After generated properties are attached, every `DClass` and `DStruct` compiles an immutable internal GC reference schema containing only reflected `TObjectPtr` paths through supported containers and nested structs. Class schemas include inherited operations, while struct schemas are reused wherever that value type appears. Collection executes this schema without reinterpreting the complete property chain. Raw reflected `DObject*` properties are not GC strong references.

GC schema tokens are an internal execution detail rather than a public reflection API. Serialization, duplication, editing, and general property iteration continue to interpret `FProperty` metadata because they require non-reference fields and different traversal semantics. The current reflection model has no hot reload or runtime property mutation, so schemas are assembled once and are not invalidated.

Outer hierarchy queries, one-way `Child -> Outer` reachability, root management, object handles, garbage requests, GC-controlled destruction, and automatic collection policy are specified in [Garbage Collection](GarbageCollection.md).

## Serialization

`DObject` has a virtual `Serialize(FArchive& Ar)` hook. The default implementation calls `SerializeDObjectProperties(...)`, which walks reflected properties and serializes supported non-`Transient` fields.

The current archive layer includes:

- `FArchive` with load/save mode, sticky `ArchiveFailure` state, bounded byte
  and string serialization, and object-reference serialization
- `FMemoryWriter` and `FMemoryReader`
- `SaveObjectGraphToMemory(DObject* RootObject, std::vector<uint8>& OutBytes)`
- `LoadObjectGraphFromMemory(const std::vector<uint8>& Bytes)`

The supported reflected property payloads are numeric primitives, `bool`, `std::string`, reflected enum storage, direct object references, vectors, maps, and recursively nested supported containers. Object references are serialized as object ids inside the saved object graph, not as process pointer addresses.

Object graph saving first gathers the root's structural descendants through the Outer index plus its serialized object references, assigns ids, writes object records, and serializes each object's reflected properties. This graph-gathering rule defines archive scope and is independent of GC reachability. Loading creates all object records first, then deserializes properties so object-reference ids can resolve to loaded objects.

Structs use the reflected non-`Transient` field walk by default. A declared
runtime `Serialize` callback replaces that complete walk and is invoked exactly
once per value; it cannot clear an earlier sticky Archive failure. Loading a
struct always decodes into default-constructed managed storage. After the
complete field walk or custom serializer succeeds, an optional
`PostDeserialize` callback receives `RuntimeArchive` with source version zero.
Only successful repair is copy-assigned into the live destination, so
truncation, missing capabilities, or `PostDeserializeRejected` leaves the prior
value unchanged.

The object graph format is an internal v1 binary memory format for tests and engine plumbing. Long-lived content uses the separate field-tagged `.dasset` package format documented in `Documentation/Runtime/Assets/AssetPackages.md`; the memory format remains useful for transient cloning and focused tests.

## Enum Metadata

`DENUM()` supports scoped and unscoped C++ enums. The generated `DEnum` metadata stores:

- qualified enum name
- short enum name
- editor display name
- scoped/unscoped form
- underlying numeric kind and size
- read-only name/value/display-name records

When explicit display metadata is absent, CoreDObject derives the enum type label
from the short name by removing a conventional leading `E` when the next
character is uppercase, then humanizes word boundaries. Enumerator labels
humanize their identifiers without removing a prefix. `DEnum` exposes
`FindValueRecordByName(...)` and `FindValueRecordByValue(...)`; the latter returns
the first declaration when aliases share a numeric value. The compatibility
name/value lookup functions preserve their existing behavior.

Enum metadata and reflective access use a canonical `uint64` value channel. Signed
values are sign-extended into that channel, while unsigned values preserve their
full range. `FEnumProperty` still reads and writes the enum's declared underlying
width, so reflected objects retain their native layout and serialization size.
`DisplayName` is presentation-only: it is never a lookup identity and is not
serialized. `FEnumValue::Name` and `Value` remain the stable reflection and
persistence identities. Numeric values missing from the reflected table remain
invalid records and editor code displays their numeric representation.

Enum properties use `FEnumProperty`. The property still stores the ordinary property metadata such as name, offset, array dimension, and size, and additionally references the generated `DEnum` singleton for the enum type.

## Property Metadata

The current generated property metadata supports these scalar and value kinds:

- `int8`, `int16`, `int32`, `int64`
- `uint8`, `uint16`, `uint32`, `uint64`
- `float`
- `double`
- `bool`
- `std::string`
- reflected enum values
- reflected `DObject*` pointers
- reflected hard `TObjectPtr<T>` values
- reflected soft `TSoftObjectPtr<T>` values
- reflected struct values

The runtime property node stores:

- name
- flags
- array dimension
- byte offset
- element size
- generated property kind
- referenced class for hard- and soft-object properties
- referenced enum for enum properties
- owner field/property for nested property trees

Concrete property types live in `DObject/DurinPropertyTypes.h`. `DObject/Property.h` keeps the common `FProperty` base and value-pointer helpers.

Supported property node types are:

- `FNumericProperty`
- `FBoolProperty`
- `FStringProperty` for `std::string`
- `FEnumProperty`
- `FObjectProperty`
- `FSoftObjectProperty`
- `FArrayProperty`
- `FMapProperty`
- `FStructProperty`

`FProperty::ContainerPtrToValuePtr<T>(...)` and `GetValuePtr(...)` provide field address access from an owning object/container address. `FObjectProperty::GetObjectPropertyValue(...)` and `SetObjectPropertyValue(...)` provide direct hard-object-reference access for GC and serialization. `FSoftObjectProperty::GetSoftObjectPtr(...)` exposes the wrapper value and expected class without treating its weak cache as reflected state. `FStringProperty` exposes a `std::string*` pointer helper. Array and Map properties expose the capability-checked container operations described below. Map operations expose mutable mapped values while keeping keys immutable in place; key edits use copy, uniqueness validation, and node-based rename operations so hashing and equality invariants remain intact.

DurinHeaderTool recognizes direct and fixed-array `TSoftObjectPtr<T>` fields and
soft values nested through supported Array, Map-value, and reflected-struct
paths. `T` must resolve to a reflected `DObject` subclass. Raw wrapper aliases,
unresolved or non-object targets, soft Map keys, and unsupported qualifiers are
rejected rather than emitted with incomplete metadata. `FSoftObjectProperty`
is a distinct property kind: serialization and editor access use its canonical
path identity, while GC deliberately excludes it from the strong-reference
schema. `TWeakObjectPtr<T>` remains unsupported by DHT property generation.

Generated leaf records do not emit element-size expressions. Plain aliases own
their exact C++ value type, Enum records resolve their underlying type, and
Object/SoftObject factories instantiate their declared storage type. Reflected
struct properties likewise emit no element-size expression because their
resolved `DStruct` owns size and alignment. This keeps ABI ownership with the
compiler-built descriptor and prevents synthetic parser declarations or host
STL contents from changing runtime property sizes.

`DStructBase::ChildProperties` stores only properties declared directly on that reflected type. Superclass properties are reached through the superclass chain. Use `FindPropertyByName(...)` or the property iteration helpers when inherited properties should be visible.

## Container Properties

DurinHeaderTool currently recognizes these standard library container spellings for `DPROPERTY()` fields:

- `std::vector<T>`
- `std::unordered_map<K, V>`

`std::vector<T>` creates an `FArrayProperty` whose `Inner` points at the generated property metadata for `T`.

`std::unordered_map<K, V>` creates an `FMapProperty` whose `KeyProp` and `ValueProp` describe the key and value types.

Generated Array and Map records are typed `FArrayPropertyParams` and
`FMapPropertyParams`. Each record owns the nested logical property descriptors
and resolves one immutable, process-lifetime `FArrayOps` or `FMapOps` table for
its concrete storage specialization. Generated source does not contain
property-local count, element, traversal, construction, insertion, or removal
functions. The current reusable adapters accept only default-form
`std::vector<T>` and `std::unordered_map<K, V>`; storage is a replaceable C++
backend and is not part of reflected type identity or serialized schema.

The version-1 operation tables advertise individual capabilities instead of
assuming every container can perform every operation. Consumers request and
check only what they need: count and const/mutable traversal, indexed Array
element access, resizing, Map lookup/insertion/removal/key rename, and detached
construction plus transactional commit. Missing capability slots return a
typed `EContainerOpResult`; a property is never treated as empty and a mutation
is never silently skipped. Map traversal is single-pass and callback-scoped.
There is no indexed Map API, no persistent iterator identity, and no permission
to retain entry pointers across structural mutation.

Array/Map loading uses `FDetachedContainerStorage`: input is decoded into a
managed temporary container, counts are bounded before allocation, duplicate
logical Map keys are rejected, and the destination is committed only after all
nested values and post-deserialize work succeed. Failure leaves the destination
logically unchanged. GC, runtime Archive, snapshots, AssetCore, and editor
property access all use the same checked operations.

Containers may be nested up to `MAX_CONTAINER_PROPERTY_DEPTH` in DurinHeaderTool, currently 4. For example:

```cpp
DPROPERTY()
std::vector<std::vector<int32>> NestedScores;

DPROPERTY()
std::unordered_map<std::string, std::vector<DObject*>> Groups;
```

Nested generated parameter records use path-style names and postorder generation:

- `NestedScores_Inner_Inner`
- `NestedScores_Inner`
- `NestedScores`
- `Groups_Value_Inner`
- `Groups_Value`
- `Groups`

Only the top-level field uses `STRUCT_OFFSET`. Nested inner/key/value properties use offset `0` because they describe an element type, not a field within the reflected class. Struct descriptors at either level still resolve the same referenced `DStruct` and do not carry local value-operation thunks.

Container key restrictions are intentionally stricter than value restrictions. Map keys may be primitive, `bool`, `std::string`, reflected enum, or reflected struct types. Map keys may not be `DObject*` or another container. Vector inner types and map values may be primitive, `bool`, `std::string`, reflected enum, reflected struct, reflected `DObject*`, or another supported container within the depth limit.

Canonical writers do not persist `std::unordered_map` iteration or bucket
order. They build a logical token for each supported key and sort entries by
that token. The canonical key domain includes bool, integers, floating-point
values, reflected enums, strings, `FName`, `FGuid`, and complete ordinary
reflected structs composed recursively from supported fields. Object/container
keys and structs with hidden state or custom identity/serialization operations
cannot be canonicalized and fail before output. Ordinary streaming Archives
retain traversal order unless they explicitly advertise canonical Map output.

Unsupported `DPROPERTY()` types fail DHT with stable diagnostics. This includes
unknown templates, smart pointers, non-reflected object pointers, references,
containers nested deeper than the configured limit, and aliases whose meaning
is available only from a neutralized include. `std::vector<bool>`, custom vector
allocators, custom unordered-map hash/equality/allocator arguments, and
unsupported Map key kinds are rejected during generation rather than failing
later in generated C++.

Runtime nested metadata can be walked with `ForEachNestedProperty(...)`. The helper visits array inner properties and map key/value properties recursively. Nested properties also expose their containing property through `FProperty::GetOwnerProperty()`.

## Testing And Verification

The current focused DHT tests live under:

```text
Engine/Source/Programs/DurinHeaderTool/tests
```

The implementation entrypoint lives inside the package at `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/__main__.py`.
Internal code lives under `Engine/Source/Programs/DurinHeaderTool/durin_header_tool`, split by role:
`cli`, `config`, `io`, `model`, `parser`, `resolver`, `cache`, `writers`, and `generators`.

Run them with:

```powershell
.\.venv\Scripts\python.exe -m pytest Engine\Source\Programs\DurinHeaderTool\tests
```

For C++ verification, build the registered Agent profile through the repository build driver:

```powershell
.\DevTool.bat build --target all --plain
```

Run the focused CoreDObject tests after lifecycle, GC, or serialization changes:

```powershell
.\DevTool.bat test --target CoreObjectTests --plain
```

When adding new reflection behavior, validate both the DHT tests and a real C++ build. The generated files are part of the compile surface, and macro/friend/access errors often appear only during C++ compilation.
