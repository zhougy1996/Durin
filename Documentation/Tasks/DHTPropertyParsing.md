# Unify DHT Property Parsing And Symbol Resolution

## Outcome

DurinHeaderTool resolves fallback property types deterministically and applies
one typed property-kind policy across AST-backed and source-spelling parsing,
with parsing responsibilities owned by the parser layer rather than the model.

This is a bounded parser-ownership and semantic-unification task, not a
reflection feature redesign. Complete the required changes and validation as
one outcome, then delete this file in the implementation commit.

## Evidence

- The source-spelling path in `model/reflection_info.py` gathers every exported
  struct or enum with a matching short name and accepts the first. With partial
  Clang information, two namespaces exporting the same short name can bind
  metadata according to insertion order.
- `model/reflection_info.py` owns data models, macro preprocessing, lexical
  source scanning, Clang initialization, AST traversal, symbol lookup, and
  property construction, while `parser/reflection_parser.py` is only a
  re-export.
- AST-backed and source-spelling property construction duplicate primitive,
  enum, struct, container, and object rules.
- Exported symbol collections contain `ExportedSymbolInfo` but are typed as
  `dict[str, object]`. Repeated `getattr` defaults hide the schema, including a
  fallback for the nonexistent `UnderlyingByteSize` field.

## Required Changes

1. Introduce one typed symbol-resolution path for AST-backed and
   source-spelling construction. Prefer qualified identity; accept a short name
   only when it has one valid candidate in the applicable context.
2. Surface or skip ambiguity according to the existing unsupported-property
   contract. Never select the first global suffix match.
3. Keep reflection data classes in the model layer and move Clang/source
   parsing behind the parser layer.
4. Share property-kind and symbol-resolution rules instead of maintaining two
   semantic implementations. Split only along real responsibilities; do not
   create new forwarding modules.
5. Replace `dict[str, object]` and speculative attribute fallbacks with the
   concrete exported-symbol type wherever the collection contract is known.
6. Add focused coverage for ambiguous short names and for matching AST-backed
   and source-spelling behavior.

## Protected Invariants

- Preserve partial Clang translation-unit support and source spellings needed
  when Clang canonicalizes Durin math aliases.
- Preserve physical-header marker ownership, flat generated filenames,
  deterministic diagnostics, nested-container limits, and
  configuration-independent metadata described in
  `Documentation/Runtime/Core/ReflectionSystem.md`.
- Do not turn DHT into a full C++ compiler, require diagnostic-free translation
  units, change reflection features, or change generated C++ schema.

## Likely Working Set

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/model/reflection_info.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/parser/`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/resolver/reflection_resolver.py`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/writers/reflection_source_writer.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/`

Expand this set only for a direct dependency or a required lasting contract
update. Do not edit generated DHT outputs by hand.

## Acceptance

- Ambiguous unqualified enum and struct names cannot bind by dictionary or
  module insertion order; qualified and uniquely resolvable names retain their
  current output.
- AST-backed and source-spelling extraction use one typed symbol and
  property-kind policy with focused coverage for both paths.
- Parsing implementation has a real parser-layer owner and the model no longer
  serves as a parser module.
- Run the complete DHT Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Engine\Source\Programs\DurinHeaderTool\tests
  ```

- Run a successful real C++ `all` build through DurinDevTool because generated
  headers and sources are part of the compile surface:

  ```powershell
  .\DevTool.bat build --target all
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
