# Remove Redundant DHT Config And Resolver Paths

## Outcome

DurinHeaderTool loads module symbols and validated configuration through one
clear path without duplicate current-module work, repeated initialization, or
unreachable truthiness branches.

This is a bounded control-flow cleanup, not a configuration or dependency
resolution redesign. Complete the required changes and validation as one
outcome, then delete this file in the implementation commit.

## Evidence

- `reflection_resolver.load_available_symbols` loads the current module export
  before iterating a dependency list that already includes the current module.
- Module and project config loading contains repeated `__post_init__` calls and
  truthiness branches after helpers that either return a value or raise.

## Required Changes

1. Load the current module export exactly once while preserving dependency
   ordering and symbol visibility.
2. Remove repeated manual `__post_init__` calls where dataclass construction
   already performs initialization.
3. Remove branches that are unreachable under the helper's return-or-raise
   contract.
4. Add focused coverage where existing tests do not demonstrate the
   single-load and validation behavior.

## Protected Invariants

- Preserve project and module configuration schema, diagnostics, and defaults.
- Preserve dependency resolution order, deterministic symbol ordering, cache
  behavior, and generated output.
- Do not change reflection features or build-graph behavior.

## Likely Working Set

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/config/`
- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/resolver/reflection_resolver.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/`

Expand this set only for a direct dependency or a required lasting contract
update. Do not edit generated DHT outputs by hand.

## Acceptance

- The current module export is loaded once even when present in the dependency
  list.
- Config construction performs validation once and contains no unreachable
  post-load truthiness checks.
- Existing configuration, resolver, cache, and generation tests continue to
  pass.
- Run the complete DHT Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Engine\Source\Programs\DurinHeaderTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
