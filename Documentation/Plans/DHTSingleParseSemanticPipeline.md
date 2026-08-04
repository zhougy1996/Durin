# DHT Single-Parse Semantic Pipeline Plan

Summary: Parse each invalidated reflected header once into a serializable semantic IR, then project exports and generate reflection output without a second libclang parse.

Last reviewed: 2026-08-03

Status: Active
Completed:

## Current Status

The direction is selected and implementation has not started. Current DHT
generation calls `parse_reflection_header` once in export mode and again in
reflection mode. A measured cold Engine generation parsed the same 29 headers
for 22.921 seconds during Export and 19.834 seconds during Reflection.

Implementation is ordered after the cache schema and storage primitives in
[DHT Persistent Header Cache](Archive/2026-08/DHTPersistentHeaderCache.md) reach a stable stage.
This plan reuses that ownership, versioning, atomic-publication, and corruption
contract instead of creating a second intermediate format.

## Goal

On a true cold generation or invalidated-header build, invoke libclang exactly
once per affected reflected header, publish module exports from that parse, and
later resolve and generate reflection files from a serializable semantic IR
after dependency exports become available.

## Scope

- Introduce a plain-data, versioned per-header semantic IR containing every
  declaration and raw property fact needed by both export projection and
  reflection generation.
- Separate AST extraction from exported-symbol resolution and C++ source
  generation.
- Use one unified DHT parser mode for reflected headers, including explicit
  export-only declarations required by dependency parsing.
- Make Export own libclang parsing, IR persistence, and module export projection.
- Make Reflection load IR, resolve symbols against the completed export set, and
  generate outputs without reparsing headers.
- Preserve existing module ordering: exports remain available to dependent
  modules before reflection generation completes.
- Retain a safe parser fallback when a required IR entry is missing, stale, or
  damaged.
- Remove the legacy second-parse path only after differential output and error
  behavior are qualified.

## Non-Goals

- Preserving cache entries across clean/rebuild; that lifecycle belongs to
  [DHT Persistent Header Cache](Archive/2026-08/DHTPersistentHeaderCache.md).
- Changing reflected declarations, supported property types, generated symbol
  names, generated file layout, or runtime reflection behavior.
- Merging Export and Reflection into one CMake command or delaying module export
  publication until reflection completes.
- Introducing Clang PCH files, serialized Clang translation units, umbrella
  translation units, or batch/shard parsing in this plan.
- Solving Ninja/DHT CPU-budget coordination or changing the active Ninja
  executable.

## Design Decisions and Invariants

- The semantic IR contains only versioned Python primitives and model values.
  It never stores a libclang cursor, type handle, translation unit, process-local
  object identity, or absolute worktree-specific path.
- Export remains the producer edge because it can run before dependency exports
  and must publish symbols early. Reflection remains a consumer edge ordered
  after the module's own export and required dependency exports.
- The unified parse runs with the DHT export-parser environment so declarations
  needed only to describe cross-module root types remain visible. Such
  declarations receive an explicit `ExportOnly` semantic marker and are omitted
  from reflection code generation.
- `_DHT_EXPORTS_PARSER` is currently used only by the CoreDObject mirror-export
  header. Migration replaces this implicit phase difference with an explicit,
  tested export-only IR projection contract; adding another phase-conditional
  reflected declaration is rejected unless it adopts the same marker.
- Raw property IR records source spelling, canonical spelling, declaration kind,
  size/alignment facts required by generation, annotations, container nesting,
  and unresolved referenced-symbol candidates. It does not require the final
  module export table during AST traversal.
- Export projection consumes declaration identity, namespace, inheritance,
  abstractness, enum representation, and helper naming from IR. It must produce
  byte-identical module export data to the legacy extractor.
- Reflection resolution consumes IR plus available exports and produces the
  current `ReflectedHeaderInfo`/generated-source model. Dependency export changes
  rerun resolution and generation but do not rerun libclang when the header IR
  remains compatible.
- Each IR entry records header content identity, tool fingerprint, schema,
  platform, runtime variant, parser options, module, and logical header path.
  Export and Reflection verify the same entry identity before use.
- Parser diagnostics retain source file, line, and column information in IR so
  moving resolution after parsing does not degrade actionable error messages.
- During migration, the legacy two-pass implementation remains available only
  as a differential oracle and fallback. It is removed from normal execution
  once every supported construct and error contract matches.
- Existing atomic output publication, compare-before-write, module locking,
  interrupted-process recovery, and declared header ordering remain unchanged.

## Current Foundations and Gaps

