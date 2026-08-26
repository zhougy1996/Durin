# DHT Namespace-Scoped Generated Symbols Plan

Summary: Move generated reflection helpers into each reflected type's owning namespace and remove underscore restrictions without changing runtime type identity.

Last reviewed: 2026-08-26

Status: Archived
Completed: 2026-08-26

## Current Status

The `namespace-scoped-v2` contract is implemented across DurinHeaderTool,
generated outputs, schema-v6 exports, phase state, handwritten CoreDObject
reflection, repository consumers, tests, and the lasting reflection contract.
Reflected namespace and type segments accept underscores without flattened-name
collisions. Runtime and serialized reflected identity remain unchanged.

## Goal

Make the C++ scope of every generated per-type reflection symbol follow the
reflected type's canonical owning namespace, so ordinary underscores in
namespace and type names require no escaping and cannot collide through name
flattening.

For example, generate:

```cpp
namespace Durin::Game_Play
{
	DClass* Z_Construct_DClass_A_Player();
}
```

for `Durin::Game_Play::A_Player`, with the complete helper identity
`::Durin::Game_Play::Z_Construct_DClass_A_Player`.

## Scope

- Replace flattened global class, struct, and enum construction helpers with
  namespace-member helpers derived from reflected kind plus exact short name.
- Move `_NoRegister`, per-type `_Statics`, and registration-info entities into
  the same owning namespace so no per-type external symbol still needs a
  flattened qualified-name encoding.
- Give generated headers enough namespace structure to predeclare helpers and
  statics before `GENERATED_BODY()` expands inside the authored type.
- Fully qualify every generated friend declaration, callback, property helper,
  base helper, compiled-in registration record, and cross-module reference.
- Replace the persisted `GeneratedHelperName` string with a single central
  generated-symbol derivation model based on reflected kind, canonical
  namespace, and short name.
- Upgrade export, cache, manifest, dependency-snapshot, and tool identities that
  can retain the old symbol scheme.
- Migrate handwritten CoreDObject reflection symbols and repository callers to
  the same contract.
- Remove the `_` prohibition from reflected namespace and type-name segments,
  with focused same-spelling and cross-module collision coverage.

## Non-Goals

- Changing `QualifiedName`, `ShortName`, `/Cpp/<Module>` package ownership,
  runtime registry keys, serialized type names, asset formats, or legacy-name
  behavior.
- Changing namespace-aware reflected-type lookup, property resolution, or the
  hermetic reflected-header parsing boundary.
- Adding reflection for class-nested types, local types, anonymous-namespace
  types, templates as declarations, functions, or overloads.
- Preserving link compatibility with binaries generated under
  `qualified-underscore-v1`; all affected modules must be regenerated and
  relinked together.
- Publishing generated helpers as a supported gameplay/editor API. Existing
  direct repository callers are migrated, but a new public convenience API is
  deferred.
- Mirroring source directory layout in the flat module DHT output directory.

## Design Decisions and Invariants

### Symbol identity

- Runtime reflected identity remains the canonical qualified C++ type name.
  Generated C++ symbol identity is derived from, but is not part of, runtime or
  serialized identity.
- The selected scheme is `namespace-scoped-v2`:

  | Reflected type | Owning namespace | Local helper |
  | --- | --- | --- |
  | `Durin::Gameplay::A_Player` | `Durin::Gameplay` | `Z_Construct_DClass_A_Player` |
  | `Durin::Data_Set::F_Row` | `Durin::Data_Set` | `Z_Construct_DStruct_F_Row` |
  | `Durin::E_Mode` | `Durin` | `Z_Construct_DEnum_E_Mode` |
  | `F_Global` | global | `Z_Construct_DStruct_F_Global` |

- Kind prefixes keep class, struct, and enum implementation symbols distinct.
  `_NoRegister`, `_Statics`, and registration-info names are derived only from
  the same local helper/type identity inside the owning namespace.
- No delimiter encodes namespace boundaries. `_` is accepted in every named
  namespace and reflected short-name segment supported by the C++ parser.
