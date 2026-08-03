# DHT Persistent Header Cache Plan

Summary: Preserve versioned per-header DHT parse results across clean and rebuild operations while keeping generated outputs disposable and reproducible.

Last reviewed: 2026-08-03

Status: Active
Completed:

## Current Status

The direction is selected and implementation has not started. A measured
Win64 Debug DurinEditor Tests rebuild deleted 820 CMake-owned outputs and then
reparsed every reflected header. The Engine module spent 22.921 seconds in
export generation and 19.834 seconds in reflection generation for the same 29
headers. The existing output manifests provide effective incremental reuse only
while the generated outputs survive; they are not an independent persistent
parse cache.

Stage 0 owns the reproducible baseline and freezes the cache schema before code
changes begin.

## Goal

Make an unchanged warm `rebuild --target all` reconstruct DHT outputs without
invoking libclang for already cached headers, while preserving deterministic
cold generation, one-header invalidation, atomic publication, and explicit
purge semantics.

## Scope

- Add a versioned, per-header persistent cache beneath the project intermediate
  root, partitioned by platform, runtime variant, module, and DHT phase.
- Cache the export-symbol projection required to reconstruct a module export.
- Cache the reflection result required to reconstruct each generated header and
  source, including counts and resolved-symbol dependency metadata.
- Separate cache ownership from CMake `OUTPUT` and `BYPRODUCTS` ownership so a
  normal clean or rebuild may delete generated outputs without deleting cache
  entries.
- Treat missing, stale, incompatible, or damaged cache entries as ordinary
  misses that fall back to the existing parser.
- Add cache hit, miss, materialization, parse-count, and elapsed-time diagnostics.
- Keep project purge as the operation that removes the platform/runtime-variant
  intermediate root and therefore the persistent cache.

## Non-Goals

- Eliminating the separate Export and Reflection libclang parses on a true cold
  cache miss; that work belongs to the
  [DHT Single-Parse Semantic Pipeline](DHTSingleParseSemanticPipeline.md).
- Changing reflected C++ syntax, generated C++ layout, symbol naming, or runtime
  reflection behavior.
- Replacing Ninja, introducing a jobserver, or changing global compiler/DHT CPU
  scheduling.
- Sharing caches across worktrees, machines, platforms, or runtime variants.
- Retaining unbounded historical cache entries.

## Design Decisions and Invariants

- The cache root is
  `<Project>/Intermediate/Build/<Platform>/<RuntimeVariant>/DHTCache/`. It is
  worktree-local, shared by compatible presets in that worktree, preserved by
  CMake clean/rebuild, and removed by the existing project-intermediate purge.
- Generated `.export`, manifest, `.gen.h`, `.gen.cpp`, and stamp files remain
  normal disposable build outputs. The cache is never declared as a CMake
  output or byproduct.
- One logical latest entry is owned per module, phase, and reflected header.
  Atomic replacement bounds storage while preventing a killed writer from
  exposing partial JSON or content payloads.
- Every entry records a schema version, tool fingerprint, platform, runtime
  variant, generator-options hash, module, logical header path, and header
  content hash. Timestamp and size may accelerate hashing but never define
  semantic identity.
- Export entries store only serializable exported-symbol data. Module export
  ordering remains deterministic and follows the module's declared reflected
  header order.
- Reflection entries store generated header/source content, class/property
  counts, and resolved-symbol dependency metadata. Initial correctness uses a
  canonical semantic digest of the complete available export set as the safe
  dependency key; finer per-symbol reuse may be considered only after the safe
  contract is validated.
- Cache reads never mutate published outputs until the entry has been fully
  decoded and validated. Cache writes occur only after a successful parse and
  generation result exists in memory.
- Existing module output locking owns cache mutation as well as generated-output
  publication. Readers never observe a partially replaced entry.
- Cache corruption is recoverable. DHT logs the rejected entry at warning level,
  reparses the header, replaces the entry atomically, and continues.
- Compare-before-write remains authoritative for generated outputs so cache
  materialization does not cause unnecessary downstream recompilation when an
  existing output is already identical.
- A DHT tool fingerprint, schema, platform, runtime variant, generator option,
  header content, or relevant export semantic change invalidates the affected
  entry deterministically.

## Current Foundations and Gaps

- Export generation already stores per-header fingerprints and can reuse symbol
  subsets from the current module export, but the export and its manifest are
  one cache entry and both are deleted by clean.
