# Generated Reflection Roadmap

This document describes the intended design for Durin's generated-code reflection system, the build dependency flow, and the staged path toward a small usable type system and garbage collector.

## Goals

Durin uses C++ headers as the source of truth for reflected runtime types. DurinHeaderTool scans selected headers, emits generated C++ files, and produces compact metadata files that let modules resolve reflected symbols across module boundaries.

The first usable milestone is intentionally smaller than Unreal's full UObject system:

- reflected classes declared with `DCLASS()`
- generated `GENERATED_BODY()` declarations
- stable `StaticClass()` and `GetClass()` behavior
- reflected base-class relationships
- reflected primitive and object-pointer properties
- runtime property chains attached to `DClass`
- enough metadata for later reference traversal and GC

The first milestone should not include a full editor property system, asset package system, Blueprint-like function invocation, hot reload, or a complete garbage collector.

## Core Rules

The runtime identity of every reflected C++ symbol is its fully qualified C++ name. Short names are display conveniences only.

Examples:

- `Durin::DObject`
- `Durin::AActor`
- `Durin::Components::DSceneComponent`

Generated C++ may use helper names, but all type references should be generated with fully qualified names when possible. This avoids ambiguity when reflected types live inside namespaces.

Generated helper symbols are private implementation details. They should favor readable names derived from the fully qualified C++ name, with namespace separators replaced by `_`.

For example:

```cpp
// Durin::Gameplay::AActor
DClass* Z_Construct_DClass_Durin_Gameplay_AActor();
```

This is not the runtime identity of the type. The runtime identity and export-file key remain the fully qualified C++ name, such as `Durin::Gameplay::AActor`.

To keep this helper-name scheme readable and reversible, reflected namespace segments and reflected type names must not contain `_`. DurinHeaderTool should validate this while collecting reflected symbols and fail early with a clear diagnostic. For example, `Durin::Gameplay::AActor` may generate `Durin_Gameplay_AActor`, but `Durin::Gameplay_AActor` should be rejected before code generation.

This naming restriction only applies to reflected symbol path segments that participate in generated helper names. It does not need to apply globally to every C++ helper, local variable, private non-reflected type, or third-party symbol.

## Build Flow

The high-level dependency order is:

```text
.dmodule and project config
-> generated module CMake metadata
-> module export file
-> module manifest and reflection-generated files
-> C++ compile and link
```

`add_durin_project(...)` prepares project and module build metadata under:

```text
Engine/Intermediate/Build/<Platform>/<Profile>/...
```

`add_durin_module(...)` then imports the generated module CMake file and wires:

- original public and private source files
- reflection headers from `ReflectHeaders`
- module export file generation
- reflection `.gen.h` and `.gen.cpp` generation
- generated source files into the module target

This shape is correct for namespace-aware reflection. Reflection generation must happen after export files exist, because a header often cannot resolve base classes, enum underlying types, or object-pointer property types from the current header alone.

## Export Files

An export file is a thin symbol index for other modules. It should not become the complete reflection database.

Export files should answer questions needed during parsing and generation:

- Which reflected symbols does this module publish?
- What is each symbol's fully qualified name?
- What generated helper name should other modules reference?
- What kind of symbol is it: class, struct, enum, or function later?
- Which module and header own the symbol?
- Which API macro exports the symbol?
- For enums, what is the scoped/unscoped form and underlying type?
- For classes, what is the reflected base type when known?
- Did every reflected namespace and type-name segment pass generated-name validation?

Recommended shape:

```json
{
  "SchemaVersion": 2,
  "Module": "Engine",
  "Symbols": {
    "Durin::AActor": {
      "Kind": "class",
      "ShortName": "AActor",
      "Namespace": "Durin",
      "GeneratedHelperName": "Z_Construct_DClass_Durin_AActor",
      "Header": "Public/Engine/Actor.h",
      "API": "ENGINE_API"
    }
  }
}
```

Export files should stay small. Reading dependency export JSON files is usually cheap compared with repeatedly parsing C++ headers through Clang.

## Manifest Files

A manifest file is a private incremental-build cache for one module's reflection generation. It should record the inputs and outputs used by DurinHeaderTool so the generator can safely skip unchanged files.

The current direction is right: record reflect header fingerprints and dependency export fingerprints. The manifest should grow to include enough versioning and option data to avoid stale generated code.

Recommended fields:

```json
{
  "SchemaVersion": 2,
  "ToolVersion": "0",
  "SymbolNameScheme": "qualified-underscore-v1",
  "ModuleName": "Engine",
  "Profile": "DurinEditor",
  "Platform": "Win64",
  "ModuleConfigFingerprint": {},
  "GeneratorOptionsHash": "",
  "ReflectHeaders": {},
  "DependencyExports": {},
  "GeneratedOutputs": []
}
```

The manifest is not a public contract between modules. Other modules should read export files, not manifests.

The symbol name scheme should participate in manifest invalidation. Changing helper-name generation must force regenerated `.gen.h`, `.gen.cpp`, and export files, even when the reflected headers themselves did not change.

## Dependency Scope

The first implementation can read export files from all recursive public and private dependencies. That is simple and safe enough while the system is small.

Later, dependency scope should be narrowed:

- generated code for public headers should depend on public dependency exports
- generated code for private headers can depend on public and private dependency exports
- modules should not regenerate because of unrelated private symbols from distant dependencies

This refinement can wait until the reflection model works end-to-end.

## Parser And Resolver

DurinHeaderTool should be structured as a pipeline:

```text
clang parser
-> reflection model
-> symbol resolver
-> generated C++ writer
-> export and manifest writer
```

The parser should prefer Clang AST information over token-string guessing:

- use semantic parent chains to build namespaces
- use base-specifier declarations for base classes
- use enum declarations for underlying types
- use canonical declarations or Clang USRs internally when useful

The resolver should map both fully qualified names and compiler-stable identities, such as Clang USRs when available, to exported symbol records. Fully qualified names are easier to inspect and should remain the generated-code identity.

The resolver should also own helper-name allocation and naming-rule validation. Writers should consume the resolved helper names instead of recomputing them independently, so exports, generated declarations, registration records, diagnostics, and snapshot tests all agree.

## Generated Header Contract

For each reflected header, the generated `.gen.h` should provide the declarations injected by `GENERATED_BODY()`.

The first class milestone should generate:

- friend access for generated construction helpers
- `GetPrivateStaticClass()`
- `StaticClass()` through `DECLARE_CLASS(...)`
- deleted copy and move operations
- default constructor glue through object initializer or default constructor macros

The original header should include its generated header near the bottom of its include block and before reflected declarations use `GENERATED_BODY()`.

Generated header macros must be namespace-aware. If a reflected class lives in `Durin::Gameplay`, generated friend declarations and `DECLARE_CLASS(...)` arguments must refer to the exact helper names resolved for `Durin::Gameplay::AActor`, not just the short spelling `AActor`.

## Generated Source Contract

For each reflected header, the generated `.gen.cpp` should provide:

- includes for `DObject/GeneratedCppIncludes.h`
- the original reflected header
- cross-module construct declarations using qualified generated helper names
- `IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(...)`
- no-register class construction helpers
- full class construction helpers
- static class parameter records
- static property parameter records
- compiled-in registration records

Generated source should use fully qualified C++ names for reflected C++ types. Helper function names should use the readable qualified-underscore scheme, such as `Z_Construct_DClass_Durin_Gameplay_AActor`, and cross-module declarations should come from dependency export files.

Generated `.gen.cpp` files should avoid re-parsing or guessing dependency symbols. If a superclass or property type is reflected, the writer should either resolve it through the current module model or read it from dependency exports before emitting declarations.

## Runtime Type System Milestone

The first runtime milestone is a small but real type system:

- `DCLASS` types register a `DClass`
- `T::StaticClass()` returns the same singleton for each reflected type
- `Obj->GetClass()` is correct for `NewObject<T>()`
- `DClass` records its super `DClass`
- `IsA` and `Cast` can replace simple `dynamic_cast` use cases
- `DStructure::ChildProperties` contains generated `FProperty` fields

`DurinCodeGen::FClassParams` should be expanded beyond class name and no-register function so generated class construction can pass property metadata into runtime construction.

## Property Milestone

Start with the smallest useful property set:

- primitive integer types
- `float`
- `double`
- `bool`
- `FString` when available
- reflected enum values
- reflected `DObject*` pointers

Each property needs:

- name
- flags
- array dimension
- byte offset
- property kind
- referenced reflected type when applicable

Container properties, nested reflected structs, aliases, templates, metadata specifiers, editor-only details, and function parameters can wait.

## Garbage Collection Milestone

GC should come after object-pointer properties are reflected. The GC can then consume the same property metadata instead of inventing a separate reference-description system.

The first GC milestone should include:

- a global object array with object records, not just raw storage
- root set support
- mark traversal through reflected object-pointer properties
- sweep or deferred destruction policy
- basic object flags for reachability and pending kill

Weak references, clusters, incremental marking, multithreaded marking, object handles, package roots, and editor transaction integration should wait.

## Staged Implementation Plan

1. Keep DurinHeaderTool in Python while the data model is changing. Stabilize schemas and tests before considering a C++ rewrite.
2. Replace empty reflection generation with a real parse-model-generate path for one simple header.
3. Generate readable namespace-safe class helper names from fully qualified names using the qualified-underscore scheme.
4. Generate working `.gen.h` content for `GENERATED_BODY()`.
5. Generate working `.gen.cpp` class registration for `DCLASS()`.
6. Extend export files into thin symbol indexes, including generated helper names and naming-rule validation.
7. Extend manifests with schema, tool, symbol-name scheme, profile, platform, config, options, and generated output tracking.
8. Add DHT snapshot tests that compare generated files for tiny reflected headers, including at least one namespaced class.
9. Attach generated primitive properties to `DClass`.
10. Attach generated object-pointer properties to `DClass`.
11. Implement `IsA` and `Cast` using `DClass` hierarchy.
12. Build a minimal GC on top of reflected object-pointer traversal.

## Deferred Work

Do not block the first usable milestone on:

- full Unreal-style package and CDO behavior
- hot reload
- Blueprint-style function reflection
- editor property panels
- serialization formats
- reflected containers
- reflected templates
- complete metadata specifier support
- incremental or concurrent GC

These systems are valuable, but they should consume the reflection model after the small type system is already reliable.

## Related Code

- `CMake/Project/ProjectTargets.cmake`
- `Engine/Source/Programs/DurinHeaderTool`
- `Engine/Source/Runtime/CoreDObject`
- `Engine/Intermediate/Build/<Platform>/<Profile>/<Module>/DHT`

## Related Docs

- `Documentation/Architecture/ReflectionSystem.md`
- `Documentation/Architecture/BuildSystem.md`
- `Documentation/Architecture/RuntimeArchitecture.md`
- `Documentation/Architecture/WorkspaceProjects.md`