- Global-namespace reflected types retain global helpers because the global
  namespace is their owning namespace. Their uniqueness follows the same
  uniqueness required of the reflected C++ type itself.

### Namespace representation and emission

- DHT models namespace paths structurally rather than recovering them by
  splitting or concatenating generated helper strings.
- Generated `.gen.h` files remain included at global scope. The writer groups
  forward declarations into explicit namespace blocks, then emits macros whose
  friend and accessor references use absolute qualified names.
- Inline namespace membership is semantic and must be captured from the AST and
  reproduced when a generated header or source reopens that namespace. Plain
  namespace strings remain sufficient for runtime identity, but not for C++
  emission.
- Anonymous-namespace reflected types are rejected deterministically before
  export because their helpers cannot form a cross-translation-unit module
  contract. Class-nested reflected types remain rejected until a separate
  ownership model exists.
- Generated namespace blocks contain declarations and definitions only. They
  do not add `using namespace` directives or depend on argument-dependent
  lookup.

### Visibility, ownership, and linkage

- The owning module's API macro remains attached to helper declarations and
  definitions so cross-DLL references retain explicit import/export behavior.
- Cross-module generated code always uses an absolute qualified helper
  reference; unqualified lookup and ambient `using` declarations may not affect
  code generation.
- File-level compiled-in defer aggregates that have no cross-translation-unit
  consumer use internal linkage in the generated `.gen.cpp`; they do not gain a
  fabricated public namespace identity.
- One authoritative symbol builder produces local declaration names and fully
  qualified C++ references. Writers, the intrinsic-symbol table, exports, and
  tests may not independently reconstruct helper spellings.
- The thin module export carries reflected semantic facts. The derived helper
  spelling is removed from its persisted schema unless implementation evidence
  demonstrates a consumer that cannot derive it from kind, namespace, and
  short name; any exception must be recorded before Stage 1 closes.

### Migration and failure behavior

- This is an atomic generator-contract migration. Mixed old/new generated
  outputs are invalid; tool, export schema, phase-state context, dependency
  snapshots, and manifests are invalidated together.
- No forwarding wrappers for old global generated helpers are emitted. Such
  wrappers would preserve global pollution, double the symbol surface, and
  leave the ambiguous scheme observable.
- Handwritten intrinsic helpers, macros, editor code, and tests migrate in the
  same change set or in buildable stages guarded by temporary source-local
  adapters that are removed before acceptance.
- A generation failure publishes no replacement export, generated header,
  generated source, manifest, dependency snapshot, or cache entry under the new
  scheme.

## Current Foundations and Gaps

| Area | Current foundation | Gap owned by this plan |
| --- | --- | --- |
| Semantic identity | Classes, structs, and enums retain namespace, short name, and qualified name | Helper spelling is flattened and rejects `_` |
| Header emission | Generated headers predeclare helpers/statics globally before authored namespaces | No namespace grouping or structured namespace emission |
| Friend access | Generated macros grant helper/statics access with absolute global names | References assume every helper begins at `::` |
| Source emission | One writer produces definitions, callbacks, property nodes, and registration arrays | Definitions and references assume unqualified global helper names |
| Cross-module exports | Export schema v5 publishes canonical semantic identity and `GeneratedHelperName` | Persisted derived spelling couples dependencies to v1 encoding |
| Namespace parsing | AST extraction retains canonical named namespace spelling | Inline-namespace structure and unsupported-scope rejection are not an emission contract |
| Caches/publication | Tool fingerprints, phase state, manifests, and atomic output publication exist | Every identity retaining v1 names must be enumerated and invalidated |
| Intrinsic reflection | Core math structs and `DObject` have handwritten generated-style symbols | Names, aliases, macros, and direct callers encode `Durin` into global identifiers |
| Tests | DHT has writer, property, namespace, export, cache, and publication coverage | Expected strings codify v1 and no fixture proves underscores are collision-free |

## Implementation Stages

### Stage 0: Freeze the namespace-scoped symbol contract

Dependencies: current reflection contract, DHT parser/export model, and
generated include placement.

