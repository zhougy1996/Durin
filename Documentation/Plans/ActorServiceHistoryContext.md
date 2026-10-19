# Actor Service And History Context Plan

Summary: Remove Actor service host selection and production test modes, then make transaction Context queries authoritative.

Last reviewed: 2026-10-19

Status: Completed
Completed: 2026-10-19

## Current Status

Both stages are complete. Actor context carries read-only policy only; the
initialized service uses its active editor and its history. Production test
injection and hostless execution are removed. Shared headless fixtures initialize
actual editor services through the existing engine test-access boundary, and
package-backed tests activate their Levels before editing. Stage 1 is committed
as `5b0f32df8` after its editor, structural, World and viewport checks passed.

Stage 2 makes Context queries virtual and cursor-based, removes transactor
description methods, and migrates production/test consumers in all workspace
projects. Workspace/window labels return owned strings; MainFrame reads each
label once per menu. Added coverage exercises virtual dispatch, cursor movement,
recording/cancellation, branch replacement, reset and label lifetime.

Final validation used Win64-Debug-DurinEditor outside the sandbox: 1,133 tests
passed across 20 targets, with no failures. ContentBrowserWorkflowTests has two
existing symlink cases skipped because Windows file/directory symlink privileges
are unavailable; these do not exercise the changed ownership/query behavior.
Coverage includes editor operations (133), shell (59), structural mutation (51),
World (175), viewport (101), content browser/property/asset workflows, splines,
material editing, Python/MCP, package reload, RoadWeaver, Sandbox and header checks.
Exact receipts and counts are preserved outside rolling log retention at
`Build/ActorServiceHistoryContextValidation/receipts.md`. The final production all
build passed; receipt:
`Build/ActorServiceHistoryContextValidation/20261019-120454-306386-16396-cmake.log`.

Tested source is `5b0f32df8` plus the Stage 2 implementation and fixtures in this
completion commit. Only documentation/evidence changed after qualification.
Changed-document/all-plan validation and diff/removed-symbol checks passed.
Named CPU suites were selected after affected-test inspection; unrelated
render/GPU expansion is outside this cleanup's acceptance scope.

## Selected Decisions

- Actor edit context carries document policy only. The initialized subsystem
  resolves its owning active editor and validates the target World/Level.
- Delete TestTransactions, TestActorEditor and hostless operation branches.
  Shared headless native fixtures initialize real editor services and use the
  editor-owned history; lower-level command changes remain independently testable.
- GetUndoContext/GetRedoContext become the canonical virtual history queries,
  with direct Cursor-based buffer implementations. Remove transactor description
  queries and migrate UI adapters without returning views into temporary contexts.
- Keep this task separate from tool-result compatibility, startup activation,
  public recording metadata and complete UE serialization work.

## Implementation Stages

### Stage 0: Record selected cleanup and acceptance

- [x] Record ownership, migration and validation decisions.
- [x] Validate and commit this plan before implementation.

### Stage 1: Unify Actor service ownership

Depends on Stage 0.

- [x] Remove Host selection, ForeignHost errors and test-only production modes.
- [x] Migrate all workspace consumers and structural/picking tests to real
  headless editor service lifetimes, including recovery and retirement cases.
- [x] Update Actor contracts and pass EditorOperationTests, WorldTests and
  ViewportTests before committing this stage.

### Stage 2: Canonicalize history contexts and qualify integration

Depends on Stage 1.

- [x] Implement virtual Context queries and migrate descriptions across all
  Engine, Sandbox and RoadWeaver source/test consumers, preserving string lifetime.
- [x] Cover query behavior at Undo/Redo boundaries, during recording and after
  branching/reset; update owning contracts.
- [x] Inspect affected-test selection and pass relevant editor, World, viewport,
  property/material, Python/MCP and project consumer coverage.
- [x] Build all production targets once at final acceptance because shared
  virtual history interfaces and UI return types cross editor module boundaries.
- [x] Validate changed documents/all plans, review removed symbols and the
  final diff, record evidence and commit the completed plan.

Use the existing Win64-Debug-DurinEditor profile and the repository
[build](../Agents/BuildAndRun.md) and [test](../Agents/Testing.md) workflows.
Application-hosted and GPU qualification are outside this ownership/query change.
