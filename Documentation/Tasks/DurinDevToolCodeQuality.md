# Harden And Simplify DurinDevTool Code Quality

## Outcome

DurinDevTool fails promptly when child-output capture cannot be established,
uses one clear execution pipeline for direct and interactive build commands,
reports expected bootstrap failures without hiding unexpected programming
errors, and shares identical transaction or link-preparation behavior instead
of maintaining parallel fallback implementations.

This is a bounded code-quality task, not a command-line, build-recovery, or
worktree workflow redesign. Complete the required changes and validation as one
outcome, then delete this file and its entry from
`Documentation/Tasks/README.md` in the implementation commit.

## Evidence

The current child-process output path contains a correctness defect:

- `build/process.py::run_command` starts the child before its output-reader
  thread opens the command log. If opening the log fails, the reader records an
  `OSError` and exits without draining stdout. A sufficiently verbose child
  then blocks on the full pipe while the main thread continues waiting. A
  controlled review reproduction made a large-output child use an invalid log
  destination: a one-second command timeout took 11.15 seconds to return and
  reported a timeout instead of the capture failure. Build commands normally
  have no internal timeout, so the equivalent failure can wait indefinitely.

The command and error paths also carry avoidable structure:

- `build/operations.py::execute_request` and `execute_shell_request` duplicate
  stop, create, preset-listing, status, runtime-opening, execution, and failure
  reporting branches. Their necessary difference is how an interactive session
  creates and reuses a `BuildContext`, but that distinction currently owns a
  second dispatcher.
- `build/config.py::CommandRequest` is a 32-field parameter bag spanning build,
  test, run, purge, and scaffolding operations. `build/handler.py` mirrors most
  of those fields in `NAMESPACE_FIELDS`, while later validation rejects invalid
  combinations. Adding an option therefore requires coordinated edits across
  the registry, namespace adapter, request model, and action validation.
- `tests/test_build_registry.py::
  test_direct_and_shell_tokens_produce_identical_namespaces` assigns `direct`
  and `shell` by evaluating the same parser expression twice. The assertion is
  tautological and cannot detect drift between the direct and interactive
  execution paths it claims to protect.
- `bootstrap/handler.py` catches all `RuntimeError` instances because preflight
  code uses raw `RuntimeError` for expected prerequisite failures. An unrelated
  implementation defect raising the same base type is therefore flattened into
  an ordinary user-facing error without its traceback.

Several lower-risk paths duplicate already-selected behavior:

- `documentation/archive.py::apply_archive` has its own snapshot, write, delete,
  validation, and rollback transaction even though
  `documentation/changes.py` already owns fingerprint-checked
  `DocumentChangeSet` application for Markdown moves and rewrites. The two
  transaction paths now have different precondition and rollback semantics.
- `worktree/services.py::prepare_agent_link` and `prepare_vscode_link` are the
  same preserve-to-`.pre-link-backup` and link operation with different labels
  and relative paths.
- `build/recovery.py::execute_with_recovery_marker` includes an
  `except BaseException: raise` branch that changes no behavior. Recovery target
  validation is also split between `build/core.py::execute_context` and the
  marker transaction because the target must be discovered before executing
  the action.

The Windows checkout-lock findings remain owned by
`Documentation/Investigations/DurinDevToolWindowsLockRecovery.md`. Do not copy
or partially resolve that investigation through this task; preserve its
platform boundary and acceptance requirements.

## Required Changes

1. Establish command-log capture before starting a child process, or otherwise
   guarantee that stdout continues to be drained after a logging failure. If
   the full-log contract cannot be established, fail before launching the child
   and report the log path and original `OSError`. Do not misreport a capture
   failure as a command timeout or command failure.
2. Use one common action-dispatch and failure-reporting pipeline for direct and
   interactive build commands. Isolate interactive preset selection and
   toolchain-context reuse behind a small context-acquisition boundary rather
   than duplicating action branches.
3. Reduce invalid `CommandRequest` states and the mirrored namespace mapping.
   Group common context/output options and action-specific payloads only where
   that makes validation or dispatch simpler. Do not replace the parameter bag
   with a deep request-class hierarchy or one forwarding type per command.
4. Introduce explicit bootstrap/preflight domain exceptions. Catch only
   expected operational failures at the handler boundary; allow unexpected
   programming errors to retain their traceback.
