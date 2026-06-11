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
- generated primitive property metadata for reflected `DPROPERTY()` fields
- runtime `DObject::IsA` and `Cast<T>` based on the `DClass` hierarchy

The system does not currently implement GC, package/CDO behavior, hot reload, editor property panels, serialization, reflected containers, function reflection, or complete metadata specifier parsing.

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
  "SchemaVersion": 2,
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
- `T::GetPrivateStaticClass()`
- no-register and full construct helpers
- generated statics containing `FClassParams`
- generated property parameter records
- compiled-in registration records
- generated object-initializer constructor definitions when needed

Generated code uses fully qualified C++ type names for reflected C++ types.

## Runtime Type Data

`DurinCodeGen::FClassParams` passes generated class metadata into `DurinCodeGen::ConstructDClass(...)`:

- no-register class function
- qualified class name
- short class name
- property parameter array
- property count

`ConstructDClass(...)` forces class registration, then creates `FProperty` nodes from generated property parameters and attaches them to `DStructBase::ChildProperties`.

`DStructBase` stores its superclass through `SuperStructBase`. `DClass::GetSuperClass()` exposes this as a `DClass*`.

`DObject::IsA(const DClass*)` walks the `DClass` superclass chain. `Cast<T>` uses `T::StaticClass()` and `IsA`.

## Property Metadata

The current generated property metadata supports these primitive kinds:

- `int8`, `int16`, `int32`, `int64`
- `uint8`, `uint16`, `uint32`, `uint64`
- `float`
- `double`
- `bool`

The runtime property node stores:

- name
- flags
- array dimension
- byte offset
- generated property kind
- referenced class for object-pointer properties

`FString`, enum values, and object-pointer properties have schema/runtime slots, but should be treated as still needing broader coverage and tests before use in production workflows.

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

When adding new reflection behavior, validate both the DHT tests and a real C++ build. The generated files are part of the compile surface, and macro/friend/access errors often appear only during C++ compilation.
