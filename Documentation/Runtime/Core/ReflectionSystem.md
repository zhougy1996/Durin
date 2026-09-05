# Generated Reflection System

Summary: Define reflected type registration, generated metadata, properties, and serialization integration.

Modules: CoreDObject

Last reviewed: 2026-09-03

This document describes the reflection framework that is currently implemented in Durin. Object lifetime and collector semantics are documented separately in [Garbage Collection](GarbageCollection.md).

## Overview

Durin reflection uses selected C++ headers as the source of truth. A module lists reflected headers in its `.dmodule` file under `ReflectHeaders`. During configure/build, DurinHeaderTool scans those headers and writes generated files under:

```text
Engine/Intermediate/Build/<Platform>/<RuntimeVariant>/<Module>/DHT
```

Generated files are atomically replaced. DHT serializes conflicting project
metadata writers and commands that write the same module; independent modules
may run concurrently. Debug, Release, and Profiling presets in one
worktree share configuration-independent generated files.

The current system supports:

- `DCLASS()` classes with `GENERATED_BODY()`
- fully qualified runtime type identities, such as `Durin::AActor`
- namespace-scoped generated helpers, such as
  `::Durin::Z_Construct_DClass_AActor`
- module export files as thin reflected-symbol indexes
- clean-surviving module phase state for incremental generation and output repair
- generated `.gen.h` and `.gen.cpp` files
- generated class registration into `DClass`
- generated enum registration into `DEnum`
- generated value-struct registration into `DStruct`
- generated property metadata for reflected `DPROPERTY()` fields
- primitive, `std::byte`, `std::string`, enum, struct, object-pointer, fixed C
  array, `std::vector`, and `std::unordered_map` property nodes
- a distinct UE-style `FEditorBulkData` property node with opaque atomic
  serialization and identity callbacks (not an Array or Blob alias)
- nested container property metadata, with recursive array/map inner property trees
- runtime `DObject::IsA` and `Cast<T>` based on the `DClass` hierarchy
- runtime `DObject` registration in `GDObjectArray`
- manual object destruction and minimal mark-sweep garbage collection
- reflected scalar/string/object-reference serialization through `FArchive`
- minimal in-memory object graph save/load helpers

The system does not currently implement editable class defaults, hot reload,
function reflection, general archetype chains, schema migrations, weak
references, incremental/concurrent GC, or complete metadata specifier parsing.

`DSTRUCT()` value types generate `StaticStruct()` and `DStruct` metadata without changing normal C++ copy/move behavior. [Core math aliases](Math.md) cannot depend on `CoreDObject`, so float vectors, their default double-precision counterparts, `FQuatf`, `FQuat`, `FMatrix4f`, and `FTransform` are registered externally as intrinsic structs and still appear as ordinary `FStructProperty` values. Explicit `FVector*d` and `FQuatd` source spellings use the existing default-double descriptors rather than creating persistent aliases.

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

Reflected base and property type lookup follows a deterministic subset of C++
namespace lookup. An unqualified spelling is tried in the declaring namespace,
then each enclosing namespace, and finally the global namespace. A relatively
qualified spelling follows the same outward walk; an exact exported identity is
also accepted. A spelling beginning with `::` removes that marker and performs
one exact global lookup. Lookup is filtered by the required reflected kind, so
a struct cannot satisfy an object-class reference. DHT never selects an
otherwise unrelated namespace merely because its short name is globally unique.

The declaring namespace is retained recursively for raw object pointers,
`TObjectPtr`, `TSoftObjectPtr`, fixed arrays, `std::vector`, and
`std::unordered_map` key/value properties. Successful lookup is immediately
stored as the fully qualified reflected identity. Failed lookup reports the
source spelling, declaring namespace, allowed kinds, complete lexical candidate
chain, and sorted exported candidates; export-map insertion and worker order do
not affect the result.

When libclang provides a declaration identity, DHT verifies it against the
available reflected exports and required kind. Source spelling remains
authoritative for explicit container shape, intrinsic Durin math aliases, and
target-compiler `sizeof(...)` expressions. A `using` declaration written in the
reflected header can therefore participate through libclang, while an alias or
namespace import supplied only by a stripped ordinary include remains outside
the hermetic boundary and is rejected.

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

## Build Integration

