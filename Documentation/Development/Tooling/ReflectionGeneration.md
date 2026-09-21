# Reflection Generation

Summary: Define DurinHeaderTool parsing, symbol exports, and generated C++ contracts.

Last reviewed: 2026-09-21

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
[Build System](../Build/BuildSystem.md). They do not alter the
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

Private generation-state ownership and invalidation follow
[Build System](../Build/BuildSystem.md). Consumers must not read
private phase state. Unchanged public exports retain their bytes and timestamps.
Export generation reparses only changed headers, reusing raw symbol projections
(including empty ones) for other headers. Resolution merges the complete module
projection and dependency exports in deterministic header/name order. Timestamp
and size are a cheap guard for content hashing: touching unchanged content only
refreshes cached filesystem metadata.

`CoreDObject` uses `DObject/MirrorExportTypes.h` under `_DHT_EXPORTS_PARSER` to publish intrinsic core types such as `Durin::DObject`, `Durin::DType`, `Durin::DStructBase`, and `Durin::DClass` without generating duplicate runtime class registration for those intrinsic types.

## Persistent Generation State

DHT phase bundles are disposable reconstruction data rather than public
reflection inputs. Their schemas, ownership, invalidation, clean/purge behavior,
and interrupted-output recovery are defined by
[Build System](../Build/BuildSystem.md).

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

## Related documentation

- [Runtime reflection](../../Runtime/Core/ReflectionSystem.md)
- [Build system](../Build/BuildSystem.md)
