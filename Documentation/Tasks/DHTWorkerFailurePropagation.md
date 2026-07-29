# Preserve DHT Worker Failure Identity

## Outcome

DurinHeaderTool reports a reflection worker failure once with its original
exception and traceback instead of retrying the whole module sequentially.

This is a bounded correctness task, not a worker scheduling redesign. Complete
the required changes and validation as one outcome, then delete this file in
the implementation commit.

## Evidence

`module_reflection_files_generator._write_reflection_files` catches every
exception raised while collecting process-pool results and reparses every
header sequentially. Parser, resolver, and writer defects are therefore
misclassified as parallel-infrastructure failures, successful work is repeated,
and the original traceback is reduced to a warning before the failing operation
runs a second time.

## Required Changes

1. Let parser, resolver, and writer exceptions returned by `Future.result()`
   propagate with their original traceback.
2. If process-pool startup retains a sequential fallback, restrict it to
   explicit infrastructure failures before worker tasks begin.
3. Add focused regression coverage proving that a worker task exception is
   observed once and does not trigger whole-module sequential retry.

## Protected Invariants

- Preserve worker limits, deterministic diagnostic and output ordering, output
  locking, and successful parallel generation.
- Preserve atomic generated-file replacement, unchanged-content write elision,
  cache invalidation, and stale-output cleanup.
- Do not change generated C++ schema or Ninja/DHT scheduling policy.

## Likely Working Set

- `Engine/Source/Programs/DurinHeaderTool/durin_header_tool/generators/module_reflection_files_generator.py`
- `Engine/Source/Programs/DurinHeaderTool/tests/`

Expand this set only for a direct dependency or a required lasting contract
update. Do not edit generated DHT outputs by hand.

## Acceptance

- A worker task exception is observed once with its originating failure and
  traceback.
- Successful completed work is not repeated after a worker failure.
- Run the complete DHT Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Engine\Source\Programs\DurinHeaderTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