- [x] Inventory every producer, persisted representation, and consumer of
  generated class/struct/enum helper, `_NoRegister`, `_Statics`, registration,
  and compiled-in defer names.
- [x] Add contract fixtures covering `_` in root/nested namespaces and class,
  struct, and enum short names, including pairs that collide under v1.
- [x] Record behavior for global, nested named, inline, anonymous, alias-spelled,
  and class-nested scopes; confirm supported scopes against MSVC and the DHT
  libclang AST.
- [x] Compile minimal generated-header probes proving that namespace-member
  helper/statics forward declarations satisfy qualified friend declarations
  before the authored namespace is opened.
- [x] Decide from evidence whether export schema v6 removes
  `GeneratedHelperName` entirely; record any unavoidable persisted field and
  its single derivation path.
- [x] Enumerate all handwritten and direct-call migrations, including
  CoreDObject math structs, `DObject`, legacy object macros, editor property
  views, and native tests.
- [x] Name every cache, phase-state, manifest, export, and dependency identity
  requiring an invalidation bump.

#### Acceptance Gate

- A reviewed matrix assigns one exact declaration and reference spelling to
  every supported scope/type-kind combination, including v1 collision pairs.
- C++ compiler probes establish the forward-declaration, friend, inline
  namespace, and DLL visibility forms the writers will emit.
- Export representation, unsupported-scope diagnostics, invalidation boundary,
  and all manual migration sites are explicit before production writers change.

### Stage 1: Introduce one structured generated-symbol model

Dependencies: Stage 0 contract and export decision.

- [x] Replace `qualified_name_to_helper_suffix` and the three independent
  helper-name constructors with a value model containing kind, namespace path,
  local helper name, and absolute qualified reference.
- [x] Represent namespace segments and inline status independently from the
  runtime qualified-name string used by registration and serialization.
- [x] Make `_NoRegister`, `_Statics`, and registration-info derivation consume
  the same model without flattening namespaces.
- [x] Remove the underscore rejection and retain deterministic diagnostics for
  genuinely unsupported identifiers/scopes.
- [x] Update intrinsic-symbol construction and parser models to use the shared
  builder rather than hard-coded `Z_Construct_*_Durin_*` strings.
- [x] Upgrade export schema and serialization according to Stage 0, rejecting
  stale or mixed-schema inputs explicitly.
- [x] Add order-independent model/export round-trip tests for all kinds,
  namespace depths, inline namespaces, global types, and underscore collisions.

#### Acceptance Gate

- Every supported reflected identity maps to one byte-stable namespace path,
  local generated name, and fully qualified reference with no ambiguous reverse
  parsing.
- No production model, resolver, or export path constructs a generated symbol
  by replacing `::` with `_`.
- Old exports fail or invalidate through the selected schema boundary rather
  than silently producing mixed symbols.

### Stage 2: Emit namespace-correct generated headers and sources

Dependencies: Stage 1 symbol model.

- [x] Group `.gen.h` declarations by structured namespace path and emit correct
  nested/inline namespace blocks for helper functions and statics.
- [x] Change `GENERATED_BODY()` fragments to use absolute qualified friend,
  `DECLARE_CLASS`, and `StaticStruct()` references.
- [x] Emit helper, `_NoRegister`, statics, registration-info, property metadata,
  and callback definitions in their owning namespace in `.gen.cpp`.
- [x] Fully qualify cross-namespace base/property/enum/struct helpers at every
  generated use site.
- [x] Put file-only compiled-in registration aggregates behind internal linkage
  while keeping their function-pointer entries fully qualified.
- [x] Preserve deterministic ordering and byte-stable output when one header
  declares reflected types across multiple namespaces.
- [x] Expand writer golden tests for empty/multiple namespaces, same short names
  in different namespaces, underscore names, cross-module imports, inline
  namespaces, and headers containing all three reflected kinds.

#### Acceptance Gate

- Generated headers compile with their existing global include placement and
  grant only the required access to every reflected type.
- Generated sources define no flattened externally linked per-type symbol and
  contain only absolute cross-namespace helper references.
- Writer output is deterministic across symbol-map order and repeated runs.

