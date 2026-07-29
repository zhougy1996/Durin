# Unify DurinDevTool Build Execution

## Outcome

Direct and interactive build commands use one action-dispatch and
failure-reporting pipeline, while request construction represents common
context and action-specific options without a flat mirror of unrelated fields.

This is a bounded internal-structure task, not a command-line, build, or shell
workflow redesign. Complete the required changes and validation as one outcome,
then delete this file in the implementation commit.

## Evidence

- `build/operations.py::execute_request` and `execute_shell_request` duplicate
  stop, create, preset-listing, status, runtime-opening, execution, and failure
  reporting branches. Their necessary difference is how an interactive session
  creates and reuses a `BuildContext`, but that distinction currently owns a
  second dispatcher.
- `build/config.py::CommandRequest` is a 32-field parameter bag spanning build,
  test, run, purge, and scaffolding operations. `build/handler.py` mirrors most
  of those fields in `NAMESPACE_FIELDS`, while later validation rejects invalid
  combinations.
- `tests/test_build_registry.py::
  test_direct_and_shell_tokens_produce_identical_namespaces` evaluates the same
  parser expression twice. The assertion is tautological and cannot detect
  drift between the real direct and interactive paths.

## Required Changes

1. Use one common action dispatcher and failure-reporting pipeline for direct
   and interactive build commands.
2. Isolate interactive preset selection and toolchain-context reuse behind a
   small context-acquisition boundary.
3. Reduce invalid `CommandRequest` states and the mirrored namespace mapping.
   Group common context/output options and action-specific payloads where that
   makes validation or dispatch simpler. Do not introduce a deep request-class
   hierarchy or one forwarding type per command.
4. Rewrite parity coverage so each side exercises its real entry path and the
   test fails when request construction or dispatch diverges.

## Protected Invariants

- Preserve interactive reuse of the captured toolchain environment,
  session-local preset selection, and per-command CMake and job overrides.
- Preserve direct command behavior, validation messages, exit codes, and
  Rich/plain failure rendering.
- Preserve stop, create, preset-listing, status, runtime-opening, and action
  behavior without changing command syntax or registry design.

## Likely Working Set

- `Tools/DurinDevTool/durin_dev_tool/build/operations.py`
- `Tools/DurinDevTool/durin_dev_tool/build/config.py`
- `Tools/DurinDevTool/durin_dev_tool/build/handler.py`
- `Tools/DurinDevTool/tests/test_build_registry.py`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- Direct and interactive commands share one dispatcher and retain identical
  observable behavior outside documented session-local context reuse.
- Request construction no longer requires a flat mirror of every unrelated
  action field, and invalid combinations remain rejected with actionable
  messages.
- Parity tests exercise distinct direct and interactive entry paths.
- Run the complete DurinDevTool Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Tools\DurinDevTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
