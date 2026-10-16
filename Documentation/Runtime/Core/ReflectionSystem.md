# Reflection System

Summary: Define runtime type identity, registration, class defaults, enum metadata, and reflection contract ownership.

Modules: CoreDObject

Last reviewed: 2026-10-16

## Overview

DurinHeaderTool extracts selected C++ headers listed in a module's `.dmodule`
`ReflectHeaders` field. Generated registration publishes qualified type identities
and immutable runtime descriptors: `DClass` for `DCLASS`, `DStruct` for `DSTRUCT`,
and `DEnum` for `DENUM`. Classes and structs use `GENERATED_BODY()`.

The contracts are owned by the following documents:

| Task | Owning document |
| --- | --- |
| Header parsing, symbol exports, generated files, and build integration | [Reflection generation](../../Development/Tooling/ReflectionGeneration.md) |
| Property types, metadata, registration, containers, and field compatibility | [Property reflection](PropertyReflection.md) |
| Struct lifecycle capabilities, detached values, comparison, and type defaults | [Reflected struct operations](StructOperations.md) |
| `DFUNCTION` signatures, parameter storage, inheritance, and invocation | [Native function reflection](FunctionReflection.md) |
| Object lifetime, reference schemas, and collection | [Garbage collection](GarbageCollection.md) |
| Archives, duplication, defaults, and authored override intent | [Serialization](Serialization.md) |
| Details editing and transactions | [Reflected property editing](../../Editor/Architecture/ReflectedPropertyEditing.md) |

`DSTRUCT()` preserves normal C++ copy/move behavior. [Core math aliases](Math.md)
cannot depend on `CoreDObject`; their externally registered intrinsic structs
still appear as ordinary `FStructProperty` values. Explicit `FVector*d` and
`FQuatd` spellings resolve to the default-double descriptors.

Runtime descriptors are immutable after registration; hot reload and runtime
property mutation are unsupported. Class defaults are immutable and do not
provide general archetype chains. Serialization compatibility uses the explicit
alias and deprecated-field contracts rather than an automatic schema migration.

## Runtime Type Data

`DurinCodeGen::ConstructDClass(...)` consumes `FClassParams` with the class
resolver, qualified and short names, presentation and default-object names,
serialized legacy names, property and function descriptors, and the class-owned
Cook schema hook. The generated C++ contract is defined by
[Reflection generation](../../Development/Tooling/ReflectionGeneration.md#generated-source-contract).

Type declarations may author `LegacyNames = "Former::QualifiedName;Older::QualifiedName"`
on `DCLASS`, `DSTRUCT`, and `DENUM` for serialized identities predating a C++
namespace move. Runtime and newly serialized identities always follow the current
namespace. `DCLASS(DisplayName = "...")` and `DCLASS(DefaultObjectName = "...")`
separately control editor labels and unnamed instance naming.

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

`ConstructDClass(...)` forces class registration, then uses the shared `ConstructProperties(...)` entry to create and attach owned property trees in declaration order. `AddOwnedProperty(...)` installs ownership and calls `AddCppProperty(...)` to attach the root. Container inner/key/value properties are constructed recursively and owned by their containing `FArrayProperty` or `FMapProperty`; they are not inserted into the class property chain.

`DStructBase` stores its superclass through `SuperStructBase`.
`DClass::GetSuperClass()` exposes this as a `DClass*`. `DObject::IsA(...)` walks
that hierarchy; `Cast<T>` uses `T::StaticClass()` and `IsA`.

## Class Default Objects

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
Authored actor defaults may create their fixed-name component templates
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

## Enum Metadata

`DurinCodeGen::ConstructDEnum(...)` publishes generated `DEnum` singletons.
Generated UTF-8 names are copied into process-lifetime runtime strings.

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
the first declaration when aliases share a numeric value.

Enum metadata and reflective access use a canonical `uint64` value channel. Signed
values are sign-extended into that channel, while unsigned values preserve their
full range. `FEnumProperty` still reads and writes the enum's declared underlying
width, so reflected objects retain their native layout and serialization size.
`DisplayName` is presentation-only: it is never a lookup identity and is not
serialized. `FEnumValue::Name` and `Value` remain the stable reflection and
persistence identities. Numeric values missing from the reflected table remain
invalid records and editor code displays their numeric representation.

Enum properties use `FEnumProperty`. The property still stores the ordinary property metadata such as name, offset, array dimension, and size, and additionally references the generated `DEnum` singleton for the enum type.

## Related Documentation

- [Runtime lifecycle](RuntimeLifecycle.md)
- [Editor function actions](../../Editor/Architecture/EditorFunctionActions.md)
