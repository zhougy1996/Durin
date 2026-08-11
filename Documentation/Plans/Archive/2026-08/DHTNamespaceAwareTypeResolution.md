# DHT Namespace-Aware Type Resolution Plan

Summary: Make DurinHeaderTool resolve reflected base and property type spellings with deterministic lexical C++ namespace semantics while preserving fully qualified runtime identity.

Last reviewed: 2026-08-11

Status: Archived
Completed: 2026-08-11

## Current Status

All stages are complete. DHT now uses one kind-filtered, scope-aware resolver
for base exports and reflection properties. It supports unqualified,
relatively qualified, fully qualified, and leading-global spellings; propagates
the declaring namespace through nested container shapes; produces deterministic
lookup-chain/candidate diagnostics; and invalidates old persistent results with
`hermetic-namespace-lookup-v2`.

The compatibility audit found no production header requiring the removed
global-unique-short-name fallback. DHT pytest passed 198/198, warm and fresh
configuration succeeded, the required full `all` build succeeded, and focused
runtime qualification passed `CoreObjectTests` 73/73 and `AssetPackageTests`
97/97. Existing generated qualified identities remained unchanged. Gameplay
Foundation G0 is closed and G1 is open for its entry audit.

## Goal

Make a reflected C++ type reference resolve to the same declared reflected type
that a supported, hermetic C++ spelling denotes in its lexical namespace,
including unqualified, relatively qualified, globally qualified, nested
container, and same/cross-module cases. True ambiguity or an unavailable type
must fail deterministically with a source-qualified diagnostic and candidate
information instead of depending on global short-name uniqueness.

## Scope

- One shared, scope-aware reflected-symbol resolution API for classes, structs,
  and enums.
- Unqualified lookup through the declaring namespace and each enclosing
  namespace from nearest to outermost.
- Relatively qualified lookup through the same lexical namespace chain.
- Leading global `::` normalization and exact fully qualified lookup.
- Same-module raw export plus dependency-export lookup independent of header,
  module, map, or worker completion order.
- Base-class resolution in export and reflection generation phases.
- Object, soft-object, struct, enum, `TObjectPtr`, fixed-array, `std::vector`,
  and `std::unordered_map` property resolution, including recursive nested
  values.
- AST declaration identity where libclang provides it, with scoped source-
  spelling fallback where source spelling is required for hermetic container
  parsing or target-compiler `sizeof` expressions.
- Deterministic unresolved/ambiguous diagnostics that include the source
  spelling, lookup namespace, allowed symbol kinds, and sorted candidates.
- Cache, export-manifest, dependency-snapshot, generator, and test updates
  required by the semantic change.
- Lasting documentation of supported lookup and hermetic-header behavior.

## Non-Goals

- Implementing the complete C++ standard name-lookup model, argument-dependent
  lookup, overload resolution, templates as declarations, concepts, inline-
  namespace versioning, namespace aliases imported only by stripped includes,
  or compiler-specific extension lookup.
- Making ordinary include contents a semantic DHT input. DHT continues to use
  exports plus a synthetic parser prelude and rejects include-only aliases or
  macros under the existing hermetic-header policy.
- Resolving non-reflected C++ classes as reflected property or base types.
- Changing `QualifiedName`, `ShortName`, generated helper naming, C++ package
  identity, serialized class names, asset package formats, or runtime type
  registry lookup.
- Permitting ambiguous short names because one module, dependency, or input map
  happened to load first.
- Adding gameplay types, changing Gameplay naming, or implementing Pawn,
  Controller, GameMode, movement, input, or camera behavior.
- Replacing libclang, rewriting DHT, or adopting a general C++ semantic index.

## Design Decisions and Invariants

### Stable identity versus lookup spelling

- `ExportedSymbolInfo.QualifiedName` remains the stable reflected identity and
  export-map key. `ShortName` remains presentation/search metadata and never
  becomes a unique registry key.
- Lookup resolves a source spelling to one qualified identity before code
  generation. Generated files, dependency snapshots, serialized metadata, and
  runtime registration consume only the qualified identity.
- The current `qualified-underscore-v1` generated-helper scheme is unchanged.
  This plan does not relax its prohibition on `_` inside reflected namespace
  and type-name segments.

### Supported lexical lookup

- A spelling beginning with `::` is globally qualified. DHT removes exactly the
  leading global marker and performs one exact kind-filtered lookup.
- For another spelling containing `::`, DHT treats it as relatively qualified
  unless its exact exported identity already matches. It prefixes the complete
  declaring namespace, then each enclosing namespace, from nearest to
  outermost, and finally considers the unprefixed global spelling.
- For an unqualified spelling, DHT applies the same nearest-to-outermost
  namespace walk. The first lexical scope containing a valid kind-filtered
  identity wins, matching ordinary namespace shadowing.
