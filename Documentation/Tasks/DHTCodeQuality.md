# Simplify DurinHeaderTool Code Quality

## Outcome

DurinHeaderTool fails once with the original diagnostic when parsing fails,
resolves fallback property types without guessing between ambiguous symbols,
and has one clear implementation path for its active CLI, parsing, and I/O
responsibilities.

This is a bounded code-quality task, not a reflection feature redesign. Complete
the required changes and validation as one outcome, then delete this file in
the implementation commit.

## Evidence

The current implementation has two correctness-relevant fallback problems:

- `module_reflection_files_generator._write_reflection_files` catches every
  exception raised while collecting process-pool results and reparses every
  header sequentially. Parser, resolver, and writer defects are therefore
  misclassified as parallel-infrastructure failures, successful work is
  repeated, and the original traceback is reduced to a warning before the
  failing operation runs a second time.
- The source-spelling property path in `model/reflection_info.py` gathers all
  exported structs or enums with a matching short name and accepts the first
  one. When partial Clang information activates this fallback, two namespaces
  exporting the same short name can bind metadata according to export insertion
  order rather than C++ lookup or an explicit ambiguity rule.

The same area also carries avoidable structure and dead code:

- `model/reflection_info.py` owns data models, macro preprocessing, lexical
  source scanning, Clang initialization, AST traversal, symbol lookup, and
  property construction, while `parser/reflection_parser.py` is only a
  re-export. AST-backed and source-spelling property construction duplicate
  primitive, enum, struct, container, and object rules.
- The active symbol collection contains `ExportedSymbolInfo` values but is
  typed as `dict[str, object]` across the parser, resolver, and writer. Repeated
  `getattr` defaults hide the real schema, including a fallback for the
  nonexistent `UnderlyingByteSize` field.
- `io/file_helper.py` and `io/json_helper.py` expose unused helpers including
  `FileCacheEntry`, `get_file_fingerprint`, `verify_file_fingerprint`,
  `is_file_changed`, `calculate_file_hash`, `parse_json_content`,
  `_pascal_to_snake`, and `dict_from_dataclass`. `calculate_file_hash` uses
  Python's process-randomized `hash`, so it is unsuitable for a persistent
  fingerprint if accidentally reused.
- The CLI wraps three real commands in `Command`, `CommandManager`, and a
  pass-through `run` method, while also registering an unused `EmptyCommand`.
  Argparse already owns command validation and can dispatch directly.
- `reflection_resolver.load_available_symbols` loads the current module export
  before iterating a dependency list that already includes the current module.
  Module and project config loading also contains repeated `__post_init__`
  calls and truthiness branches after helpers that either return a value or
  raise.

## Required Changes

1. Make reflection worker failures fail once with their original exception and
   traceback. If process-pool startup retains a sequential fallback, restrict it
   to explicit infrastructure failures before worker tasks begin; do not catch
   parser, resolver, or writer exceptions returned by `Future.result()`.
2. Introduce one typed symbol-resolution path for AST and source-spelling
   property construction. Prefer qualified identity, accept a short name only
   when it has one valid candidate in the applicable context, and surface or
   skip ambiguity according to the existing unsupported-property contract.
   Never select the first global suffix match.
3. Give parsing code real ownership boundaries. Keep reflection data classes in
   the model layer; move Clang/source parsing behind the parser layer; share
   property-kind and symbol resolution rather than maintaining two independent
   semantic implementations. Split only along these responsibilities—do not
   create additional forwarding modules.
4. Replace `dict[str, object]` and speculative attribute fallbacks with the
   concrete exported-symbol type wherever the collection contract is known.
5. Remove unused I/O helpers and package exports. Simplify CLI dispatch and the
   small unreachable/repeated config and resolver paths identified above.
6. Add focused regression coverage for a worker task exception and ambiguous
   short-name fallback resolution. Preserve existing cache, file I/O, locking,
   generation, and build-configuration coverage.

## Protected Invariants

- Keep atomic generated-file replacement, unchanged-content write elision,
  paired export/manifest cache invalidation, corrupt-cache regeneration,
  missing-output regeneration, pending stale-output cleanup, and output locks.
- Keep support for partial Clang translation units and source spellings needed
  when Clang canonicalizes Durin math aliases. Removing duplicated parsing must
  not turn DHT into a full C++ compiler or require diagnostic-free translation
  units.
- Preserve physical-header marker ownership, flat generated filenames,
  deterministic diagnostics and output ordering, nested-container limits, and
  configuration-independent generated metadata described in
  `Documentation/Runtime/Core/ReflectionSystem.md`.
- Do not change reflection features, generated C++ schema, CMake graph shape,
  worker limits, or Ninja/DHT scheduling policy as part of this task.

## Likely Working Set

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/model/reflection_info.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/resolver/reflection_resolver.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_reflection_files_generator.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/cli/`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/io/`
- `Engine/Source/Programs/DurinHeaderTool/tests/`

Expand this set only for a direct dependency or a required lasting contract
update. Do not edit generated DHT outputs by hand.

## Acceptance

- A worker task exception is observed once and retains the originating failure
  rather than triggering a whole-module sequential retry.
- Ambiguous unqualified enum and struct names cannot bind to a candidate by
  dictionary or module insertion order; qualified and uniquely resolvable names
  retain current generated output.
- AST-backed and source-spelling property extraction use one shared symbol and
  property-kind policy, with focused tests covering both paths.
- Removed helpers and CLI scaffolding have no remaining imports or package
  exports.
- Run the complete DHT Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Engine\Source\Programs\DurinHeaderTool\tests
  ```

- Run a successful real C++ `all` build through DurinDevTool because generated
  headers and sources are part of the compile surface:

  ```powershell
  .\DevTool.bat build --target all
  ```

- Inspect the final status and diff, update any lasting reflection contract
  affected by the implementation, then delete this task file in the same
  commit. Do not archive it or add Plan provenance.