Reflected modules run a public-symbol export command before reflection
generation so dependencies can resolve qualified reflected identities without
reparsing their headers. CMake stamps and byproducts, persistent DHT phase
state, parser-worker scheduling, cache invalidation, output repair, and logging
are build-system responsibilities defined by
[Build System](../../Development/Build/BuildSystem.md). They do not alter the
runtime reflection schema defined below.

`GENERATED_BODY()` identities include source line numbers, so any reflected
header edit remains a direct generation input even when its public export is
byte-for-byte unchanged.

## Symbol Model

The runtime identity for every reflected C++ type is the fully qualified C++ name:

```text
Durin::DObject
Durin::AActor
Durin::DSceneComponent
```

Generated helper names are implementation details. The `namespace-scoped-v2`
scheme places each helper in the reflected type's exact owning namespace and
derives its local spelling only from the reflected kind and short name:

```text
Durin::AActor                 -> ::Durin::Z_Construct_DClass_AActor
Durin::Game_Play::A_Player   -> ::Durin::Game_Play::Z_Construct_DClass_A_Player
F_Global                     -> ::Z_Construct_DStruct_F_Global
```

Namespace boundaries are never encoded into one identifier. Underscores are
therefore ordinary characters in every supported named namespace and type
segment, and formerly colliding spellings such as `A_B::C` and `A::B_C` have
distinct helpers. `_NoRegister`, per-type `_Statics`, and registration-info
entities use the same owning namespace. Generated cross-namespace references
are absolute qualified names.

DHT supports global, named nested, and inline namespaces. It records namespace
segments structurally, including inline status, and reopens them explicitly in
generated headers and sources. Anonymous-namespace and class-nested reflected
types are rejected before output publication because they cannot participate in
the cross-translation-unit free-helper contract.

## Export Files

Each reflected module writes:

```text
<Module>.export
```

The export file uses schema v6 JSON:

```json
{
  "SchemaVersion": 6,
  "Module": "Engine",
  "Symbols": {
    "Durin::AActor": {
      "Kind": "class",
      "ShortName": "AActor",
      "Namespace": "Durin",
      "QualifiedName": "Durin::AActor",
      "Header": "Public/Engine/Actor.h",
      "API": "ENGINE_API",
      "NamespacePath": [
        { "Name": "Durin", "IsInline": false }
      ],
      "BaseQualifiedName": "Durin::DObject"
    }
  }
}
```

Export files are intentionally thin. They persist semantic identity and the
structured namespace path, not a derived helper spelling. All consumers derive
the same local and absolute C++ names through the central generated-symbol
model. Exports are used for symbol resolution during parsing/generation, not as
the full runtime reflection database.

Export files should only change when the exported reflected-symbol contract changes. Whitespace-only edits in a reflected header may force the owning module's export command to run, but should not rewrite the public `.export` file if the symbol index is unchanged. This keeps downstream modules from regenerating purely because an upstream header timestamp changed.

DurinHeaderTool stores export-generation reuse data in:

```text
<Module>/DHTState/export-state.json
```

The export and reflection phase payloads use schema v2 inside a schema-v2
checksummed envelope.
It records the tool and native-libclang fingerprints, parser context, runtime
variant, platform, dependency-export content digests, SHA-256 reflected-header
fingerprints, a serializable raw symbol projection for each header, and the
resolved public-export digest. It lets `generate_module_export_file` skip entirely
when no inputs changed. If only some headers changed, DurinHeaderTool reparses
only those headers and reuses the other raw projections, including an empty
projection for a header that exports no symbols. It then resolves bases against
the complete current-module projection and dependency exports in deterministic
header/name order before writing the unchanged thin public `.export` format.
The reflected-header content identity is SHA-256; timestamp and size are only a
cheap guard that avoids
rehashing an unchanged file. Touching a header therefore refreshes its cached
filesystem metadata without invalidating export or reflection parsing when its
content hash is unchanged. When multiple headers require parsing, the export
generator parses them in a bounded worker pool and merges results in module
header order. Other modules should not depend on or read this private state
directly.

`CoreDObject` uses `DObject/MirrorExportTypes.h` under `_DHT_EXPORTS_PARSER` to publish intrinsic core types such as `Durin::DObject`, `Durin::DType`, `Durin::DStructBase`, and `Durin::DClass` without generating duplicate runtime class registration for those intrinsic types.

## Persistent Generation State

DHT phase bundles are disposable reconstruction data rather than public
reflection inputs. Their schemas, ownership, invalidation, clean/purge behavior,
and interrupted-output recovery are defined by
[Build System](../../Development/Build/BuildSystem.md).

