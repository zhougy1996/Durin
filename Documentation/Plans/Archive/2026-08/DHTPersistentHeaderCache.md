# DHT Persistent Header Cache Plan

Summary: Preserve versioned per-header DHT parse results across clean and rebuild operations while keeping generated outputs disposable and reproducible.

Last reviewed: 2026-08-04

Status: Archived
Completed: 2026-08-04

## Current Status

Stages 0 through 5 are complete. DHT now enforces the frozen hermetic parser
boundary: libclang receives the current reflected header, a versioned synthetic
prelude, and canonical exported-symbol data, with all ordinary includes
neutralized in-place.

The persistent storage layer now implements cache schema v1 as canonical UTF-8
JSON with typed export/reflection payloads, content and entry checksums, safe
logical-header hashing, native-libclang content identity, atomic latest-entry
replacement, stale-header cleanup, typed miss reasons, and aggregate diagnostics.
Export generation now consults the persistent per-header projection before it
schedules libclang work, publishes successful cold projections into the cache,
and reconstructs the module export in declared reflected-header order from
mixed manifest, persistent-cache, and parser results. Reflection generation now
keys each header by its content plus a canonical semantic digest of the complete
available export set, restores cached generated sources and dependency metadata,
and records content-stable dependency and generated-output digests in its
manifest.

The measured unchanged rebuild deleted 499 CMake-owned outputs and reparsed all
36 reflected headers in both phases. Engine spent 25.419 seconds in export
generation and 13.341 seconds in reflection generation for the same 29 headers.
The existing output manifests provide effective incremental reuse only while
the generated outputs survive; they are not an independent persistent parse
cache.

The production characterization regenerated all 36 headers. All five public
exports and all 36 generated headers remained byte-identical. Twelve generated
sources intentionally migrated non-fundamental element-size literals to target-
compiler `sizeof(SourceType)` expressions; this removes the old path's
include-dependent and sometimes invalid libclang layout values. The complete
`all` build passed with the new outputs.

The production Stage 3 acceptance run seeded 36 export entries across five
modules. After all five `.export` files and their manifests were deleted, an
`all` build reconstructed every output with 36/36 persistent hits, zero export
parses, and no SHA-256 differences across the ten rematerialized files.

The production Stage 4 acceptance run seeded 36 reflection entries across the
same five modules. After deleting the 72 per-header generated files, five module
generated sources, and five reflection manifests, the next `all` build reported
36/36 reflection hits, zero misses, and zero parses. All 82 reconstructed files
matched their canonical cold-path SHA-256, including manifests with cached
resolved-symbol dependency snapshots; cache diagnostics also restored the cold
class/property totals.

Stage 5 validated the complete artifact lifecycle. A normal clean removed 102
CMake-owned DHT outputs while preserving all 72 persistent entries byte-for-byte;
the following rebuild reconstructed both phases with 72/72 hits and zero parses.
The compatible `Win64-Debug-DurinEditor` preset reused the shared
`Win64/DurinEditor` entries through the checkout writer lock. A registered
project purge then removed the shared intermediate root, and the next build
reported 72/72 `not-found` misses, performed all 72 parses, and reseeded the
cache. Stage 5 handed the completed lifecycle contract to final performance,
documentation, native-test, and editor-smoke qualification.

Stage 6 performance qualification now passes. Across three runs per scenario,
the Engine warm-rebuild Export/Reflection median is 0.610 seconds, a 98.4%
improvement from the 38.760-second baseline, and the non-export-semantic
one-header build median is 7.01 seconds, 21.6% faster than the 8.94-second
baseline. Cold generation parsed and reseeded all 72 phase/header entries in
every run, while all three no-op builds remained at 0.38 seconds with no DHT
work. The full `all` build and five-tick hidden-window editor smoke pass, and the
lasting cache contract is now owned by the build, runtime-variant, and reflection
documentation.

Final qualification is complete. The remaining asset failures were not an
authored-asset schema change: Stage 1 exposed that AssetCore's serialized type
signature had incorrectly included process-local `ElementSize` for String,
Name, and Guid even though their payloads use logical encodings. AssetCore now
writes versioned wire signatures, recursively accepts the old ABI-sized forms,
and retains exact widths only for raw scalar and enum payloads. No authored
asset rewrite was required. All 850 runnable native tests pass and the Sandbox
editor initializes, opens its default content without compatibility errors,
ticks five times, and exits normally.

## Goal