- DHT does not search arbitrary unrelated namespaces merely because a short
  name is globally unique. Any temporary legacy fallback discovered in Stage 0
  must be removed or explicitly bounded by compatibility evidence; it cannot
  override lexical lookup or hide ambiguity.
- Candidate construction, kind filtering, and diagnostics are deterministic
  and sorted by qualified name. Dictionary insertion and parallel header
  completion order are never observable.

Example from `Durin::Sandbox`:

```text
Gameplay::APawn
  -> Durin::Sandbox::Gameplay::APawn
  -> Durin::Gameplay::APawn
  -> Gameplay::APawn
```

Example for unqualified `AActor` from `Durin::Gameplay`:

```text
AActor
  -> Durin::Gameplay::AActor
  -> Durin::AActor
  -> AActor
```

### Parser and source-spelling policy

- When libclang supplies the referenced declaration for a base or field type,
  its semantic qualified identity is authoritative after verification against
  available reflected symbols and the allowed kind.
- Source spelling remains necessary for deterministic reflection parsing where
  DHT deliberately strips includes, for Durin math aliases whose canonical
  Clang form is not the reflected spelling, for explicit container-shape
  validation, and for target-compiler `sizeof(...)` expressions.
- Source-spelling fallback receives the declaring class/struct namespace and
  propagates it recursively into container inner/key/value resolution. Nested
  parsing may not silently fall back to a context-free global short-name scan.
- An explicit `using` declaration written in the reflected header may resolve
  through libclang declaration identity. A `using` or alias supplied only by a
  stripped include remains unsupported and receives the existing non-hermetic
  diagnostic category.
- If AST and scoped source resolution both produce reflected identities, they
  must agree. Disagreement is a deterministic DHT error rather than a priority
  heuristic.

### Failure and publication

- Unresolved and ambiguous are distinct failures. Ambiguous diagnostics list
  all viable candidates at the first applicable lookup scope; unresolved
  diagnostics state the attempted lexical chain.
- A failed header publishes no replacement generated output or dependency
  snapshot. Existing output/cache transaction rules remain intact.
- Export resolution has the complete raw same-module symbol set before it
  resolves bases, so header order and cold/warm cache state cannot change the
  result.
- Any cache entry capable of retaining pre-change resolution results is
  invalidated through the appropriate tool/parser/resolver context version or
  dependency snapshot. Manual cache deletion is not part of correctness.

## Current Foundations and Gaps

| Area | Foundation | Gap owned by this plan |
| --- | --- | --- |
| Symbol model | Export maps keyed by qualified name with short name, namespace, kind, header, API, base, and helper data | Resolver API has no declaring-scope input |
| Fully qualified references | Exact map lookup works | Leading global `::` is not normalized |
| Unqualified references | Global unique short name resolves; ambiguity returns no result | No enclosing-scope walk; unrelated global uniqueness can mask incorrect semantics |
| Relative qualification | Parser/export retain source spellings | `Gameplay::APawn` is treated as an exact global identity |
| Base export | Same-module raw symbols are merged; exact current namespace tried first | No parent namespace, global marker, AST/source agreement, or candidate diagnostic |
| Simple properties | AST canonical identity can rescue some struct/enum cases | Scope is not a first-class input and behavior differs by property shape |
| Containers | Explicit source parsing validates nested shapes | Recursive types resolve without declaring namespace and fail under duplicate short names |
| Hermetic parsing | Includes are stripped; synthetic declarations reproduce exported types | Supported header-local using behavior and AST/source agreement are not fully qualified |
| Diagnostics | Unsupported non-hermetic types and invalid containers fail | Ambiguity is often reported as generic unsupported spelling without scope/candidates |
| Tests | Qualified helpers, ambiguity non-selection, same-short-name source association, cold export order | No full lookup matrix across bases, properties, nesting, namespaces, modules, and caches |
| Production symbols | 85 current DurinEditor symbols, no duplicate short names | No production collision exercises the latent defect |

## Implementation Stages

### Stage 0: Freeze the supported lookup contract and regression matrix

Dependencies: current qualified symbol/export model and hermetic parser policy.

- [x] Add focused resolver fixtures for unqualified, relatively qualified,
  fully qualified, and leading-global spellings from root, nested, sibling, and
  unrelated namespaces.
- [x] Add deliberate same-short-name class, struct, and enum collisions across
  same-module and dependency-module exports.
- [x] Record nearest-scope shadowing, enclosing-scope fallback, unrelated-
  namespace rejection, kind filtering, and sorted ambiguity expectations.
- [x] Cover base classes, raw object pointers, `TObjectPtr`, `TSoftObjectPtr`,
  structs, enums, fixed arrays, Vector/Map, and recursively nested containers.
- [x] Inventory current production/reflection-test callers that rely on global
  unique short-name fallback, header-local `using`, aliases, canonical Durin
  math spellings, or source-only parsing.