5. Rewrite the direct-versus-interactive parity coverage so each side exercises
   its real entry path. Add focused regression coverage for a command-log open
   failure with a child that can fill stdout, proving that the child is not
   launched or cannot block the caller.
6. Express plan archival through the existing document change transaction, or
   extract one shared transaction primitive used by both archive and ordinary
   document changes. Preserve preview data and post-apply validation while
   eliminating divergent rollback and precondition behavior.
7. Replace the duplicated Agent and VS Code preparation functions with one
   parameterized preserve-and-link helper. Remove the no-op recovery exception
   branch and give recovery-target validation one clear owner without changing
   marker semantics.

## Protected Invariants

- Preserve compact, progress, and full child-output behavior, complete command
  logs, bounded failure excerpts, 30-second heartbeats, log retention, test
  summaries, Ctrl+C handling, process-tree termination, Windows Job Object
  descendant waiting, and `close_fds=True`.
- Preserve interactive-session reuse of the captured toolchain environment,
  session-local preset selection, per-command CMake and job overrides, direct
  command behavior, exit codes, and Rich/plain failure rendering.
- Preserve configure/build/test/rebuild interruption-marker semantics described
  in
  `Documentation/Development/Build/BuildAndRun.md`.
- Preserve atomic Markdown updates, reference repair, rollback after validation
  failure, archive previews, and the rule that plan archival changes completed
  status to archived status.
- Preserve non-empty local `.agents` and `.vscode` directories under their
  `.pre-link-backup` names, idempotent worktree preparation, dry-run output, and
  safe junction handling during worktree removal.
- Do not weaken or generalize the Windows lock recovery described in
  `Documentation/Investigations/DurinDevToolWindowsLockRecovery.md`.
- Do not change command syntax, add dependencies, redesign the registry, or
  change CMake/build behavior as part of this task.

## Likely Working Set

- `Tools/DurinDevTool/durin_dev_tool/build/process.py`
- `Tools/DurinDevTool/durin_dev_tool/build/operations.py`
- `Tools/DurinDevTool/durin_dev_tool/build/config.py`
- `Tools/DurinDevTool/durin_dev_tool/build/handler.py`
- `Tools/DurinDevTool/durin_dev_tool/build/core.py`
- `Tools/DurinDevTool/durin_dev_tool/build/recovery.py`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/handler.py`
- `Tools/DurinDevTool/durin_dev_tool/bootstrap/preflight.py`
- `Tools/DurinDevTool/durin_dev_tool/documentation/archive.py`
- `Tools/DurinDevTool/durin_dev_tool/documentation/changes.py`
- `Tools/DurinDevTool/durin_dev_tool/worktree/services.py`
- `Tools/DurinDevTool/tests/`

Expand this set only for a direct dependency or a required lasting contract
update. Keep the lock investigation separate unless its own acceptance
requirements are explicitly selected for implementation.

## Acceptance

- An unavailable or unwritable command-log destination fails promptly with the
  original capture error. A verbose child cannot block on an undrained stdout
  pipe, and no child is left running after any output-capture failure.
- Direct and interactive build commands share one action dispatcher and retain
  identical observable behavior outside documented session-local context reuse.
- Request construction no longer requires a flat mirror of every unrelated
  action field, and invalid option combinations remain rejected with actionable
  messages.
- Expected setup and prerequisite failures remain concise user-facing errors;
  an unexpected `RuntimeError` is not silently reclassified as an expected
  bootstrap failure.
- Direct-versus-interactive coverage exercises distinct entry paths and fails
  when either request construction or action dispatch diverges.
- Plan archival and ordinary document changes share transaction semantics, and
  existing write-failure and post-validation rollback tests continue to pass.
- Agent and VS Code directory preservation uses one implementation, with
  existing dry-run, backup-conflict, idempotency, and link tests passing.
- Run the complete DurinDevTool Python suite:

  ```powershell
  .\.venv\Scripts\python.exe -m pytest Tools\DurinDevTool\tests
  ```

- Validate the final documentation changes:

  ```powershell
  .\DevTool.bat doc validate --scope changed
  ```

- Inspect the final status and diff, update any lasting build or documentation
  contracts affected by the implementation, then delete this task file and its
  open-task index entry in the same commit. Do not archive it or add Plan
  provenance.