Make an unchanged warm `rebuild --target all` reconstruct DHT outputs without
invoking libclang for already cached headers, while preserving deterministic
cold generation, one-header invalidation, atomic publication, and explicit
purge semantics.

## Scope

- Add a versioned, per-header persistent cache beneath the project intermediate
  root, partitioned by platform, runtime variant, module, and DHT phase.
- Establish a hermetic per-header parse boundary whose semantic inputs are the
  current reflected header, canonical exported-symbol data, and versioned DHT
  parser context rather than transitive include contents.
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
  cache miss; any future attempt should begin with a newly measured DHT redesign
  rather than extending this cache plan.
- Changing reflected C++ syntax, symbol naming, or runtime reflection behavior.
  Stage 1's one-time `sizeof(SourceType)` normalization is the required removal
  of parser-host layout from generated metadata; later cache stages preserve
  those canonical outputs byte-for-byte.
- Replacing Ninja, introducing a jobserver, or changing global compiler/DHT CPU
  scheduling.
- Sharing caches across worktrees, machines, platforms, or runtime variants.
- Retaining unbounded historical cache entries.
- Making ordinary non-reflected headers, third-party headers, or system headers
  part of DHT semantic identity. Reflected declarations that need such semantic
  state must instead use supported source spellings or exported reflected
  symbols.

## Design Decisions and Invariants

- The cache root is
  `<Project>/Intermediate/Build/<Platform>/<RuntimeVariant>/DHTCache/`. It is
  worktree-local, shared by compatible presets in that worktree, preserved by
  CMake clean/rebuild, and removed by the existing project-intermediate purge.
- Generated `.export`, manifest, `.gen.h`, `.gen.cpp`, and stamp files remain
  normal disposable build outputs. The cache is never declared as a CMake
  output or byproduct.
- One logical latest entry is owned per module, phase, and reflected header.
  Each entry is one self-contained UTF-8 JSON file committed by atomic
  replacement, so a killed writer cannot publish metadata separately from its
  payload.
- Every entry records schema and entry-kind versions, the repository DHT tool
  fingerprint, native libclang fingerprint, platform, runtime variant,
  parser/generator-context hash, module, logical header path, SHA-256 header
  content hash, canonical payload hash, and the phase-specific dependency
  digest. Timestamp and size may accelerate hashing but never define semantic
  identity.
- The parser context is hermetic. DHT preserves current-header source lines,
  neutralizes real include directives for extraction, and supplies only
  versioned built-in declarations plus canonical exported-symbol projections.
  An unsupported reflected declaration fails deterministically instead of
  acquiring semantic meaning from an ordinary included header.
- Export entries store only serializable exported-symbol data. Module export
  projections retain unresolved source spellings needed for module-level name
  resolution, so a cold build does not require the current module's final
  export to parse one of its headers. Module export ordering remains
  deterministic and follows the module's declared reflected header order.
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
  native parser, header content, or phase dependency semantic change invalidates
  the affected entry deterministically.

## Current Foundations and Gaps

- Export generation stores per-header fingerprints and serializable raw symbol
  projections in its private manifest. It can re-resolve unchanged raw
  projections when a dependency export changes, but the export and manifest are
  both deleted by clean.
- Reflection generation already stores header fingerprints and resolved-symbol
  dependencies, but missing generated outputs force regeneration and libclang
  parsing.
- File publication already uses compare-before-write and atomic replacement.
- DHT output directories already use cross-process writer locks.
- AST traversal accepts reflection declarations owned by the current physical
  header only. The translation unit has no real include search paths; source-
  spelling fallbacks, the versioned prelude, and exported symbols resolve the
  supported semantic model.
- Cold export extraction stores unresolved source base spellings per header,
  then resolves the complete module deterministically against same-module raw
  symbols and ordered dependency exports.
- CMake currently declares module exports, manifests, and generated sources as
  custom-command byproducts, so Ninja clean removes both the products and the
  data used for reuse.
- Existing tests cover the hermetic boundary, raw projection serialization,
  incremental manifests, and invalidation, but not a clean-output/warm-cache
  reconstruction workflow.

## Stage 0 Baseline And Frozen Contract

Baseline commit: `7fece783645a224ec7c7980c269adc5956871ddb`

Agent Build Profile: `windows-msvc-x64`, preset
`Win64-Debug-DurinEditor-Tests`, Ninja 1.12.1, Ninja `-j 18`, DHT pool depth 2,
and DHT worker ceiling 4. Measurements were captured on 2026-08-04 through the
root DurinDevTool entrypoint. Phase times are DurinHeaderTool elapsed times and
may overlap because Ninja can run two DHT commands concurrently.

