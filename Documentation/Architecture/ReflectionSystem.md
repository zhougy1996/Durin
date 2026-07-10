# Generated Reflection System

This document describes the reflection framework that is currently implemented in Durin. For future work and deferred milestones, see `ReflectionRoadmap.md`.

## Overview

Durin reflection uses selected C++ headers as the source of truth. A module lists reflected headers in its `.dmodule` file under `ReflectHeaders`. During configure/build, DurinHeaderTool scans those headers and writes generated files under:

```text
Engine/Intermediate/Build/<Platform>/<Profile>/<Module>/DHT
```

The current system supports:

- `DCLASS()` classes with `GENERATED_BODY()`
- fully qualified runtime type identities, such as `Durin::AActor`
- namespace-safe generated helper names, such as `Z_Construct_DClass_Durin_AActor`
- module export files as thin reflected-symbol indexes
- module manifests for incremental reflection generation
- generated `.gen.h` and `.gen.cpp` files
- generated class registration into `DClass`
- generated enum registration into `DEnum`
- generated property metadata for reflected `DPROPERTY()` fields
- primitive, `std::string`, enum, object-pointer, fixed C array, `std::vector`, and `std::unordered_map` property nodes
- nested container property metadata, with recursive array/map inner property trees
- runtime `DObject::IsA` and `Cast<T>` based on the `DClass` hierarchy
- runtime `DObject` registration in `GDObjectArray`
- manual object destruction and minimal mark-sweep garbage collection
- reflected scalar/string/object-reference serialization through `FArchive`
- minimal in-memory object graph save/load helpers

The system does not currently implement package/CDO behavior, hot reload, editor property panels, function reflection, general template reflection, type-erased container mutation, long-term asset/package serialization formats, version migration, weak references, incremental/concurrent GC, or complete metadata specifier parsing.

## Build Flow

The build flow is:

```text
.dmodule and profile config
-> generated module CMake metadata
-> module export file
-> module reflection files and manifest
-> C++ compile/link
```

`add_durin_module(...)` wires two DurinHeaderTool commands for reflected modules:

- `generate_module_export_file`
- `generate_reflection_files`

The export command runs before reflection generation so other modules can resolve reflected base classes and object-pointer property types without reparsing dependency headers.

Reflection generation also depends directly on the module's reflected headers. A whitespace-only header edit may leave the public `.export` symbol index unchanged, but it still must regenerate that header's `.gen.h/.gen.cpp` because `GENERATED_BODY()` macro names include source line numbers.

DurinHeaderTool logs key build progress at `INFO` level:

- export skip status, scan start/end, and symbol count
- per-header export parse time and symbol count
- export parse worker count when multiple headers require parsing
- reflection manifest preparation
- number of regenerated/skipped headers
- reflection parse worker count when multiple headers require regeneration
- dependency export loading
- per-header parse/write time, class count, and property count
- module reflection completion time

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

The export file is schema v1 JSON:

