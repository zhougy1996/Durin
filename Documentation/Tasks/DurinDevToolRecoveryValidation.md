# Clarify DurinDevTool Recovery Validation Ownership

## Outcome

DurinDevTool validates recovery targets in one clear layer and keeps recovery
marker cleanup behavior explicit without no-op exception branches.

This is a bounded recovery-code cleanup, not a recovery workflow or Windows
lock-handling redesign. Complete the required changes and validation as one
outcome, then delete this file in the implementation commit.

## Evidence

- `build/recovery.py::execute_with_recovery_marker` contains an
  `except BaseException: raise` branch that changes no behavior.
- Recovery target validation is split between
  `build/core.py::execute_context` and the marker transaction because the
  target must be discovered before executing the action.

The separate Windows checkout-lock findings remain owned by
`Documentation/Investigations/DurinDevToolWindowsLockRecovery.md` and are out
of scope.

## Required Changes

1. Remove the no-op exception branch without changing marker cleanup.
2. Give recovery-target discovery and validation one clear owner before the
   marker transaction executes the action.
3. Add or adjust focused tests that distinguish validation failure, ordinary
   command failure, interruption, and successful recovery.

## Protected Invariants

- Preserve configure, build, test, and rebuild interruption-marker semantics
  described in `Documentation/Development/Build/BuildAndRun.md`.
- Preserve marker restoration on ordinary failure and cleanup on success.
- Do not weaken, generalize, or partially implement the Windows lock recovery
  investigation.

## Likely Working Set

- `Tools/DurinDevTool/durin_dev_tool/build/core.py`
- `Tools/DurinDevTool/durin_dev_tool/build/recovery.py`
- `Tools/DurinDevTool/tests/test_build_core.py`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- Recovery-target validation has one owner and produces the existing
  actionable failures before action execution.
- Removing the no-op exception branch does not change marker semantics.
- Run the complete DurinDevTool Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Tools\DurinDevTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