- Reflection generation already stores header fingerprints and resolved-symbol
  dependencies, but missing generated outputs force regeneration and libclang
  parsing.
- File publication already uses compare-before-write and atomic replacement.
- DHT output directories already use cross-process writer locks.
- CMake currently declares module exports, manifests, and generated sources as
  custom-command byproducts, so Ninja clean removes both the products and the
  data used for reuse.
- Existing tests cover incremental manifests and invalidation, but not a
  clean-output/warm-cache reconstruction workflow.

## Implementation Stages

### Stage 0: Baseline and cache contract

- [ ] Capture one cold/full-generation rebuild, one unchanged warm rebuild, one
  no-op build, and one reflected-header incremental build using the registered
  Win64 Debug DurinEditor Tests preset.
- [ ] Record DHT parse counts and per-module Export/Reflection times, Ninja
  version, Ninja job count, DHT pool depth, and DHT worker ceiling.
- [ ] Enumerate every field needed to serialize exported symbols, reflection
  output content, counts, and resolved-symbol dependencies without Python object
  identity or libclang objects.
- [ ] Freeze cache root ownership, filename normalization, schema versioning,
  canonical export digest, and corruption behavior in focused tests.
- [ ] Confirm the existing purge path contains the selected cache root and that
  no ordinary CMake clean rule owns it.

#### Acceptance Gate

- Baseline measurements are reproducible, the cache key completely describes
  the selected correctness inputs, and no cache path can escape the registered
  project intermediate root.

### Stage 1: Persistent cache storage primitives

- [ ] Add typed cache-entry models and explicit JSON/content serialization for
  export and reflection entries.
- [ ] Implement canonical logical-header naming, safe cache paths, schema and
  fingerprint validation, and atomic replacement.
- [ ] Add bounded latest-entry replacement and stale-header cleanup under the
  module writer lock.
- [ ] Add unit tests for round trips, ordering, incompatible schema/tool/runtime
  values, path containment, truncated content, and interrupted replacement.
- [ ] Add structured cache hit, miss reason, materialization, and parse-count
  diagnostics without making per-header INFO output noisy.

#### Acceptance Gate

- Cache entries round-trip deterministically, invalid entries are treated as
  misses without damaging outputs, and storage remains bounded to the current
  reflected-header ownership set.

### Stage 2: Export reconstruction cache

- [ ] Consult the persistent per-header export entry before scheduling a parser
  task, even when the generated module export and output manifest are missing.
- [ ] Serialize newly parsed symbol projections only after successful header
  extraction.
- [ ] Reconstruct `ModuleExportInfo` in declared header order from mixed cache
  hits and parser results.
- [ ] Continue publishing the module export and output manifest through existing
  atomic compare-before-write helpers.
- [ ] Cover full hit, partial hit, header removal, header rename, tool change,
  damaged entry, and export-only symbol changes.

#### Acceptance Gate

- Deleting every generated export output while retaining the persistent cache
  reconstructs byte-identical exports with zero libclang parses for unchanged
  headers.

### Stage 3: Reflection reconstruction cache

- [ ] Compute a canonical semantic digest for the complete available export set
  used by the safe initial reflection cache key.
- [ ] Consult persistent reflection entries before scheduling parser tasks and
  materialize missing `.gen.h` and `.gen.cpp` outputs from validated entries.
- [ ] Restore class/property counts and resolved-symbol dependency metadata from
  the cached result when rebuilding the module manifest.
- [ ] Serialize new entries after successful parse, resolution, and source
  generation.
- [ ] Cover output deletion, dependency export change, generator-option change,
  partial cache hits, corrupt content, and interrupted materialization.

#### Acceptance Gate

- Deleting every generated reflection output while retaining the persistent
  cache reconstructs byte-identical generated sources and manifests with zero
  libclang parses whenever the selected keys remain valid.

### Stage 4: Clean, rebuild, and purge lifecycle

- [ ] Update the CMake graph only as needed to keep generated outputs owned and
  ordered while ensuring the persistent cache is absent from clean metadata.
- [ ] Add an integration test that warms the cache, removes CMake-owned DHT
  outputs, rebuilds them, and verifies zero parser calls plus identical content.
- [ ] Verify a normal clean/rebuild preserves cache entries and a registered
  project purge removes them.
- [ ] Verify presets sharing platform/runtime-variant metadata reuse compatible
  entries only under the existing single-writer checkout lock.
