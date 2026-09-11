# Material Preview and Apply Plan

Summary: Isolate base-material graph editing and preview compilation from publication to the source material and its scene dependents.

Last reviewed: 2026-09-11

Status: Active
Completed:

## Current Status

The existing editor edits and compiles the loaded source directly. Investigation
confirmed that graph operations, property transactions, preview rendering, and
document saving share that object. Implementation has not started.

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
- [x] Select an isolated, graph-private package for working-copy transactions.
- [x] Define Compile / Apply / Save and instance behavior above.
- [ ] Validate and commit this plan before implementation.

### Stage 1: Isolate editing and integrate publication

Depends on Stage 0. Outcome: preview changes cannot alter the source until Apply.

- [ ] Add a UI-independent working-copy session with strong lifetime ownership,
  authored-state comparison and optimistic source conflict detection.
- [ ] Publish validated authored state through material mutation boundaries;
  coalesce source compilation and preserve last-good rendering on failure.
- [ ] Route base graph, Details, diagnostics and preview to the working copy.
- [ ] Add Apply and distinguish unapplied changes from compilation and disk dirtiness.
- [ ] Integrate Save, Undo/Redo, close/discard, package reload, relocation,
  deletion and shutdown without retaining stale draft history or compile jobs.

### Stage 2: Validate and document

Depends on Stage 1. Outcome: automated evidence covers isolation and lifecycle.

- [ ] Add focused tests for graph/default/static/presentation isolation, explicit
  publication, failed/pending compilation, no-op Apply and source conflicts.
- [ ] Cover draft lifetime and discard, source dependents, and edit-after-Apply.
- [ ] Run relevant material/editor tests and build the runnable editor using the
  [build workflow](../Agents/BuildAndRun.md) and
  [test workflow](../Agents/Testing.md).
- [ ] Update [material graph operations](../Editor/Architecture/MaterialGraphOperations.md)
  with the implemented document contract and report any omitted manual UI checks.
- [ ] Validate documentation and complete this plan with evidence.