- [x] Select the exact compatibility disposition for every discovered legacy
  caller and record any required source qualification before resolver behavior
  changes.
- [x] Identify all persistent/export/reflection cache identities that retain
  resolved names and select the minimum deterministic invalidation bump.

#### Acceptance Gate

- One reviewed matrix states the result or diagnostic for every supported
  spelling/property shape and distinguishes C++ lexical visibility from
  unrelated global uniqueness.
- Every current compatibility dependency has an explicit retain, qualify, or
  reject disposition; no insertion-order fallback remains implicit.
- Cache invalidation and generated-output publication boundaries are named
  before implementation begins.

### Stage 1: Implement one scope-aware symbol resolver

Dependencies: Stage 0 lookup matrix.

- [x] Extend the shared resolver API with declaring namespace and source-
  spelling context while retaining explicit allowed-kind filtering.
- [x] Normalize leading global qualification and generate the nearest-to-
  outermost candidate chain for unqualified and relatively qualified names.
- [x] Return structured success, unresolved, and ambiguity information suitable
  for deterministic diagnostics rather than collapsing every failure to
  `None`.
- [x] Make candidate selection independent of dictionary/export/module order.
- [x] Replace export base resolution's separate local-name heuristic with the
  shared resolver and the exported symbol's declaring namespace.
- [x] Resolve cold same-module and dependency-module bases from the complete
  available symbol set and publish only qualified base identities.
- [x] Add unit coverage for exact, lexical, relative, shadowed, wrong-kind,
  unresolved, ambiguous, and order-permuted cases.

#### Acceptance Gate

- Base-type lookup agrees with the Stage 0 matrix for every scope and spelling.
- Same-module header order, dependency order, worker completion order, and
  symbol-map insertion order produce byte-identical export identities.
- No resolver consumer needs a separate current-namespace or global-unique
  heuristic.

### Stage 2: Propagate declaring scope through every property shape

Dependencies: Stage 1 resolver and parser AST/source inventory.

- [x] Pass each reflected class/struct namespace into field parsing and
  post-parse symbol resolution.
- [x] Prefer verified libclang declaration identity for simple base, class,
  struct, and enum references when available.
- [x] Preserve source spellings for layout and hermetic validation while
  checking AST/source identity agreement.
- [x] Thread namespace context recursively through raw object, `TObjectPtr`,
  `TSoftObjectPtr`, fixed-array, Vector inner, Map key/value, and nested
  container parsing.
- [x] Preserve Durin math aliases, primitive aliases, explicit container limits,
  soft-reference restrictions, and target-compiler `sizeof` generation.
- [x] Cover explicit header-local using declarations that libclang resolves and
  retain deterministic rejection for include-only aliases.
- [x] Remove context-free `_resolve_short_symbol` calls or make their scope and
  intended exact behavior explicit.

#### Acceptance Gate

- Every supported property shape resolves to one qualified reflected identity
  under deliberate short-name collisions.
- AST and source paths agree, nested containers inherit the declaring scope,
  and unsupported aliases remain hermetic failures.
- Generated code contains the correct qualified helper, referenced type, and
  `sizeof` spelling without changing property layout or serialization identity.

### Stage 3: Harden diagnostics, dependencies, and cache behavior

Dependencies: Stages 1-2.

- [x] Emit source/header/property-or-base-qualified unresolved diagnostics with
  the attempted spelling, lookup namespace, allowed kinds, and lexical chain.
- [x] Emit ambiguity diagnostics with deterministically sorted viable qualified
  candidates and no recommended winner.
- [x] Ensure resolved-symbol dependency snapshots contain the selected
  qualified identity and invalidate when that symbol's relevant export changes.
- [x] Bump the selected parser/resolver/tool cache context so clean, rebuild,
  incremental, persistent-cache, and stale-output paths cannot reuse old
  resolution semantics.
- [x] Verify failed resolution publishes no partial export, generated header,
  generated source, manifest, or cache entry.
- [x] Add cold/warm, cache hit/miss/corruption, changed dependency export, and
  failed-regeneration recovery coverage around colliding names.

#### Acceptance Gate

- Equivalent failures have byte-stable diagnostics regardless of execution and
  map order.
- Cache state cannot change the selected identity, hide a new ambiguity, or
  retain generated output from the previous lookup contract.
- Dependency snapshots and manifests explain every external selected type.

### Stage 4: Qualify generated code and publish the lasting contract

Dependencies: Stages 1-3 and all focused acceptance gates.

- [x] Run the complete DurinHeaderTool Python suite, including the new namespace
  collision and cache matrix, under repository guidance.
- [x] Run root configure and generation from both warm incremental and clean-
  output states and compare the selected qualified identities.
- [x] Run the required shared-infrastructure full `all` build because DHT
  generation affects every reflected module.