## Generated Header Contract

For each reflected header, DurinHeaderTool writes:

```text
<Header>.gen.h
```

The generated header groups helper and statics declarations into explicit
owning namespace blocks before any `GENERATED_BODY()` expansion. It includes:

- namespace-member construct helper declarations
- a generated statics forward declaration
- `GetPrivateStaticClass()`
- absolute qualified friend and accessor references
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

The generated source defines each per-type helper, statics record, and
registration-info entity inside the same owning namespace. It includes:

- `DObject/GeneratedCppIncludes.h`
- the original reflected header
- namespace-grouped cross-module helper declarations
- `FClassRegistrationInfo`
- `FEnumRegistrationInfo`
- `T::GetPrivateStaticClass()`
- no-register and full construct helpers
- generated statics containing `FClassParams`
- generated enum value tables and `FEnumParams`
- generated property parameter records
- compiled-in registration records
- generated object-initializer constructor definitions when needed

Generated code uses fully qualified C++ type names and absolute qualified
cross-namespace helper references. File-only compiled-in registration
aggregates have internal linkage and point at the qualified per-type symbols.
Intrinsic `DObject` and math-struct helpers follow the same contract.

The v2 helper ABI is an atomic binary migration. Outputs, export schema, parser
context, phase state, dependency snapshots, tool identity, and generator
context are versioned together. Schema-v5 exports and schema-v1 phase bundles
are rejected or invalidated and regenerated; no forwarding wrappers preserve
the former flattened global symbols. All dependent modules must regenerate and
relink together. Runtime `QualifiedName`, `/Cpp/<Module>` ownership, serialized
type names, and legacy-name behavior are unchanged.

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
- optional `LegacyNames = "Former::QualifiedName;Older::QualifiedName"` on `DCLASS`,
  `DSTRUCT`, and `DENUM` for read-only compatibility with serialized identities
  emitted before a qualified C++ namespace move; generated and runtime identity
  always follow the declaration's current namespace, and recognized legacy
  names are canonicalized at the asset read boundary
- property parameter array
- property count

`DClass` keeps runtime identity, C++ spelling, editor presentation, and instance naming separate. `QualifiedName` is the current serialized/type-lookup identity and `ShortName` remains the C++ class spelling. Legacy aliases are accepted only by serialized-name lookup and never by ordinary qualified-name lookup or new saves. `DisplayName` is used by editor UI, while `DefaultObjectName` is used when an instance is created without an explicit name. When metadata is omitted, Durin removes the conventional `A`/`D` prefix when followed by an uppercase letter; display names additionally split CamelCase words. For example, `AStaticMeshActor` defaults to display name `Static Mesh Actor` and object name `StaticMeshActor`.

After reflection registration, `CaptureSerializedReflectionAliases()` freezes a
deterministically ordered, value-only catalog of every class, struct, and enum
alias and its current identity. Package workers consume the copied catalog;
they never consult or mutate the live qualified-name maps. A registered alias
is compatible canonical-resave debt, while an identity absent from both current
metadata and this catalog remains an unavailable-type compatibility failure.
Aliases may be removed only after every supported authored-content baseline has
zero findings, canonical-resave CI passes, authored diffs have been reviewed,
and the compatibility policy for external content has been recorded separately.
The current repository baseline has completed that process for retired
identities, so their concrete `LegacyNames` registrations are absent. The
generic mechanism and owner-scoped alias tests remain available for future
bounded migrations.

`ConstructDClass(...)` forces class registration, then creates `FProperty` nodes from generated property parameters and attaches top-level fields to `DStructBase::ChildProperties`. Container inner/key/value properties are constructed recursively and owned by their containing `FArrayProperty` or `FMapProperty`; they are not inserted into the class property chain.

### Class Default Objects

Every eligible reflected class owns one immutable class default object after its
registration batch completes. Eligibility requires a valid constructor and
layout and excludes `DCLASS(Abstract)`, intrinsic reflection metadata, and the
explicit `DCLASS(NoClassDefaultObject)` service/infrastructure opt-out.
`DClass::GetDefaultObject()` is const-only and reports a stable state/reason when
no default is available.