### Stage 3: Migrate handwritten reflection and repository consumers

Dependencies: Stage 2 writer contract.

- [x] Move handwritten `DObject` and intrinsic math struct helpers into their
  owning namespace and shorten their local names consistently with v2.
- [x] Update legacy CoreDObject reflection macros or replace their generated-name
  token concatenation with the shared namespace-scoped contract.
- [x] Remove global-to-`Durin` `using` aliases and any temporary adapters after
  all generated and handwritten definitions use v2.
- [x] Migrate generated-code fixtures, Editor property-view comparisons, native
  tests, and other direct callers to absolute v2 names.
- [x] Prefer normal `StaticClass()`/`StaticStruct()` entry points at call sites
  where they already exist, without introducing a new public reflection API.
- [x] Verify API import/export annotations and mangled symbols across at least
  one dependency-module boundary on the supported Windows toolchain.

#### Acceptance Gate

- Repository source outside archived documentation contains no production use
  of a `Z_Construct_D*_Durin_*` flattened global symbol.
- Handwritten and generated reflection expose the same namespace-scoped naming
  contract and link across module DLL boundaries.
- No compatibility wrapper or `using` alias keeps a v1 global helper linkable.

### Stage 4: Invalidate persistent state and harden publication

Dependencies: Stages 1-3.

- [x] Bump the DHT tool/symbol scheme, export schema, parser/generator context,
  and each phase-state or manifest identity selected in Stage 0.
- [x] Ensure dependency snapshots compare semantic exported identities and
  cannot retain an old helper spelling.
- [x] Cover cold generation, warm reuse, touched-but-unchanged input, changed
  dependency export, corrupt cache, missing output, and interrupted publication.
- [x] Verify old export/cache/output mixtures trigger deterministic regeneration
  or a clear schema error without publishing partial v2 artifacts.
- [x] Confirm independent modules remain safely parallel while commands writing
  the same module retain their existing serialization boundary.

#### Acceptance Gate

- Clean, incremental, and cache-recovery paths converge on byte-identical v2
  exports and generated outputs.
- No old helper spelling survives through a cache hit, dependency snapshot, or
  stale generated-file repair path.
- Injected failure cannot expose a mixed v1/v2 module output set.

### Stage 5: Qualify the repository and publish the lasting contract

Dependencies: Stages 0-4 and all focused acceptance gates.

- [x] Run the complete DurinHeaderTool Python suite, including writer, namespace,
  property, export, persistent-state, and failure-publication coverage.
- [x] Run required configure/generation validation from warm incremental and
  clean generated-output states under the repository build workflow.
- [x] Run the shared-infrastructure full build because generated declarations
  and link symbols affect every reflected module.
- [x] Run affected CoreDObject, serialization/asset, Engine, and Editor native
  tests selected under the repository testing workflow.
- [x] Inspect produced library/executable symbols or link maps to confirm v2
  namespace mangling and absence of v1 exported helpers.
- [x] Update the Generated Reflection System contract with namespace-scoped
  symbols, underscore support, supported namespace forms, internal linkage,
  export derivation, and binary migration rules.
- [x] Record exact validation evidence, close every acceptance gate, and update
  plan lifecycle metadata before completion.

#### Acceptance Gate

- DHT tests, cold/warm generation, the required full build, selected native
  tests, and documentation validation all pass.
- Representative v1 collision pairs generate, compile, link, register, and
  resolve as distinct reflected types across same-module and dependency-module
  boundaries.
- Runtime qualified identities and serialized metadata remain byte-for-byte
  compatible for all pre-existing reflected types.

## Validation Matrix