- `parse_reflection_header` already constructs dataclass-based classes, enums,
  structs, and properties, but mixes libclang traversal with symbol-dependent
  property resolution.
- Export parsing builds nearly a complete `ReflectedHeaderInfo` and then discards
  everything except projected exported symbols.
- Reflection parsing repeats translation-unit creation and AST traversal after
  exports are ready.
- The existing reflected models are mostly serializable, but contain `Path`
  values and already-resolved properties rather than a stable raw semantic
  contract.
- `_DHT_EXPORTS_PARSER` changes the visible declaration set for mirror export
  types, so simply feeding the current export parse result into the current
  reflection writer would generate incorrect output.
- Existing tests strongly cover generated code and reflection semantics, but do
  not compare a unified IR projection against both legacy phases for every
  supported declaration form.

## Implementation Stages

### Stage 0: Freeze semantic and phase contracts

- [ ] Depend on the stable cache entry envelope, path ownership, and atomic I/O
  primitives selected by DHT Persistent Header Cache.
- [ ] Inventory every field read from libclang for classes, structs, enums,
  constructors, inheritance, scalar/object/enum/container properties, metadata,
  source locations, and generated-body locations.
- [ ] Inventory every `_DHT_EXPORTS_PARSER` conditional and make the reviewed
  CoreDObject mirror declarations the only accepted phase-specific case.
- [ ] Define the versioned semantic IR schema, explicit export-only marker,
  canonical ordering, and diagnostic location format.
- [ ] Capture legacy export, reflected model, generated source, manifest, and
  expected-error fixtures for every supported construct.

#### Acceptance Gate

- The IR schema is sufficient to project every current export and resolve every
  current reflected property without retaining or recreating libclang objects,
  and all phase-specific declarations have an explicit selected treatment.

### Stage 1: Separate AST extraction from semantic resolution

- [ ] Add typed raw IR models for headers, declarations, constructors,
  inheritance, enum values, properties, nested containers, metadata, and source
  diagnostics.
- [ ] Refactor libclang traversal to populate raw IR without requiring available
  module exports.
- [ ] Refactor exported-symbol extraction into a deterministic projection from
  raw IR.
- [ ] Refactor property/reference resolution into a pure IR-plus-symbol-table
  transformation that produces the existing reflection model consumed by
  writers.
- [ ] Add serialization round-trip tests and pure resolver tests for qualified,
  ambiguous, missing, object, enum, struct, array, map, and nested references.

#### Acceptance Gate

- One in-memory AST traversal can project the legacy export model and, after
  symbols are supplied, the legacy reflection model for all focused fixtures.

### Stage 2: Unified parser mode and differential qualification

- [ ] Add the explicit `ExportOnly` annotation/specifier and migrate mirror
  export declarations without changing their generated production C++ behavior.
- [ ] Parse each fixture once in the unified mode and compare its export
  projection with the legacy export-mode result.
- [ ] Resolve the same IR against fixture exports and compare reflected models,
  generated headers/sources, manifests, and diagnostics with the legacy
  reflection-mode result.
- [ ] Add repository-level differential tests over every currently reflected
  header and module, with stable normalization only for intentionally
  non-semantic timestamps or paths.
- [ ] Reject unsupported future phase-conditional reflected declarations with a
  clear configuration or parser diagnostic.

#### Acceptance Gate

- Unified parsing produces byte-identical exports and generated source output,
  equivalent manifests, and equivalent source diagnostics across the complete
  repository corpus.

### Stage 3: Export-produced IR pipeline

- [ ] Make each export parser task return raw IR plus its export projection.
- [ ] Persist the IR through the cache envelope established by DHT Persistent
  Header Cache before publishing the merged module export.
- [ ] Record an IR semantic digest in the private export manifest so Reflection
  can verify it is consuming the exact parse result associated with the module
  export.
- [ ] Preserve mixed cache-hit/parser-result assembly and declared header order.
- [ ] Ensure interrupted IR publication cannot pair a new export with stale or
  partial IR.

#### Acceptance Gate

- Export generation publishes the same module exports as before and leaves one
  complete, verified IR entry for every current reflected header.

### Stage 4: Reflection IR consumption and legacy-path retirement

- [ ] Load and validate header IR entries after own/dependency exports are ready.
- [ ] Resolve raw properties and inheritance against the available symbol table,
  generate output content, and preserve resolved-symbol dependency tracking
  without invoking libclang.
- [ ] On missing or damaged IR, perform one unified parser fallback, repair the
  entry, verify its export projection against the already published module
  export, and fail safely on a semantic mismatch.
