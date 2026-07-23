# BuildTool Windows Lock Recovery Boundary

**Status:** Open  
**Last reviewed:** 2026-07-24

## Scope And Verdict

The checkout ownership mechanism is not Windows-only. BuildTool needs one
checkout-wide lock on every supported host, and the existing `msvcrt.locking`
and `fcntl.flock` branches provide that portable contract.

The recovery added for an inaccessible persistent lock file is Windows-specific:

- ACL normalization through `icacls`;
- PowerShell recovery commands;
- replacing an inaccessible lock on the assumption that Windows denies rename
  while the live owner has the file open.

That recovery must remain behind an explicit Windows boundary. It must not be
copied to POSIX hosts: POSIX permits renaming or unlinking an open, locked file,
which would allow a second inode at `checkout.lock` and split checkout
ownership.

`close_fds=True` is separate from ACL recovery. It is a portable defense that
prevents BuildTool-owned descriptors and handles from leaking to CMake, Ninja,
tests, or the runtime process and should remain enabled on every host.

Relevant implementation:

- [`core.py`](../../Engine/Scripts/Build/durin_build_tool/core.py), especially
  `open_checkout_lock`, `recover_inaccessible_windows_lock`,
  `normalize_windows_lock_acl`, `BuildToolLock`, and `run_command`;
- [`test_agent_tooling.py`](../../Engine/Scripts/Tests/test_agent_tooling.py);
- [`BuildAndRun.md`](../Setup/BuildAndRun.md#recovery).

## Verified Findings

### P2 — Windows recovery guidance leaks through the portable error path

`open_checkout_lock` calls `inaccessible_lock_error` after any
`PermissionError`, regardless of host. `inaccessible_lock_error` always emits
`icacls` and `Remove-Item` PowerShell commands.

The destructive rename helper and ACL reset helper themselves check
`os.name == "nt"`, so the current implementation does not rename a POSIX lock.
However, a POSIX permission failure still receives unusable Windows recovery
instructions. The repository currently registers only a Windows build profile,
but `BuildAndRun.md` documents direct non-Windows invocation, so this is a real
cross-platform contract defect rather than merely dead code.

**Impact:** a future Linux or macOS profile encountering an inaccessible lock
will be told to run commands that do not exist and will not receive host-correct
recovery guidance.

**Recommended direction:** keep `open_checkout_lock` platform-neutral and route
permission failures to a Windows-specific recovery function only when the
resolved host is Windows. Other hosts should report the path and permission
failure without deleting or renaming the lock automatically.

### P2 — `stop` does not use the improved permission diagnosis

`stop_active_operation` first calls `lock_is_owned`. That function still turns
all lock-file `OSError` failures into the older generic “Could not open
BuildTool lock” error and does not use the ACL-specific recovery explanation.

**Impact:** the command most likely to be used while diagnosing a stuck
operation gives less actionable output than a normal `build`, `test`, or `run`
startup against the same inaccessible file.

**Recommended direction:** share one host-aware lock-open diagnostic between
normal acquisition and ownership probing. Ownership probing must remain
read-only and must not invoke stale-file replacement.

### P2 — Safety relies on a Windows file-sharing invariant without an integration test

The stale-file recovery is safe only if a live BuildTool's Python file handle
does not share delete access. This was verified manually on Windows:

1. process A opened a file with Python `open("r+b")` and acquired the first-byte
   `msvcrt` lock;
2. process B attempted to rename the file;
3. Windows rejected the rename because the file was in use.

This supports the current implementation: an inaccessible abandoned file can be
renamed, while a live BuildTool file cannot. The existing automated tests mock
the recovery result and therefore do not guard this OS-level premise.

**Impact:** a future change in how the lock handle is opened or shared could
make rename succeed against a live owner, creating two lock files and allowing
concurrent checkout writers.

**Recommended direction:** add a Windows-only subprocess integration test that
holds the real byte lock, verifies rename failure while the holder is alive,
then verifies replacement and reacquisition after it exits. Keep the fast mocked
unit tests for branch coverage.

## Confirmed Correct Behavior

- The OS byte lock, not the metadata PID or physical file presence, is the
  ownership authority. PID metadata is useful for diagnostics and `stop`, but
  PID reuse and the acquire-before-metadata-write interval make it unsafe as the
  sole stale-lock test.
- A normal process exit releases the byte lock even though `checkout.lock`
  remains on disk.
- Waiting for the Windows job's active-process count keeps `BuildTool run`
  alive until the last relaunched editor process exits.
- `close_fds=True` prevents child-process lifetime from extending BuildTool's
  ownership handle and is appropriate on both Windows and POSIX.
- ACL normalization is best-effort. Failure to obtain `WRITE_DAC` must not
  invalidate a byte lock that was successfully acquired.

## Additional Risks And Validation Gaps

- The quarantine filename contains only the current PID. A leftover
  `checkout.lock.<pid>.stale` from PID reuse can prevent automatic recovery.
  A unique suffix or collision retry would make cleanup more robust.
- Cross-identity ACL behavior cannot be fully represented by same-identity unit
  tests. Validation should include a Windows sandbox identity that creates the
  lock and a different identity that subsequently opens it.
- The documented manual recovery enables inheritance recursively and then
  removes the lock. It must continue to require an explicit check that
  BuildTool, DurinEditor, CMake, and Ninja have exited before deletion.

## Reproduction And Acceptance

The implementation is ready to close this issue when all of the following hold:

1. Non-Windows permission errors contain no PowerShell or `icacls` guidance and
   never rename or unlink the persistent lock automatically.
2. Windows startup and `stop` distinguish byte-lock contention from an
   inaccessible lock ACL.
3. A Windows integration test proves that a live owner blocks stale-file rename
   and that an abandoned inaccessible file can be replaced without splitting
   ownership.
4. Existing lock exclusivity, ACL best-effort, child-handle, and BuildTool
   tooling tests continue to pass.