```json
{
  "SchemaVersion": 3,
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

The module export manifest records the schema/tool/options/profile/platform and reflected-header fingerprints. It lets `generate_module_export_file` skip entirely when no inputs changed. If only some headers changed, DurinHeaderTool reparses those headers and reuses unchanged header symbols by grouping entries from the previous public `.export` file, then assembles the new public `.export` file. When multiple headers require parsing, the export generator parses them in a bounded worker pool and merges results in module header order. Other modules should not depend on or read this private cache file.

`CoreDObject` uses `DObject/MirrorExportTypes.h` under `_DHT_EXPORTS_PARSER` to publish intrinsic core types such as `Durin::DObject`, `Durin::DType`, `Durin::DStructBase`, and `Durin::DClass` without generating duplicate runtime class registration for those intrinsic types.

## Manifest Files

Each reflected module writes:

```text
<Module>.manifest
```

The manifest is schema v1 JSON and is private to DurinHeaderTool. It records:

- `SchemaVersion`
- `ToolVersion`
- `SymbolNameScheme`
- `ModuleName`
- `Profile`
- `Platform`
- `GeneratorOptionsHash`
- reflected header fingerprints
- dependency export fingerprints
- resolved reflected-symbol dependencies per header

Changing the tool version, schema, symbol-name scheme, profile, platform, options hash, dependency exports, or reflected header fingerprints invalidates generated reflection outputs.

Dependency export changes are filtered through resolved symbol dependencies. If an upstream export changes but a header does not reference the changed reflected symbols, that header can keep its existing generated files. Missing generated outputs still force regeneration for the affected header.

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

`DENUM()` declarations are explicit reflected enum opt-ins. DurinHeaderTool exports them as `Kind: "enum"` symbols, records scoped/unscoped form and underlying type metadata, and emits `Z_Construct_DEnum_*` helpers plus generated value tables. Reflected enum fields also generate `FEnumProperty` metadata that points at the corresponding `DEnum`.

## Runtime Type Data

`DurinCodeGen::FClassParams` passes generated class metadata into `DurinCodeGen::ConstructDClass(...)`:

- no-register class function
- qualified class name
- short class name
- property parameter array
- property count

`ConstructDClass(...)` forces class registration, then creates `FProperty` nodes from generated property parameters and attaches top-level fields to `DStructBase::ChildProperties`. Container inner/key/value properties are constructed recursively and owned by their containing `FArrayProperty` or `FMapProperty`; they are not inserted into the class property chain.

`DurinCodeGen::ConstructDEnum(...)` forces enum registration for generated `DEnum` singletons. `DEnum` stores qualified name, short name, scoped flag, underlying kind/size, and a read-only name/value table.

`DStructBase` stores its superclass through `SuperStructBase`. `DClass::GetSuperClass()` exposes this as a `DClass*`.

`DObject::IsA(const DClass*)` walks the `DClass` superclass chain. `Cast<T>` uses `T::StaticClass()` and `IsA`.

## Object Lifetime And GC

`NewObject<T>(Outer, Name)` constructs reflected runtime objects through `StaticConstructObject(...)`, assigns their class/name/outer data, attaches them to their outer's inner-object list when an outer is present, and registers them in `GDObjectArray`.

`GDObjectArray` is the runtime object registry. It supports adding objects, removing destroyed objects by nulling their slots, taking traversal snapshots, and compacting null slots after destruction or GC.

The current lifecycle API is intentionally small:

- `DestroyObject(DObject*)` destroys non-intrinsic runtime objects and their inner objects
- `AddToRoot(DObject*)` and `RemoveFromRoot(DObject*)` control manual GC roots
- `FScopedObjectRoot` temporarily roots an object for a C++ scope
- `CollectGarbage()` performs synchronous mark-sweep collection
- `DObject::BeginDestroy()` and `DObject::FinishDestroy()` are lifecycle hooks for derived classes

Reflection metadata drives reference traversal. `ForEachObjectReference(...)` scans reflected direct `FObjectProperty` fields, including `TObjectPtr` wrappers, and calls an `FReferenceCollector`. GC marks from permanent reflected type objects, root-set objects, reflected object references, and outer-to-inner ownership. Intrinsic reflected metadata objects such as `DClass` and `DEnum` are treated as permanent and are not swept.

This is a stop-the-world v1 collector. It does not scan the native stack, traverse container elements, support weak references, defer destruction, compact object handles, or run incrementally.

## Serialization

`DObject` has a virtual `Serialize(FArchive& Ar)` hook. The default implementation calls `SerializeDObjectProperties(...)`, which walks reflected properties and serializes supported non-`Transient` fields.

The current archive layer includes:

- `FArchive` with load/save mode, byte serialization, string serialization, and object-reference serialization
- `FMemoryWriter` and `FMemoryReader`
- `SaveObjectGraphToMemory(DObject* RootObject, std::vector<uint8>& OutBytes)`
- `LoadObjectGraphFromMemory(const std::vector<uint8>& Bytes)`

The supported reflected property payloads are numeric primitives, `bool`, `std::string`, reflected enum storage, direct object references, vectors, maps, and recursively nested supported containers. Object references are serialized as object ids inside the saved object graph, not as process pointer addresses.

Object graph saving first gathers reachable objects from the root using outer/inner ownership and reflected direct object references, assigns ids, writes object records, and serializes each object's reflected properties. Loading creates all object records first, then deserializes properties so object-reference ids can resolve to loaded objects.

The object graph format is an internal v1 binary memory format for tests and engine plumbing. Long-lived content uses the separate field-tagged `.dasset` package format documented in `Documentation/Architecture/AssetPackages.md`; the memory format remains useful for transient cloning and focused tests.

## Enum Metadata

`DENUM()` supports scoped and unscoped C++ enums. The generated `DEnum` metadata stores:

- qualified enum name
- short enum name
- scoped/unscoped form
- underlying numeric kind and size
- read-only name/value table

Enum properties use `FEnumProperty`. The property still stores the ordinary property metadata such as name, offset, array dimension, and size, and additionally references the generated `DEnum` singleton for the enum type.

## Property Metadata

The current generated property metadata supports these scalar kinds:

- `int8`, `int16`, `int32`, `int64`
- `uint8`, `uint16`, `uint32`, `uint64`
- `float`
- `double`
- `bool`
- `std::string`
- reflected enum values
- reflected `DObject*` pointers

The runtime property node stores:

- name
- flags
- array dimension
- byte offset
- element size
- generated property kind
- referenced class for object-pointer properties
- referenced enum for enum properties
- owner field/property for nested property trees

Concrete property types live in `DObject/DurinPropertyTypes.h`. `DObject/Property.h` keeps the common `FProperty` base and value-pointer helpers.

Supported property node types are:

- `FNumericProperty`
- `FBoolProperty`
- `FStringProperty` for `std::string`
- `FEnumProperty`
- `FObjectProperty`
- `FArrayProperty`
- `FMapProperty`

`FProperty::ContainerPtrToValuePtr<T>(...)` and `GetValuePtr(...)` provide field address access from an owning object/container address. `FObjectProperty::GetObjectPropertyValue(...)` and `SetObjectPropertyValue(...)` provide direct object-reference access for GC and serialization. `FStringProperty` exposes a `std::string*` pointer helper. Generated array and map helpers provide type-erased traversal and mutation used by GC and both memory/package serialization.

`DStructBase::ChildProperties` stores only properties declared directly on that reflected type. Superclass properties are reached through the superclass chain. Use `FindPropertyByName(...)` or the property iteration helpers when inherited properties should be visible.

## Container Properties

DurinHeaderTool currently recognizes these standard library container spellings for `DPROPERTY()` fields:

- `std::vector<T>`
- `std::unordered_map<K, V>`

`std::vector<T>` creates an `FArrayProperty` whose `Inner` points at the generated property metadata for `T`.

`std::unordered_map<K, V>` creates an `FMapProperty` whose `KeyProp` and `ValueProp` describe the key and value types.

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

Only the top-level field uses `STRUCT_OFFSET`. Nested inner/key/value properties use offset `0` because they describe an element type, not a field within the reflected class.

Container key restrictions are intentionally stricter than value restrictions. Map keys may be primitive, `bool`, `std::string`, or reflected enum types. Map keys may not be `DObject*` or another container. Vector inner types and map values may be primitive, `bool`, `std::string`, reflected enum, reflected `DObject*`, or another supported container within the depth limit.

Unsupported types are skipped by DHT with stable diagnostics/test behavior. This includes unknown templates, smart pointers, non-reflected object pointers, references, and containers nested deeper than the configured limit.

Runtime nested metadata can be walked with `ForEachNestedProperty(...)`. The helper visits array inner properties and map key/value properties recursively. Nested properties also expose their containing property through `FProperty::GetOwnerProperty()`.

## Testing And Verification

The current focused DHT tests live under:

```text
Engine/Source/Programs/DurinHeaderTool/tests
```

The implementation keeps the external entrypoint at `Engine/Source/Programs/DurinHeaderTool/main.py`.
Internal code lives under `Engine/Source/Programs/DurinHeaderTool/durin_header_tool`, split by role:
`cli`, `config`, `io`, `model`, `parser`, `resolver`, `cache`, `writers`, and `generators`.

Run them with:

```powershell
python -m unittest discover -s Engine\Source\Programs\DurinHeaderTool\tests -p "test_*.py"
```

For C++ verification, build a representative editor chain:

```powershell
cmake --build Build/Win64-Debug-DurinEditor --target CoreDObject Engine LevelEditor DurinLauncher -j 18
```

Run the focused CoreDObject test executable after lifecycle, GC, or serialization changes:

```powershell
.\Engine\Binaries\Win64\Debug\Tests\DurinEditor\Bin\CoreDObjectTests.exe
```

When adding new reflection behavior, validate both the DHT tests and a real C++ build. The generated files are part of the compile surface, and macro/friend/access errors often appear only during C++ compilation.