Registration batches expand superclass chains and construct base-before-derived,
then qualified-name order. Defaults are not published until the whole batch
succeeds. A default has Outer equal to its `DClass`, a
`Default__<ShortClassName>` name, and `ClassDefaultObject | Transient` flags.
The four authored actor defaults may create their fixed-name component templates
with `DefaultSubobject | Transient`; arbitrary archetype graphs are not supported.

Class defaults are created eagerly and published atomically after reflection
registration. `DClass` records a stable ready/unavailable/failed/released state
and reason; callers never lazily construct a default through a const read. The
default and a live instance form a bounded graph by Outer-relative
`(class, name)` identity. `FDefaultObjectGraphMap` pairs the root and every
required default subobject, rejects missing, extra, duplicate, mis-parented, or
class-mismatched nodes, and provides graph-relative hard-reference identity.
Object fields use only this most-derived class-default graph as their baseline.

`EObjectConstructionPurpose` distinguishes runtime, asset-load, duplication,
class-default, and default-subobject construction. Constructors use it to retain
authored values while skipping runtime publication or activation. Broad object
queries must explicitly choose `EObjectQueryScope::LiveOnly` or
`IncludeTemplates`; asset, world, editor, and loaded-instance queries use the
former, while reflection diagnostics and lifecycle code use the latter.

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
Archive, Engine, and GC still use `IsObjectPtrWrapper()` to preserve the
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

Transactional struct loading also supports an accessor-backed struct field.
It constructs a temporary offset-zero property using the same `DStruct`, loads
and repairs that detached value, then copy-assigns through the original field's
accessor. This keeps matrix-column loading atomic without pretending that a GLM
column has a portable byte offset.

`DurinCodeGen::ConstructDEnum(...)` forces enum registration for generated
`DEnum` singletons. `DEnum` stores qualified name, short name, display name,
scoped flag, underlying kind/size, and a read-only value table. Generated UTF-8
pointers are copied into process-lifetime runtime strings during construction.

`DStructBase` stores its superclass through `SuperStructBase`. `DClass::GetSuperClass()` exposes this as a `DClass*`.

`DObject::IsA(const DClass*)` walks the `DClass` superclass chain. `Cast<T>` uses `T::StaticClass()` and `IsA`.

## Byte And Blob Properties

`std::byte` is a distinct reflected one-byte leaf. It is not numeric: generic
property editing presents hexadecimal byte data and does not apply arithmetic,
range, or decimal controls. Fixed C arrays of `std::byte` retain fixed-array
shape.

A direct `FByteBuffer` is generated as one owned Blob property with logical
Archive kind `Bytes`; it is not an `FArrayProperty` and exposes only a read-only
byte-count summary in generic Details. `std::vector<std::byte>` is the private
implementation of `FByteBuffer`, not a second reflected spelling. Blob
construction, destruction, copy, exact equality, snapshots, duplication, and
Archive loading operate on the complete array. Blob values contain no object
references and cannot be Map keys or targets of indexed authored overrides.

`std::vector<uint8>` remains the existing numeric `Array<UInt8>` contract.
DurinHeaderTool does not infer Blob intent from a field name or size. The first
Blob contract admits only a direct field: nested Blob containers, custom
allocators, and Blob Map participation are rejected during generation.
`FByteView` and `FMutableByteView` are borrowed API types and are not reflected
properties; reflected byte storage must use `FByteBuffer`.

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
Current non-mechanical specializations provide deterministic mathematical
defaults for vector, quaternion, matrix, and color structs. The former mutable
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

Eligible `DStruct` values also publish one immutable deterministic type default
during the registration batch. Eligibility requires complete authored fields,
deterministic default construction and destruction, and no custom Archive
serializer; unavailable structs retain a stable reason rather than a partial
value. Publication is atomic, aligned storage is destroyed exactly once, hidden
strong references are rooted through the struct's collection contract, and
module teardown releases defaults before their callbacks can unload. Explicit
Struct values compare against this type default, never against nested memory in
a containing class default.

`ComparePropertyValues(...)` returns `Identical`, `Different`, or `Unsupported`
with a stable logical path, kind, and reason. Unsupported operations are not an
ordinary difference and cannot authorize omission. The comparison grammar is
recursive and bounded to depth 64: fixed arrays and Arrays are positional, Maps
use canonical logical key tokens and compare key/value associations independent
of iteration order, hard references may use a supplied default-object graph,
soft references compare canonical paths, and floating-point values compare
their complete bits. Descriptor cycles, missing container operations,
incomplete/custom Struct semantics, duplicate Map keys, excessive counts, and
malformed values fail with a typed diagnostic.

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

