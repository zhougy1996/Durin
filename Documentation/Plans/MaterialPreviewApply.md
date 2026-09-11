# Material Preview and Apply Plan

Summary: Isolate base-material graph editing and preview compilation from publication to the source material and its scene dependents.

Last reviewed: 2026-09-11

Status: Completed
Completed: 2026-09-11

## Current Status

Implementation is complete. Base documents use isolated transient working copies;
Apply publishes only a successfully compiled, unchanged request to the existing
source. Save applies then persists, and source conflicts fail without mutation.
Six new session tests and the existing async lifecycle fixture cover isolation,
dependent publication, failure, edit-during-Apply cancellation, automatic draft
scheduling, no-op Apply, relocation, Undo/Redo and draft-history retirement.
MaterialTests (146), MaterialThumbnailTests (8), AssetSaveReadinessTests (3),
AssetPackageReloadTests (11), EditorShellTests (54), and EditorAssetWorkflowTests
(39) passed: 261 tests total. MaterialEditor and DurinLauncher builds passed.
Changed-document validation passed; the completion validation receipt is recorded
with this handoff. Manual UI interaction and GPU execution were not performed.

## Goal

Base-material documents own an isolated working material. Compile and Auto
Compile affect that working material only. Apply publishes authored values to
the existing source object and requests compilation for its loaded dependents.
Save applies first and then saves the source package. Failed preview compilation
or an externally changed source prevents Apply. No source object replacement is
needed, so existing scene references remain valid.

Material-instance documents retain their existing live parameter-edit workflow.
They see base graph and default changes only after the base document applies.

## Implementation Stages

### Stage 0: Establish the implementation contract

- [x] Trace the shared source object through graph, Details, preview and save.
- [x] Select an isolated transient package for working-copy transactions.
- [x] Define Compile / Apply / Save and instance behavior above.
- [x] Validate and commit this plan before implementation (`78cfcfbd6`).

The working package uses a unique transient identity instead of a graph-private
replacement identity: Engine intentionally excludes prepared replacement graphs
from automatic compilation scans. A transient live owner participates in the
existing scheduler without exposing a persistent asset or changing that boundary.

### Stage 1: Isolate editing and integrate publication

Depends on Stage 0. Outcome: preview changes cannot alter the source until Apply.

- [x] Add a UI-independent working-copy session with strong lifetime ownership,
  authored-state comparison and optimistic source conflict detection.
- [x] Publish validated authored state through material mutation boundaries;
  coalesce source compilation and preserve last-good rendering on failure.
- [x] Route base graph, Details, diagnostics and preview to the working copy.
- [x] Add Apply and distinguish unapplied changes from compilation and disk dirtiness.
- [x] Integrate Save, Undo/Redo, close/discard, package reload, relocation,
  deletion and shutdown without retaining stale draft history or compile jobs.

### Stage 2: Validate and document

Depends on Stage 1. Outcome: automated evidence covers isolation and lifecycle.

- [x] Add focused tests for graph/default/static/presentation isolation, explicit
  publication, failed/pending compilation, no-op Apply and source conflicts.
- [x] Cover draft lifetime and discard, source dependents, and edit-after-Apply.
- [x] Run relevant material/editor tests and build the runnable editor using the
  [build workflow](../Agents/BuildAndRun.md) and
  [test workflow](../Agents/Testing.md).
- [x] Update [material graph operations](../Editor/Architecture/MaterialGraphOperations.md)
  with the implemented document contract and report any omitted manual UI checks.
- [x] Validate documentation and complete this plan with evidence.