- [x] Confirm representative Runtime/Editor asset serialization and reflected
  registration tests retain their existing qualified identities.
- [x] Update the Generated Reflection System documentation with supported
  lexical lookup, ambiguity, global qualification, hermetic using/alias, and
  diagnostic rules.
- [x] Record final tests, generation/build evidence, cache/version changes, and
  Gameplay Foundation G0 handoff; update the roadmap to open G1.
- [x] Run plan, roadmap, and repository documentation validation.

#### Acceptance Gate

- Complete DHT tests, configure/generation paths, the required full build, and
  affected reflection/runtime tests pass.
- Existing qualified reflected identities and serialized metadata remain
  unchanged unless an explicitly recorded incorrect legacy reference was
  qualified.
- The lasting reflection contract owns the new behavior and the Gameplay
  Foundation roadmap can create its G1 plan without DHT workarounds.

## Validation Matrix

| Contract | Focused validation | Integration outcome |
| --- | --- | --- |
| Absolute identity | Exact and leading-`::` class/struct/enum cases | Fully qualified metadata and helpers remain stable |
| Lexical lookup | Current, parent, root, shadowed, sibling and unrelated scopes | Supported source spelling matches deterministic C++ namespace visibility |
| Relative qualification | Multi-segment candidates from nested namespaces | `Gameplay::APawn` resolves from `Durin::Sandbox` without global guessing |
| Ambiguity | Deliberate same-short-name collisions and order permutations | No insertion/module/header order selects a winner |
| Bases | Same-header, cold same-module and dependency-module inheritance | Export and reflection phases publish one identical qualified base |
| Properties | Raw/object wrapper/soft/struct/enum/fixed array | Every reference uses the intended qualified type/helper |
| Containers | Vector/Map and recursive mixed nesting | Inner/key/value resolution retains declaring scope and existing restrictions |
| AST/source | Canonical declaration, Durin math alias, source layout spelling | Identity agrees while target-compiler layout expressions remain correct |
| Hermetic headers | Local using and include-only alias fixtures | Supported local semantics work; stripped-include dependencies fail clearly |
| Caches | Cold/warm, hit/miss, invalid/corrupt, dependency change | Resolution cannot be stale or cache-state dependent |
| Publication | Failure injection before export/generation/cache publication | No partial or mixed-contract output becomes visible |
| Qualification | Complete DHT suite, configure/generate, full build, runtime reflection tests | Shared generated code remains buildable and identity-compatible |

## Definition of Done

- One resolver implements the selected lexical namespace policy for all
  reflected symbol kinds and consumers.
- Base and every supported property/container shape resolve to fully qualified
  identities with deliberate duplicate short names present.
- Relatively and globally qualified spellings behave according to the recorded
  contract.
- Unresolved and ambiguous failures are deterministic, actionable, and
  publication-safe.
- Export, reflection, dependency, cache, and generated-output behavior is
  independent of order and warm/cold state.
- Existing serialized qualified identities remain compatible.
- Complete DHT/shared-generation qualification passes and lasting behavior is
  documented outside this plan.
- Gameplay Foundation G0 is closed and G1 is explicitly opened by the roadmap.

## Deferred Follow-ups

- General C++ template declaration lookup, concepts, ADL, overload resolution,
  and non-reflected semantic indexing remain outside DHT's reflected-type
  contract.
- Namespace aliases or using directives supplied only by stripped includes
  require a separate hermetic export representation before they can be
  supported.
- Removing the underscore restriction from generated helper names requires a
  separate symbol-name scheme and migration plan.
- Editor class pickers may later display namespace disambiguators for duplicate
  `ShortName`/`DisplayName` values; this plan guarantees identity and diagnostics
  but does not redesign class-selection UI.
- Gameplay implementation remains owned by the child plans selected through
  the Gameplay Foundation roadmap.

## Related Documentation

- [Gameplay Foundation Roadmap](../../../Roadmaps/Archive/2026-08/GameplayFoundation.md)
- [Generated Reflection System](../../../Runtime/Core/ReflectionSystem.md)
- [Build System](../../../Development/Build/BuildSystem.md)
- [Build And Run](../../../Development/Build/BuildAndRun.md)
- [Workspace And Projects](../../../Workspace/WorkspaceProjects.md)
- [DHT Persistent Header Cache](DHTPersistentHeaderCache.md)
- [DHT Strict Descriptor Parsing](DHTStrictDescriptorParsing.md)

## Related Code

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/resolver/reflection_resolver.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/reflection_parser.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/extractors/export_symbol_extractor.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_export_file_generator.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_reflection_files_generator.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/model/export_info.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/model/reflection_info.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/cache/persistent_header_cache.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_generation.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_export_persistent_cache.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_reflection_persistent_cache.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/test_persistent_header_cache.py`
