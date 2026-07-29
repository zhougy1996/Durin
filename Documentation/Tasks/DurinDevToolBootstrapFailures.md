# Preserve DurinDevTool Bootstrap Failure Identity

## Outcome

DurinDevTool reports expected bootstrap and prerequisite failures concisely
without hiding unexpected programming errors that happen to raise
`RuntimeError`.

This is a bounded error-boundary task, not a bootstrap workflow redesign.
Complete the required changes and validation as one outcome, then delete this
file in the implementation commit.

## Evidence

`bootstrap/handler.py` catches every `RuntimeError` because preflight code uses
the raw base type for expected prerequisite failures. An unrelated defect
raising the same type is therefore flattened into an ordinary user-facing error
without its traceback.

## Required Changes

1. Introduce explicit bootstrap or preflight domain exceptions for expected
   operational failures.
2. Raise those exceptions from known prerequisite and setup failure paths.
3. Catch only the expected domain exceptions at the handler boundary. Allow
   unexpected `RuntimeError` instances and other programming errors to retain
   their traceback.
4. Add focused tests for both an expected preflight failure and an unexpected
   `RuntimeError`.

## Protected Invariants

- Preserve existing concise messages and exit behavior for missing tools,
  unsupported prerequisites, and expected setup failures.
- Preserve bootstrap command syntax and successful setup behavior.
- Do not broaden exception handling or reclassify unrelated failures.

## Likely Working Set

- `Tools/DurinDevTool/durin_dev_tool/bootstrap/handler.py`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/preflight.py`
- `Tools/DurinDevTool/tests/test_bootstrap_worktree.py`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- Expected setup and prerequisite failures remain concise user-facing errors.
- An unexpected `RuntimeError` is not silently reclassified and retains its
  traceback.
- Run the complete DurinDevTool Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Tools\DurinDevTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