## Serialization Integration

Reflection supplies stable type, field, container, flag, and logical-value
metadata to object serialization, duplication, property snapshots, and authored
packages. Archive capabilities, scopes, object graphs, default-relative
planning, authored override intent, and transactional failure behavior are
defined by [Serialization](Serialization.md).

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

### Typed authoring metadata

First-party property presentation and numeric authoring constraints are written
as ordinary `DPROPERTY` specifiers:

```cpp
DPROPERTY(Edit, DisplayName = "Exposure", ToolTip = "Camera exposure bias.",
    Category = "Camera", Units = "Unitless", Step = "0.1", Precision = 2,
    ClampMin = "-10", ClampMax = "10", UIMin = "-5", UIMax = "5")
float Exposure = 0.0f;
```

`DisplayName`, `ToolTip`, and `Category` accept non-empty quoted strings on a
reflected direct or fixed-array field. `Units`, `Step`, `Precision`,
`ClampMin`, `ClampMax`, `UIMin`, and `UIMax` require `Edit` and apply to scalar
integer/float properties and the components of the intrinsic float/double
vectors and quaternions. They do not apply to bool, string, enum, object,
container, matrix, or arbitrary struct properties. Numeric metadata on a
containing property never implicitly flows into Array/Map elements or nested
struct fields.

`Units` accepts `Unitless`, `Percent`, `Degrees`, `Radians`, `Seconds`,
`Milliseconds`, `Meters`, `Centimeters`, `Millimeters`, or `Kilometers`.
`Step` must be positive and finite. `Precision` is limited to 0..9 for float
values and 0..17 for double values. UI bounds must be ordered and remain inside
hard bounds when both are present. A decimal float spelling is converted once
to its declared float or double channel; integer spellings must be integral and
fit the exact declared width. Consequently full-range `uint64` metadata never
passes through `double`.

DHT rejects duplicate selected keys, malformed or non-finite numbers, invalid
units, inapplicable keys, and inconsistent ranges with a source-located
`DHT-META` diagnostic. A selected typed key may not also be supplied through
`MetaData`; unrelated extension metadata remains supported. Generated
`FPropertyMetadataParams` records keep signed, unsigned, float, and double
numbers in distinct channels. Registration validates hand-authored records and
copies them into immutable `FPropertyMetadata`. The three presentation strings
are also mirrored into raw metadata lookup for compatibility, while first-party
consumers use `GetTypedMetadata()`.

`ClampMin` and `ClampMax` constrain new editor-authored proposals through
`ValidatePropertyEditValue()`. Validation happens on detached draft
storage before an edit session or transaction mutates the object. Package
loading deliberately does not call this property-edit validator and therefore does
not clamp or repair historical data. `UIMin` and `UIMax` only configure editor
presentation.

`FProperty::ContainerPtrToValuePtr<T>(...)` and `GetValuePtr(...)` provide field address access from an owning object/container address. `FObjectProperty::GetObjectPropertyValue(...)` and `SetObjectPropertyValue(...)` provide direct hard-object-reference access for GC and serialization. `FSoftObjectProperty::GetSoftObjectPtr(...)` exposes the bounded untyped `FSoftObjectPtr` reflection boundary; authored identity is read and written through `GetPath()` and `SetPath(FObjectPath)`, while typed application code uses `TSoftObjectPtr<T>`. `FWeakObjectProperty::GetWeakObjectPtr(...)` exposes its exact wrapper and expected class. `FStringProperty` exposes a `std::string*` pointer helper. Array and Map properties expose the capability-checked container operations described below. Map operations expose mutable mapped values while keeping keys immutable in place; key edits use copy, uniqueness validation, and node-based rename operations so hashing and equality invariants remain intact.

DurinHeaderTool recognizes direct and fixed-array `TSoftObjectPtr<T>` fields and
soft values nested through supported Array, Map-value, and reflected-struct
paths. `T` must resolve to a reflected `DObject` subclass. Raw wrapper aliases,
unresolved or non-object targets, soft Map keys, and unsupported qualifiers are
rejected rather than emitted with incomplete metadata. `FSoftObjectProperty`
is a distinct property kind: serialization and editor access use its exact
authored `FObjectPath`, while GC deliberately excludes it from the strong-reference
schema. DHT also recognizes typed `TWeakObjectPtr<T>` in direct, fixed-array,
Array, Map-value, and reflected-Struct positions. The top-level property must
be explicitly `Transient`, and weak Map keys are invalid. Generated weak
properties retain exact wrapper lifecycle operations and the expected class;
they are non-owning runtime state and never acquire a soft path identity.