| Scenario | Build result | DHT result |
| --- | --- | --- |
| Purged cold `rebuild --target all` | 131.72 s total; 21.85 s configure; 109.81 s build | 36/36 export parses and 36/36 reflection parses. Engine Export 31.670 s, Reflection 26.931 s. |
| Unchanged warm `rebuild --target all` | 74.32 s total; 0.75 s clean; 11.21 s configure; 62.31 s build; 499 outputs cleaned | 36/36 export parses and 36/36 reflection parses. Engine Export 25.419 s, Reflection 13.341 s. |
| No-op `build --target all` | 0.38 s total; 0.36 s build | Ninja reported no work; no DHT command ran. |
| One-header non-semantic Engine edit | 8.94 s total; 8.91 s build | Engine Export 1/29 in 0.898 s; Reflection 1/29 in 0.893 s. |

| Module | Headers | Cold Export | Cold Reflection | Warm Export | Warm Reflection |
| --- | ---: | ---: | ---: | ---: | ---: |
| CoreDObject | 2 | 5.032 s | 3.749 s | 1.848 s | 2.175 s |
| AssetCore | 2 | 7.954 s | 6.398 s | 3.078 s | 3.488 s |
| AssetImportCore | 1 | 2.063 s | 3.090 s | 1.905 s | 2.710 s |
| DurinEd | 2 | 9.896 s | 9.568 s | 7.784 s | 4.756 s |
| Engine | 29 | 31.670 s | 26.931 s | 25.419 s | 13.341 s |
| Phase sum | 36 | 56.615 s | 49.736 s | 40.034 s | 26.470 s |

The unchanged warm performance gate is therefore an Engine Export/Reflection
combined time below 11.628 seconds, which is a 70% improvement from 38.760
seconds. Stage 6 compares medians from at least three runs per measured scenario;
the single Stage 0 samples characterize the starting state and commands rather
than claiming machine-stable microbenchmark precision.

The persistent entry contract is schema v1:

- Cache root:
  `<Project>/Intermediate/Build/<Platform>/<RuntimeVariant>/DHTCache/`.
- Entry path:
  `<Module>/<export|reflection>/<HeaderKey>.json`, where `HeaderKey` is the
  lowercase SHA-256 of the normalized logical-header path encoded as UTF-8.
  Module names are validated repository identifiers and phase names are a closed
  enum; neither source value is used as an unchecked filesystem path.
- Logical-header normalization: convert separators to `/`, reject absolute
  paths, drive prefixes, empty paths, `.` and `..` segments, then preserve the
  case and normalized spelling declared by the owning module. The entry stores
  that spelling and validates it after lookup, so a hash collision or
  case-folding alias becomes a miss rather than cross-header reuse.
- Canonical JSON: UTF-8, sorted object keys, compact separators, no NaN or
  implementation-specific object encoding. Hashes use SHA-256 over these bytes.
- Common metadata: schema version, entry kind, DHT tool fingerprint, native
  libclang fingerprint, platform, runtime variant, module, logical header,
  header content digest, parser/generator-context digest, payload digest, and
  entry digest.
- Export payload: the complete serializable `ExportedSymbolInfo` projection for
  the header plus unresolved base/type spellings and declaration namespace
  needed for deterministic module-level resolution. Symbols are ordered by
  qualified identity when encoded and merged in the module's declared header
  order.
- Reflection payload: generated header and source text, class/property counts,
  and sorted resolved-symbol dependency snapshots. Its dependency key is the
  canonical digest of the final available exported-symbol map, including
  built-in intrinsic projections.
- The native parser fingerprint identifies the loaded libclang binary rather
  than assuming the pinned Python requirement alone identifies it. The chosen
  implementation may compute the binary SHA-256 during configuration and pass
  it with the existing DHT fingerprint, but cache validation consumes one stable
  value independent of file timestamps.
- A reader validates path containment, JSON shape and primitive types, every
  common key, phase dependency key, payload digest, and entry digest before
  publishing any generated output. Unknown schema/kind values, truncation,
  malformed JSON, checksum disagreement, or metadata mismatch log one warning
  and behave as an ordinary miss.
- A writer serializes one complete entry to a sibling temporary file, flushes
  and syncs it, then atomically replaces the latest entry while holding the
  existing module lock. A failed replacement leaves the previous entry usable;
  a damaged latest entry is reparsed and replaced on the next run.
