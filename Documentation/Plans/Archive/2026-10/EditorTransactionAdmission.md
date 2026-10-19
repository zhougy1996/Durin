# Editor Transaction Admission Plan

Summary: Remove property UI and material command transactor selection and make unrecorded editing explicit.

Last reviewed: 2026-10-19

Status: Archived
Completed: 2026-10-19

## Current Status

Implementation and qualification are complete against 791352c82 plus
Stage 1 changes. Property UI contexts select an explicit mode, ordinary material
commands resolve the editor at admission, and the move gesture retains a private
submission path to its original service. Tests distinguish standalone fixture
construction from recorded editor commands. EditorPropertyTests passed 69 cases.
Shared test headers expand affected coverage to all Engine tests; named CPU
consumer targets cover the changed admission/UI behavior, while all production
targets cover shared exported API integration. MaterialQualificationTests compiled
successfully without timing/GPU execution.

Win64-Debug-DurinEditor validation passed all production targets and 18 CPU test
targets: 1105 cases passed, zero failed or skipped. Coverage includes explicit
admission rejection without an editor, recorded Undo/Redo, unrecorded standalone
editing and outer-recording rejection, preview cancellation, domain rollback,
gesture retry, package saved-state restoration and Engine/Sandbox/RoadWeaver
consumers. New editor-owned test fixtures establish saved checkpoints where the
real document-open contract requires them; dirty-state assertions remain intact.

The final all build receipt is 20261019-134638-279083-40052-cmake.log and the
qualification compilation receipt is 20261019-134659-916043-17172-cmake.log.
Per-target receipts and the manifest are preserved locally under
Build/EditorTransactionAdmissionValidation/receipts.md. Later changes corrected
consumer-test migration and documentation only; production inputs and each
passing target's relevant fixtures remain covered by their final receipts.

## Selected Decisions

- Property UI contexts carry a transaction mode, not a service pointer. Normal
  edits resolve the active editor at admission; unavailable services reject edits.
- NotTransactable explicitly skips recording and rejects an existing GUndo to
  avoid silently mixing tracked and untracked writes. NonTransactional property
  metadata retains its property-specific exclusion behavior.
- Material document and ordinary static command parameters accept the same mode
  instead of nullable transactors. UI callers use the recorded default. Preserve
  domain validation and rollback snapshots in unrecorded mode.
- Property sessions retain exact scope ownership. Keep an explicit non-null
  low-level transactor overload for native composition. Existing material gesture
  and working-copy service ownership is outside this command-entry migration.
- Tests of editor commands use real editor-owned history; low-level mutation
  tests explicitly request unrecorded mode. Do not add test-only production routes.

## Implementation Stages

### Stage 0: Record admission and history semantics

- [x] Record selected scope, failure policy and validation gates.

### Stage 1: Migrate command admission and consumers

- [x] Implement explicit admission and migrate property UI/actions/customizations.
- [x] Remove service parameters from ordinary material command consumers across
  Engine, Sandbox and RoadWeaver; retain exact gesture bindings internally.
- [x] Test default recording, unavailable editor rejection, explicit unrecorded
  editing, outer-recording rejection and normal Undo/Redo/rollback behavior.
- [x] Update property, material and transaction contracts.
- [x] Inspect affected coverage and pass relevant CPU editor/property, material,
  viewport, World, asset, texture, spline and project consumers.
- [x] Build all production targets once to cover shared exported API and UI
  consumer changes across editor modules not represented by bounded tests.
- [x] Validate documents/all plans, preserve final-code receipts and commit.

Follow [build](../../../Agents/BuildAndRun.md) and [test](../../../Agents/Testing.md)
workflows. No GPU or application-hosted behavior changes are intended.