| Contract | Focused validation | Integration outcome |
| --- | --- | --- |
| Local naming | Class/struct/enum names containing leading, trailing, and repeated `_` | Valid C++ names generate without escaping or rejection |
| Namespace separation | `A_B::C` versus `A::B_C`, including dependency modules | Distinct C++ helpers and runtime registrations |
| Namespace depth | Global, root named, nested named, and multiple namespaces per header | Correct declarations, definitions, friends, and callbacks |
| Inline namespaces | AST capture plus compiler probes for reopening and qualification | Canonical inline member links correctly without identity drift |
| Unsupported scopes | Anonymous and class-nested reflected fixtures | Deterministic pre-publication diagnostics |
| Friend access | Private class/struct members and static accessors | Existing generated access remains sufficient and no broader |
| Cross-module visibility | Imported base, object, enum, and struct properties | API macros and fully qualified callbacks link across DLLs |
| Intrinsics | `DObject`, vectors, quaternion, matrix, transform, and color | Handwritten and generated paths follow one v2 contract |
| Export/cache migration | v1 export/state/output mixed with v2 tool | Explicit invalidation and atomic regeneration |
| Determinism | Permuted header, symbol, dependency, and worker order | Byte-identical exports and generated files |
| Runtime compatibility | Existing qualified lookup and save/load fixtures | No runtime identity or serialized-name change |
| Symbol audit | Built module exports/link maps | No externally linked flattened v1 per-type helper remains |

## Definition of Done

- `namespace-scoped-v2` is the only generated per-type symbol scheme in active
  source, generated outputs, exports, and caches.
- Reflected namespace and type-name segments may contain `_` without ambiguity,
  and deliberate v1 collision pairs work across module boundaries.
- Generated and handwritten helpers, friends, statics, registration records,
  property callbacks, and compiled-in registration all use absolute,
  namespace-correct references.
- Old persistent state cannot produce or preserve mixed-contract outputs.
- Pre-existing runtime qualified identities, package ownership, serialization,
  and legacy names are unchanged.
- Full DHT/shared-build/native-test qualification passes with recorded evidence.
- Lasting behavior is authoritative in the Generated Reflection System contract,
  and this plan is completed under the repository documentation lifecycle.

## Completion Evidence

- DurinHeaderTool Python suite: 199 tests passed, including structured-symbol,
  schema-v6, underscore-collision, global, inline, anonymous-scope, writer,
  property, cache, and publication coverage.
- Cold migration: the first `CoreDObject` and full repository builds rejected
  schema-v5 exports and schema-v1 phase envelopes, regenerated v2 state, and
  linked successfully.
- Incremental generation: a second `./DevTool build --target all` completed with
  `ninja: no work to do`.
- Shared build: `./DevTool build --target all` completed successfully for the
  `MacOS-arm64-Debug-DurinEditor` profile.
- Native validation: `./DevTool test "@domain=reflection"` passed all 3 targets;
  `./DevTool test fast-all` passed all 64 selected contract, feature, and
  infrastructure targets, including CoreDObject, asset, Engine, and Editor
  coverage.
- Symbol audit: `nm -gU | c++filt` over the CoreDObject and Engine dylibs found
  975 namespace-qualified generated symbols and no global exported
  `Z_Construct_DClass_*`, `Z_Construct_DStruct_*`, or `Z_Construct_DEnum_*`
  symbol.
- Documentation validation: the changed-scope and complete plan validators
  passed before archival.

## Deferred Follow-ups

- A public typed lookup API that removes legitimate direct calls to intrinsic
  construction helpers from editor/tests.
- Reflection for class-nested types, which requires a helper ownership scheme
  that does not attempt to inject free functions into class scope.
- Reflection for anonymous-namespace or translation-unit-local types.
- General namespace-alias export semantics beyond the existing hermetic AST and
  source-spelling contract.
- A future plugin ABI/version negotiation mechanism; this migration requires a
  coordinated rebuild rather than preserving v1 generated symbols.

## Related Documentation

- [Generated Reflection System](../../../Runtime/Core/ReflectionSystem.md)
- [Build System](../../../Development/Build/BuildSystem.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [DHT Namespace-Aware Type Resolution](DHTNamespaceAwareTypeResolution.md)

## Related Code

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/model/reflection_info.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/model/export_info.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/reflection_ast_helpers.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/resolver/reflection_resolver.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_header_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_property_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_export_file_generator.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_reflection_files_generator.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/cache/phase_state.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_namespace.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_property.py`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectMacros.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/MathStructs.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/MathStructs.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Object.cpp`