- Module generation removes entries whose stored logical headers are no longer
  in the owning module's reflected-header set. One latest JSON file per current
  module/phase/header bounds persistent storage.

Hermetic extraction is part of the cache correctness contract. Only the current
reflected-header bytes, canonical available export projection, versioned
built-in parser prelude, and explicit platform/runtime/parser options may affect
the semantic result. Ordinary included-file contents are not keys. Reflected
markers, aliases, conditional declarations, and supported property spellings
must be recoverable from the current header or exported-symbol model; otherwise
DHT reports an unsupported semantic dependency. Stage 1 must prove this boundary
against the production reflected-header corpus before Stage 2 adds storage.

## Implementation Stages

### Stage 0: Baseline and cache contract

- [x] Capture one cold/full-generation rebuild, one unchanged warm rebuild, one
  no-op build, and one reflected-header incremental build using the registered
  Win64 Debug DurinEditor Tests preset.
- [x] Record DHT parse counts and per-module Export/Reflection times, Ninja
  version, Ninja job count, DHT pool depth, and DHT worker ceiling.
- [x] Enumerate every field needed to serialize exported symbols, reflection
  output content, counts, and resolved-symbol dependencies without Python object
  identity or libclang objects.
- [x] Freeze cache root ownership, filename normalization, schema versioning,
  canonical export digest, corruption behavior, and the focused Stage 2 test
  matrix in the explicit contract above.
- [x] Confirm the existing purge path contains the selected cache root and that
  no ordinary CMake clean rule owns it.

#### Acceptance Gate

- Baseline measurements are reproducible, the cache key completely describes
  the selected correctness inputs, and no cache path can escape the registered
  project intermediate root.

#### Stage 0 Handoff

- Baseline commit: `7fece783645a224ec7c7980c269adc5956871ddb`.
- Working set: this plan; no DHT implementation or lasting runtime contract has
  changed.
- Key symbols validated: `_parse_translation_unit`, `_clang_args`,
  `_fake_generated_headers`, `ExportedSymbolInfo`, `load_available_symbols`,
  `get_dht_module_lock_file_path`, and `collect_purge_paths`.
- Decisions: hermetic current-header/export semantics; SHA-256 path and content
  identity; one self-contained atomic JSON entry; complete available-export
  digest for safe initial reflection invalidation; semantic rather than
  timestamp dependency identity.
- Open implementation risk: Stage 1 must prove that the existing production
  reflected-header corpus does not rely on ordinary included aliases or macros
  that are absent from the selected semantic model.
- Validation outcome: purged cold rebuild, unchanged rebuild, no-op build,
  temporary one-header incremental build, and restoration build all completed
  successfully through DurinDevTool; the source tree was restored before this
  handoff.

### Stage 1: Hermetic per-header semantic boundary

- [x] Neutralize real include directives in the libclang extraction source while
  preserving physical line numbers and the current header's generated-body
  stubs.
- [x] Generate the versioned built-in parser prelude and synthetic reflected
  declarations from canonical exported-symbol data.
- [x] Split export extraction into a serializable raw per-header projection and
  deterministic module-level resolution so cold same-module references do not
  depend on a previously generated module export.
- [x] Load canonical dependency exports during module-level export resolution
  and add the required CMake ordering edges without making those exports inputs
  to the cached raw per-header projection.
- [x] Reject reflected aliases, macros, conditional declarations, or type
  spellings whose required meaning is absent from the current source, built-in
  prelude, or exported-symbol model.
- [x] Add tests proving that changing or removing an ordinary included header
  cannot change export/reflection extraction when the current header and export
  model are unchanged.
- [x] Characterize every production reflected header; preserve byte-identical
  public exports and generated headers, and record the required generated-source
  `sizeof(SourceType)` normalization rather than retaining parser-host layout
  literals.

#### Acceptance Gate

- DHT extraction opens no transitive header as a semantic input, public exports
  and generated headers preserve their existing bytes, non-fundamental layout
  is delegated to the target compiler, and unsupported non-hermetic dependencies
  fail with deterministic diagnostics before persistent reuse is enabled.

#### Stage 1 Handoff

- Baseline commit: `929cee95`.
- Working set: DHT parser/export model and generators, generated module CMake
  dependency metadata, focused DHT tests, this plan, and the implemented
  reflection-system contract.
- Key symbols: `_make_dht_parse_source`, `_synthetic_parser_prelude`,
  `_validate_preprocessor_context`, `resolve_module_export_info`,
  `RawSymbolsByHeader`, `load_dependency_symbols`, and
  `module_export_dependencies`.
