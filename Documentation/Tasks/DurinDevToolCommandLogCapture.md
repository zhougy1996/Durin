# Harden DurinDevTool Command Log Capture

## Outcome

DurinDevTool establishes the complete command log before launching a child
process, so an unavailable log destination fails promptly with the original
diagnostic and a verbose child can never block on an undrained stdout pipe.

This is a bounded correctness task, not a child-process or output-mode
redesign. Complete the required changes and validation as one outcome, then
delete this file in the implementation commit.

## Evidence

`build/process.py::run_command` currently starts the child before its
output-reader thread opens the command log. If opening the log fails, the
reader records an `OSError` and exits without draining stdout. A sufficiently
verbose child then blocks on the full pipe while the main thread keeps waiting.

A controlled review reproduction used a large-output child and an invalid log
destination. A one-second command timeout took 11.15 seconds to return and was
reported as a timeout rather than the capture failure. Ordinary build commands
have no internal timeout, so the equivalent failure can wait indefinitely.

## Required Changes

1. Open the command log synchronously before starting the child and keep that
   established stream available to the output reader. If the full-log contract
   cannot be established, do not launch the child.
2. Report the log path and original `OSError`. Do not reclassify a capture
   failure as a command timeout or ordinary command failure.
3. Add focused regression coverage using an unavailable or unwritable log
   destination and a child capable of filling stdout. Prove that the child is
   not launched and cannot be left running.

## Protected Invariants

- Preserve compact, progress, and full child-output behavior, complete command
  logs, bounded failure excerpts, 30-second heartbeats, and log retention.
- Preserve Ctrl+C handling, process-tree termination, Windows Job Object
  descendant waiting, timeout behavior, and `close_fds=True`.
- Do not change command syntax, recovery-marker semantics, or build behavior.

## Likely Working Set

- `Tools/DurinDevTool/durin_dev_tool/build/process.py`
- `Tools/DurinDevTool/tests/test_build_core.py`

Expand this set only for a direct dependency or a required lasting contract
update.

## Acceptance

- An unavailable or unwritable command-log destination fails promptly with the
  log path and original capture error.
- The failure occurs before `subprocess.Popen`; no child is launched or left
  running, and no stdout pipe can block the caller.
- Existing output, interruption, timeout, and process-job tests continue to
  pass.
- Run the complete DurinDevTool Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Tools\DurinDevTool\tests
  ```

- Validate changed documentation, inspect the final status and diff, then
  delete this task file in the implementation commit. Do not archive it or add
  Plan provenance.