- [ ] Instrument per-module IR hits, parser fallbacks, resolution time,
  generation time, and libclang parse counts.
- [ ] Remove the legacy second reflection parse from normal execution after the
  differential suite and full repository build pass.

#### Acceptance Gate

- Cold and one-header-invalidated builds invoke libclang exactly once for each
  affected header, while dependency-export-only changes invoke no libclang for
  unchanged headers.

### Stage 5: Performance qualification and handoff

- [ ] Run focused parser, extractor, resolver, writer, manifest, cache, CLI, and
  build-configuration tests.
- [ ] Measure cold/full-generation, warm rebuild, no-op, one-header change, and
  dependency-export-only workloads against the recorded two-pass baseline.
- [ ] Capture per-module parse count, Export time, Reflection resolution and
  generation time, total build wall time, CPU utilization, and peak memory.
- [ ] Complete a successful `all` build and full native-test run through the
  root build entrypoint, followed by the required hidden-window editor smoke
  test.
- [ ] Move the lasting unified-IR, ordering, cache identity, and diagnostic
  contracts into the DHT/build documentation.
- [ ] Record the baseline commit, working set, key schema/projection decisions,
  differential results, and remaining scheduling limits in the final handoff.

#### Acceptance Gate

- Engine cold Export plus Reflection time improves by at least 30% from the
  recorded 42.755-second baseline, every invalidated header has exactly one
  libclang parse, generated artifacts remain equivalent, and one-header
  incremental behavior does not regress.

## Validation Matrix

| Scenario | Required result |
| --- | --- |
| Cold reflected header | One unified libclang parse, export projection, persisted IR, later reflection generation. |
| Warm valid IR | No libclang parse; existing persistent-cache behavior applies. |
| One header changes | One parse for that header, with deterministic mixed-result module assembly. |
| Dependency export changes only | No header parse; affected IR is re-resolved and regenerated as required. |
| Export-only mirror declaration | Present in export projection and absent from generated reflection code. |
| Object/enum/struct reference | Pure resolver matches legacy qualified-symbol behavior. |
| Nested array/map property | Raw IR preserves nesting and generated property layout. |
| Ambiguous or missing symbol | Diagnostic matches legacy source file, line, and meaning. |
| IR is missing or corrupt | One unified fallback parse repairs IR or fails before inconsistent publication. |
| Export and IR digest disagree | Reflection fails safely and identifies the mismatched module/header. |
| Tool/schema/runtime/platform changes | IR invalidates deterministically. |
| Full repository differential run | Exports and generated C++ are byte-identical; manifests are semantically equivalent. |
| Interrupted export publication | No new export is paired with incomplete IR. |

## Definition of Done

- Every implementation stage and acceptance gate is complete.
- The legacy normal-path second libclang parse is removed.
- Automated tests prove one parse per invalidated header and zero parses for
  dependency-only re-resolution.
- Export projection, reflection models, generated artifacts, diagnostics, and
  failure recovery match the previous behavior.
- Performance gates pass on the same Agent Build Profile used for the baseline.
- DHT and build documentation owns the lasting semantic IR and phase-ordering
  contracts.
- The implementation commit records this plan and its completed stage
  provenance according to repository rules.

## Deferred Follow-ups

- Ninja-visible per-header/shard edges and a shared jobserver remain tracked by
  [DHT And Ninja Parallelism Coordination](../Investigations/DHTNinjaParallelismCoordination.md).
- Phase-specific Clang PCH, umbrella translation units, and persistent worker
  processes require separate measurement after duplicate parsing is removed.
- Narrow per-symbol re-resolution may replace whole-export-set invalidation only
  after the simpler resolver pipeline is qualified.

## Related Documentation

- [DHT Persistent Header Cache](Archive/2026-08/DHTPersistentHeaderCache.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [DHT And Ninja Parallelism Coordination](../Investigations/DHTNinjaParallelismCoordination.md)

## Related Code

- [`module_export_file_generator.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_export_file_generator.py)
- [`module_reflection_files_generator.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_reflection_files_generator.py)
- [`export_symbol_extractor.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/extractors/export_symbol_extractor.py)
- [`reflection_parser.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/reflection_parser.py)
- [`reflection_resolver.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/resolver/reflection_resolver.py)
- [`reflection_info.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/model/reflection_info.py)
- [`reflection_cache.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/cache/reflection_cache.py)
- [`ProjectTargets.cmake`](../../CMake/Project/ProjectTargets.cmake)