- Decisions: ordinary includes are blanked without changing line count; raw
  header projections never depend on dependency exports; module-level base
  resolution consumes dependency exports after raw extraction; unsupported
  reflected meaning fails only in the strict reflection phase; non-fundamental
  property size uses target-compiler `sizeof`.
- Production characterization: 82 public/generated artifacts were compared to
  the Stage 0 baseline. Five exports, 36 `.gen.h` files, five module sources,
  and 24 header sources were byte-identical; the remaining 12 header sources
  contain only the recorded `sizeof(SourceType)` layout normalization.
- Open Stage 2 question: none; use parser context version `hermetic-v1` as an
  explicit cache context input.
- Validation outcome: 87 DHT tests passed; a full `all` build regenerated 36/36
  exports and 36/36 reflection outputs and compiled successfully through
  DurinDevTool. The characterized DHT phase totals were under 1.0 second for
  export and under 1.1 seconds for reflection on the Stage 0 profile.

### Stage 2: Persistent cache storage primitives

- [x] Add typed cache-entry models and explicit JSON/content serialization for
  export and reflection entries.
- [x] Implement canonical logical-header naming, safe cache paths, schema and
  fingerprint validation, single-file atomic replacement, and native libclang
  fingerprint validation.
- [x] Add bounded latest-entry replacement and stale-header cleanup under the
  module writer lock.
- [x] Add unit tests for round trips, ordering, incompatible schema/tool/runtime
  values, path containment, truncated content, and interrupted replacement.
- [x] Add structured cache hit, miss reason, materialization, and parse-count
  diagnostics without making per-header INFO output noisy.

#### Acceptance Gate

- Cache entries round-trip deterministically, invalid entries are treated as
  misses without damaging outputs, and storage remains bounded to the current
  reflected-header ownership set.

#### Stage 2 Handoff

- Baseline commit: `084954bf`.
- Working set: persistent header-cache storage, shared file/path helpers,
  obsolete libclang environment override cleanup, focused storage tests, and
  this plan.
- Key symbols: `PersistentHeaderCache`, `CacheEntryIdentity`,
  `ExportHeaderCachePayload`, `ReflectionHeaderCachePayload`,
  `CacheDiagnostics`, `normalize_logical_header`, and
  `fingerprint_native_libclang`.
- Decisions: schema v1 uses compact sorted-key UTF-8 JSON; payload and complete
  entry digests are independent SHA-256 checks; one context digest owns the
  phase-specific parser/generator contract; compatibility misses remain quiet
  and typed while malformed/checksum failures emit one warning; INFO diagnostics
  are aggregate-only; stale cleanup is an explicit primitive called while the
  existing module writer lock is held; the native-parser fingerprint hashes the
  actual binary selected by the pinned `libclang` Python package, with obsolete
  repository-local libclang path overrides removed.
- Open Stage 3 question: none; raw export entries use the canonical digest of an
  empty dependency set because dependency exports participate only in later
  module-level resolution.
- Validation outcome: 24 new persistent-cache tests pass, all 111 DHT tests
  pass, and the registered `all` build completes successfully. The cache is not
  connected to export/reflection generation in this stage, so no production
  cache entries or generated-output changes are expected.

### Stage 3: Export reconstruction cache

- [x] Consult the persistent per-header export entry before scheduling a parser
  task, even when the generated module export and output manifest are missing.
- [x] Serialize newly parsed symbol projections only after successful header
  extraction.
- [x] Reconstruct `ModuleExportInfo` in declared header order from mixed cache
  hits and parser results.
- [x] Continue publishing the module export and output manifest through existing
  atomic compare-before-write helpers.
- [x] Cover full hit, partial hit, header removal, header rename, tool change,
  damaged entry, and export-only symbol changes.

#### Acceptance Gate

- Deleting every generated export output while retaining the persistent cache
  reconstructs byte-identical exports with zero libclang parses for unchanged
  headers.

#### Stage 3 Handoff

- Baseline commit: `a65ca035`.
- Working set: module export generation, focused persistent export-cache tests,
  and this plan.
- Key symbols: `_export_cache_context_digest`,
  `_make_export_cache_identity`, `_build_module_export_from_cache`,
  `PersistentHeaderCache.read`, `PersistentHeaderCache.write`, and
  `CacheDiagnostics.log_summary`.