Generated leaf records do not emit element-size expressions. Plain aliases own
their exact C++ value type, Enum records resolve their underlying type, and
Object/SoftObject factories instantiate their declared storage type. Reflected
struct properties likewise emit no element-size expression because their
resolved `DStruct` owns size and alignment. This keeps ABI ownership with the
compiler-built descriptor and prevents synthetic parser declarations or host
STL contents from changing runtime property sizes.

`DStructBase::ChildProperties` stores only properties declared directly on that reflected type. Superclass properties are reached through the superclass chain. Use `FindPropertyByName(...)` or the property iteration helpers when inherited properties should be visible.

## Serialized Property Aliases

A durable reflected field may declare read-only compatibility names when its C++
member is renamed:

```cpp
DPROPERTY(LegacyNames = "OldExposure;ExposureValue")
float ExposureEV = 0.0f;
```

DHT requires an explicitly quoted semicolon-separated list of unqualified C++
identifiers. Within one declaring class or struct, an alias must differ from its
property's current name and must not collide with any current property name or
another property alias. Generated property descriptors carry the aliases as
first-class compatibility data, and runtime registration repeats the collision
checks for manually authored descriptors.

`FindPropertyByName(...)` remains current-name-only. Serialized readers use
`FindPropertyBySerializedName(...)`, scoped to the field's declaring class or
struct, and canonicalize a recognized alias to the property's current name at
the bytes-to-runtime boundary. Type signatures are still checked independently;
an alias permits a rename only and does not make a type change compatible.
Canonical saves, authored-override paths, compatibility catalogs, and reference
routes use the current property name. If one stored schema contains both a
current name and an alias that resolve to the same property, the schema is
rejected as ambiguous rather than applying two values in record order. Moving a
field between declaring types is not covered by property aliases.

## Deprecated Properties

Use a deprecated property when a stored field cannot be handled by a
`LegacyNames` rename because its logical type or meaning changed:

```cpp
DPROPERTY()
float Distance = 0.0f;

DPROPERTY(Deprecated)
int32 Distance_DEPRECATED = 0;
```

`Deprecated` is explicit; the `_DEPRECATED` suffix alone has no special
behavior. An explicitly deprecated field must use that suffix, cannot be
`Edit` or `Transient`. The historical stored name is inferred by removing the
suffix; deprecated-property annotations do not accept migration targets,
custom-version domains, or version bounds.

Routing is exact on declaring type, inferred historical name, and logical type
signature. It happens before a reused current name is considered, so an old
integer `Distance` loads into `Distance_DEPRECATED` while the current float
`Distance` remains untouched. The owning class performs semantic conversion in
`PostLoad`; reflected structs do so in `PostDeserialize`. Those callbacks can
use `WasDeprecatedPropertyLoaded` to distinguish a consumed old route from an
unloaded deprecated member holding its C++ default. Any rejection rolls back
the complete load.

If the stored and current fields have the same name and logical type, keep one
current property and perform any version-dependent in-place conversion in the
owning callback. A deprecated route is for a historical schema shape that can
be distinguished by reflection, not for every semantic version change.

Deprecated fields are load-only: current Archive saves, default deltas,
canonical schemas, Details panels, and current authored ledgers exclude them.
Migration code owns both value conversion and any authored-intent transfer. It
can call `SetAuthoredOverride` for each current path whose explicit or forced
state must survive the resave; splits, merges, and conditional targets therefore
remain ordinary migration logic rather than reflection metadata. Compatibility
and canonical-resave reports expose each consumed route as remaining migration
debt. A route should be removed only after the supported content baseline
contains no matching evidence and the normal compatibility-policy gate has
approved retirement.

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
logically unchanged. GC, runtime Archive, snapshots, Engine, and editor
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

## Verification

DurinHeaderTool parser and generator tests cover extraction, namespace lookup,
export identity, generated output, and recovery behavior. Native CoreDObject and
asset tests cover runtime metadata, properties, serialization, duplication, and
container behavior. Test selection and execution follow
[Native C++ Tests](../../Development/Build/NativeTests.md) and
[Build and Run](../../Development/Build/BuildAndRun.md).