- [ ] Verify interrupted cache writes and interrupted output materialization
  recover through an ordinary rerun without a manual cache deletion.

#### Acceptance Gate

- Clean/rebuild and purge exhibit distinct documented behavior: rebuild may
  reuse valid parse cache, while purge guarantees the next DHT generation is
  cold.

### Stage 5: Performance qualification and handoff

- [ ] Run focused DurinHeaderTool tests and DurinDevTool build-lifecycle tests.
- [ ] Compare cold generation, unchanged warm rebuild, no-op build, and
  one-header incremental measurements with the Stage 0 baseline.
- [ ] Complete a successful `all` build and full native-test run through the
  root build entrypoint, then perform the required hidden-window editor smoke
  test.
- [ ] Document cache ownership, invalidation, clean/rebuild behavior, purge
  behavior, diagnostics, and recovery in the owning build documentation.
- [ ] Record the baseline commit, working set, key cache schema decisions,
  validation evidence, and any remaining cold-build limitation in the final
  stage handoff.

#### Acceptance Gate

- An unchanged warm rebuild parses zero reflected headers, Engine's combined
  DHT Export/Reflection time improves by at least 70% from the recorded warm
  rebuild baseline, cold generation remains correct, and a one-header
  incremental build regresses by no more than 10%.

## Validation Matrix

| Scenario | Required result |
| --- | --- |
| Cold cache and missing outputs | Existing full parser path runs and seeds valid entries. |
| Warm cache and missing outputs | Outputs are materialized with zero libclang parses. |
| No-op build | Ninja schedules no DHT work. |
| One reflected header changes | Only that header's phase entries miss, subject to safe dependency invalidation. |
| Dependency export semantics change | Affected reflection entries miss; unrelated export entries remain reusable. |
| Timestamp changes but content does not | Content identity remains a cache hit after hashing. |
| Tool/schema/runtime/platform changes | Incompatible entries miss deterministically. |
| Entry is truncated or malformed | Warning, parser fallback, atomic replacement, correct outputs. |
| Generated output is damaged | Valid cache rematerializes the correct output. |
| Cache write is interrupted | Previous complete entry remains usable or the next run reparses. |
| CMake clean/rebuild | Cache survives and reconstructs deleted DHT outputs. |
| Project purge | Cache is removed and the next generation is cold. |
| Shared compatible presets | Cache reuse remains serialized by checkout/module ownership. |

## Definition of Done

- Every implementation stage and acceptance gate is complete.
- Cache format, containment, invalidation, corruption, locking, and lifecycle
  behavior have automated coverage.
- Generated exports, reflection sources, and manifests are byte-identical
  between cache-hit and cold-parser paths.
- Measured performance gates pass on the same Agent Build Profile used for the
  baseline.
- Build and DHT documentation owns the lasting cache, rebuild, purge, and
  recovery contracts.
- The implementation commit records this plan and its completed stage
  provenance according to repository rules.

## Deferred Follow-ups

- Reusing one semantic parse across Export and Reflection is tracked by
  [DHT Single-Parse Semantic Pipeline](DHTSingleParseSemanticPipeline.md).
- Coordinating hidden parser workers with Ninja remains tracked by
  [DHT And Ninja Parallelism Coordination](../Investigations/DHTNinjaParallelismCoordination.md).
- Per-symbol reflection cache invalidation may replace the safe whole-export-set
  key after correctness and hit-rate measurements justify the added complexity.
- Cross-worktree or remote cache sharing requires a separate trust, eviction,
  and compatibility design.

## Related Documentation

- [Build System](../Development/Build/BuildSystem.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Runtime Variants](../Development/Build/RuntimeVariants.md)
- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [DHT And Ninja Parallelism Coordination](../Investigations/DHTNinjaParallelismCoordination.md)

## Related Code

- [`CMake/Project/ProjectTargets.cmake`](../../CMake/Project/ProjectTargets.cmake)
- [`CMake/Project/ProjectSetup.cmake`](../../CMake/Project/ProjectSetup.cmake)
- [`module_export_file_generator.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_export_file_generator.py)
- [`module_reflection_files_generator.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_reflection_files_generator.py)
- [`reflection_cache.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/cache/reflection_cache.py)
- [`file_helper.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/io/file_helper.py)
- [`output_lock.py`](../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/io/output_lock.py)