- Decisions: export identity hashes the current header bytes with SHA-256 and
  combines parser context version, generator options, and symbol-name scheme in
  the context digest; its dependency digest is the canonical empty-object
  digest because dependency exports participate only in later module-level
  resolution; the native parser binary is fingerprinted once per module
  invocation; persistent entries are written by the parent process only after
  every scheduled header extraction succeeds; mixed results are reordered to
  the module's declared reflected-header order before name resolution.
- Open Stage 4 question: none; reflection uses the already selected canonical
  semantic digest of the complete available export set as its dependency key.
- Validation outcome: six focused Stage 3 scenarios and all 117 DHT tests pass.
  A production `all` build seeded 36 export entries. After deleting all five
  module exports and five export manifests, the next `all` build reported 36
  hits, zero misses, and zero parses; all ten reconstructed files preserved
  their previous SHA-256. Reflection entries remain disconnected until Stage 4.

### Stage 4: Reflection reconstruction cache

- [x] Compute a canonical semantic digest for the complete available export set
  used by the safe initial reflection cache key.
- [x] Consult persistent reflection entries before scheduling parser tasks and
  materialize missing `.gen.h` and `.gen.cpp` outputs from validated entries.
- [x] Restore class/property counts and resolved-symbol dependency metadata from
  the cached result when rebuilding the module manifest.
- [x] Replace timestamp-only dependency-export identity in the reflection
  manifest with canonical content/semantic digests so clean rematerialization
  remains byte-identical.
- [x] Serialize new entries after successful parse, resolution, and source
  generation.
- [x] Cover output deletion, dependency export change, generator-option change,
  partial cache hits, corrupt content, and interrupted materialization.

#### Acceptance Gate

- Deleting every generated reflection output while retaining the persistent
  cache reconstructs byte-identical generated sources and manifests with zero
  libclang parses whenever the selected keys remain valid.

#### Stage 4 Handoff

- Baseline commit: `86bdec43`.
- Working set: module reflection generation, reflection manifest schema and
  serialization, focused persistent reflection-cache/recovery tests, and this
  plan.
- Key symbols: `_available_symbols_digest`,
  `_make_reflection_cache_identity`, `_write_reflection_files`,
  `ReflectionHeaderCachePayload`, `ModuleManifest.generated_output_digests`,
  and `save_module_manifest_file`.
- Decisions: the safe dependency key hashes the complete canonical available
  symbol set, including built-ins and the current module export when available;
  dependency-export manifest identity uses SHA-256 file content; reflection
  context combines parser context, generator options, and symbol-name scheme;
  the parent publishes a successful parse batch before atomically materializing
  outputs so an interrupted materialization can reuse completed entries; output
  digests detect damaged generated content; schema/tool/options changes do not
  reuse legacy cheap header fingerprints; all nested dependency snapshots are
  canonically sorted for cold/cache byte identity.
- Open Stage 5 question: none; the configured project intermediate root already
  places `DHTCache` outside module CMake output manifests, while registered
  project purge owns that root.
- Validation outcome: seven focused Stage 4 scenarios and all 124 DHT tests
  pass. A production `all` build seeded 36 reflection entries. After deleting
  82 reflection outputs/manifests, the next `all` build reported 36 hits, zero
  misses, and zero parses; all 82 reconstructed SHA-256 values matched the
  canonical cold path. Build log:
  `Build/.agent-state/logs/20260804-045103-353183-30820-cmake.log`.

### Stage 5: Clean, rebuild, and purge lifecycle

- [x] Update the CMake graph only as needed to keep generated outputs owned and
  ordered while ensuring the persistent cache is absent from clean metadata.
- [x] Add an integration test that warms the cache, removes CMake-owned DHT
  outputs, rebuilds them, and verifies zero parser calls plus identical content.
- [x] Verify a normal clean/rebuild preserves cache entries and a registered
  project purge removes them.
- [x] Verify presets sharing platform/runtime-variant metadata reuse compatible
  entries only under the existing single-writer checkout lock.
- [x] Verify interrupted cache writes and interrupted output materialization
  recover through an ordinary rerun without a manual cache deletion.

#### Acceptance Gate

- Clean/rebuild and purge exhibit distinct documented behavior: rebuild may
  reuse valid parse cache, while purge guarantees the next DHT generation is
  cold.

#### Stage 5 Handoff

- Baseline commit: `83331273`.
- Working set: combined export/reflection cache lifecycle coverage,
  DurinDevTool purge coverage, production clean/rebuild and shared-preset
  validation, and this plan. No CMake implementation change was required.
- Key symbols: `add_durin_module`, `collect_purge_paths`,
  `remove_purge_paths`, `_ReflectionHarness.generate_all`, and
  `_ReflectionHarness.cmake_owned_outputs`.
- Decisions: the existing custom-command stamps and `BYPRODUCTS` already give
  CMake ownership and ordering to every disposable DHT output without listing
  `DHTCache`; compatible presets share the platform/runtime-variant cache root
  while DurinDevTool retains one checkout writer lock; registered project purge
  owns the entire corresponding runtime intermediate root and therefore makes
  the next generation intentionally cold.
- Open Stage 6 question: none.
- Validation outcome: the combined lifecycle test and all 125 DHT tests pass;
  all 62 tests in `test_build_core.py` pass as part of the 70-test focused run.
  Normal clean removed 102 owned outputs but preserved 72 cache entries with
  aggregate SHA-256
  `6E09685FCFE92E83F269A0D4AFEACA7BF399E9A220757584CC23612EFAE3FC71`;
  the rebuild log
  `Build/.agent-state/logs/20260804-045838-539968-50832-cmake.log`
  reported 36/36 export and 36/36 reflection hits with zero parses. A freshly
  configured compatible preset reused all 36 reflection entries with zero
  parses in
  `Build/.agent-state/logs/20260804-050014-392655-22592-cmake.log`.
  Registered purge removed the shared intermediate root; the next build was
  cold for all 72 phase/header entries and reseeded them, as recorded in
  `Build/.agent-state/logs/20260804-050117-985915-56520-cmake.log`.

### Stage 6: Performance qualification and handoff

- [x] Run focused DurinHeaderTool tests and DurinDevTool build-lifecycle tests.
- [x] Compare medians from at least three cold generation, unchanged warm
  rebuild, no-op build, and non-export-semantic one-header incremental runs
  with the Stage 0 baseline.
- [x] Complete a successful `all` build and full native-test run through the
  root build entrypoint, then perform the required hidden-window editor smoke
  test.
- [x] Document cache ownership, invalidation, clean/rebuild behavior, purge
  behavior, diagnostics, and recovery in the owning build documentation.
- [x] Record the baseline commit, working set, key cache schema decisions,
  validation evidence, and any remaining cold-build limitation in the final
  stage handoff.

#### Acceptance Gate

- An unchanged warm rebuild parses zero reflected headers, the median Engine
  combined DHT Export/Reflection time improves by at least 70% from the recorded
  38.760-second warm rebuild baseline, cold generation remains correct, and the
  median non-export-semantic one-header incremental build regresses by no more
  than 10% from the recorded 8.94-second baseline.

#### Stage 6 Qualification Measurements

Agent Build Profile: `windows-msvc-x64`, preset
`Win64-Debug-DurinEditor-Tests`, Ninja `-j 18`, DHT pool depth 2, and DHT worker
ceiling 4. Values are three-run samples captured on 2026-08-04 through
DurinDevTool; DHT values are Engine Export plus Reflection elapsed time.

| Scenario | Run totals | Median | DHT result and baseline comparison |
| --- | --- | ---: | --- |
| Purged cold rebuild | 50.29 s, 48.07 s, 59.86 s | 50.29 s | Engine 2.084 s, 1.849 s, 1.945 s; median 1.945 s; every run parsed 36 exports and 36 reflections. |
| Unchanged warm rebuild | 51.05 s, 46.89 s, 49.80 s | 49.80 s | Engine 1.152 s, 0.610 s, 0.585 s; median 0.610 s; 72/72 hits and zero parses; 98.4% faster than the 38.760 s baseline. |
| No-op build | 0.38 s, 0.38 s, 0.38 s | 0.38 s | Ninja reported no work and scheduled no DHT command in all runs. |
| One-header non-semantic edit | 6.50 s, 7.03 s, 7.01 s | 7.01 s | Engine DHT 0.466 s, 0.456 s, 0.460 s; exactly one export and one reflection parse per run; total median is 21.6% faster than the 8.94 s baseline. |

#### Stage 6 Final Handoff

- Baseline commit: `5b5f32d1` after the completed plan history was squashed and
  rebased onto `dev` commit `857131cf`.
- Working set: owning build/runtime/reflection documentation, this plan, and
  performance/native/editor validation, plus the AssetCore serialized-signature
  correction and focused package tests. Temporary `Actor.h` measurement edits
  and material-preview fixture diagnostics were restored completely; authored
  assets were not changed.
- Key contracts: `DHTCache` is reconstruction-only data outside CMake clean
  ownership; project purge owns its enclosing runtime-variant intermediate
  root; entry compatibility includes tool, native-libclang, schema, platform,
  runtime, header, phase context, and reflection available-symbol identity;
  ordinary reruns recover interrupted publication or materialization.
- Cold-build limitation: cold generation still invokes libclang once per
  phase/header (72 parses total). It is correct and substantially faster than
  the Stage 0 include-expanded path. Cross-phase parse reuse remains deliberately
  unresolved and requires a fresh DHT redesign if it becomes material.
- Performance evidence logs: cold
  `20260804-050739-997123-57620-cmake.log`,
  `20260804-050842-901535-47232-cmake.log`, and
  `20260804-050945-292460-23896-cmake.log`; warm
  `20260804-051057-811800-23544-cmake.log`,
  `20260804-051155-615068-24680-cmake.log`, and
  `20260804-051254-678100-29324-cmake.log`; incremental
  `20260804-051432-634021-50588-cmake.log`,
  `20260804-051458-884828-34052-cmake.log`, and
  `20260804-051521-061433-50380-cmake.log`. All are under
  `Build/.agent-state/logs/`.
- Validation outcome: 358 DHT and DurinDevTool tests pass. The final post-fix
  `all` build passed in `20260804-054814-038541-17932-cmake.log`; focused
  `FPackageAssetTests` passed 33/33 in
  `20260804-054801-682583-58024-AssetPackageTests.log`; the complete native run
  passed all 850 runnable tests in
  `20260804-054822-245495-22284-ctest.log` (one fixture-dependent test skipped
  and one benchmark disabled). The Sandbox editor initialized without asset or
  level compatibility errors, ticked five times, and exited normally in
  `20260804-054852-663962-14212-DurinEditor.log`.

## Validation Matrix

| Scenario | Required result |
| --- | --- |
| Cold cache and missing outputs | Existing full parser path runs and seeds valid entries. |
| Warm cache and missing outputs | Outputs are materialized with zero libclang parses. |
| No-op build | Ninja schedules no DHT work. |
| One reflected header changes without changing its export projection | Only that header's export/reflection entries miss. |
| One reflected header changes its export projection | That header's export entry misses; the safe complete-export digest invalidates every reflection entry using the changed available export set. |
| Dependency export semantics change | Export entries remain independently reusable; every reflection entry keyed by the changed complete available export set misses. |
| Ordinary included header changes | DHT semantic identity is unchanged; current-header/export cache entries remain valid. Normal C++ dependency rebuilding is unaffected. |
| Timestamp changes but content does not | Content identity remains a cache hit after hashing. |
| Tool/native-libclang/schema/runtime/platform changes | Incompatible entries miss deterministically. |
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

- If cross-phase parse reuse becomes material, design a new DHT restructuring
  plan from the then-current measurements and parser/cache contracts.
- Coordinating hidden parser workers with Ninja remains tracked by
  [DHT And Ninja Parallelism Coordination](../../../Investigations/DHTNinjaParallelismCoordination.md).
- Per-symbol reflection cache invalidation may replace the safe whole-export-set
  key after correctness and hit-rate measurements justify the added complexity.
- Cross-worktree or remote cache sharing requires a separate trust, eviction,
  and compatibility design.

## Related Documentation

- [Build System](../../../Development/Build/BuildSystem.md)
- [Build And Run](../../../Development/Build/BuildAndRun.md)
- [Runtime Variants](../../../Development/Build/RuntimeVariants.md)
- [Reflection System](../../../Runtime/Core/ReflectionSystem.md)
- [DHT And Ninja Parallelism Coordination](../../../Investigations/DHTNinjaParallelismCoordination.md)

## Related Code

- [`CMake/Project/ProjectTargets.cmake`](../../../../CMake/Project/ProjectTargets.cmake)
- [`CMake/Project/ProjectSetup.cmake`](../../../../CMake/Project/ProjectSetup.cmake)
- [`module_export_file_generator.py`](../../../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_export_file_generator.py)
- [`module_reflection_files_generator.py`](../../../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_reflection_files_generator.py)
- [`reflection_cache.py`](../../../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/cache/reflection_cache.py)
- [`file_helper.py`](../../../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/io/file_helper.py)
- [`output_lock.py`](../../../../Engine/Source/Programs/DurinHeaderTool/durin_header_tool/io/output_lock.py)
