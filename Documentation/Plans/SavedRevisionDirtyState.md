# Saved-Revision Dirty State Plan

Summary: Make package dirty state reflect divergence from the last successful save across edits, Undo, Redo, and branched transaction history.

Last reviewed: 2026-07-28

Status: Active
Completed:

## Current Status

The editor currently stores dirty state as a boolean on `DPackage`. Successful
asset saves clear that boolean, while reflected-property edits, transform
transactions, Undo, and Redo mark the affected package dirty. The transaction
manager owns bounded Undo/Redo stacks and monotonic transaction IDs, but it does
not identify the revision represented by the current history head or remember
which revision was last saved.

Consequently, Undo can restore all scene values to their last-saved state while
the Level Editor continues to show the document as dirty. The implemented
architecture documents this as a conservative temporary policy pending a
saved-revision model.

No implementation stages in this plan have started.

## Goal

Define dirty as an exact document-state relationship for transaction-managed
scene changes:

```text
dirty = saved revision is invalid
     or current revision != saved revision
```

After a successful save, Undo and Redo must clear dirty exactly when they return
the package to that saved revision. A new edit after Undo must create a new
revision identity, so discarding a Redo branch can never make a different state
appear saved.

## Scope

- Add package-scoped current and saved revision tracking to
  `FEditorTransactionManager`.
- Record the before/after package revisions represented by every committed
  transaction.
- Associate reflected-property and Level Editor transform transactions with
  every package they modify.
- Establish or advance the active Level package's save checkpoint only after a
  load/activation or successful save with known clean state.
- Keep `DPackage::IsDirty()` as the canonical query used by editor chrome,
  document-close confirmation, and existing asset workflows.
- Invalidate the saved checkpoint for Level Editor modifications that cannot be
  represented by the transaction manager.
- Cover save-in-the-middle, Undo/Redo, history branching, history eviction,
  failed operations, document switching, and untracked edits with automated and
  interactive validation.
- Replace the temporary conservative dirty-state statement in the reflected
  property editing architecture after the behavior is implemented.

## Non-Goals

- Comparing serialized scene bytes or serializing the full scene after each
  edit.
- Defining dirty from whether the Undo stack is empty.
- Persisting revision IDs across editor processes or package reloads.
- Making editor session settings, viewport navigation, selection, or other
  non-package state participate in level dirty state.
- Retrofitting independent Material Editor and Texture Editor document
  lifecycles in this plan. The shared transaction API must support them, but
  Level Editor is the first required integration.
- Turning currently non-undoable scene operations into undoable transactions
  unless that conversion is required to avoid an incorrect clean result.
- Changing save prompts, document tab visuals, or asset serialization formats.

## Design Decisions and Invariants

### Package-scoped revision identity

- Introduce a revision ID type distinct from `FEditorTransactionId`.
- Allocate revision IDs monotonically for the editor session. `0` is invalid
  and an allocated ID is never reused, including after Undo, Redo-stack
  truncation, history eviction, document switching, or transaction-manager
  reset.
- Track revision state per `DPackage`, not as one global history cursor. The
  editor has a global transaction manager, and a transaction may eventually
  affect more than one package.
- A tracked package owns:
  - its current revision;
  - an optional saved revision; and
  - whether the saved checkpoint is still trustworthy.
- Package references retained by revision metadata must follow the existing
  editor object-lifetime rules. Tracking must not leave a dangling raw package
  pointer after history clear, document replacement, package unload, or garbage
  collection.

### Transaction transitions

- Each history entry records a deduplicated set of affected packages and one
  `{BeforeRevision, AfterRevision}` transition per package.
- A newly committed transaction captures each package's current revision as
  `BeforeRevision` and allocates a fresh `AfterRevision`.
- Undo changes each affected package to `BeforeRevision` only after the
  transaction's value restoration succeeds. Redo changes it to
  `AfterRevision` only after reapplication succeeds.
- A failed Execute, Commit, Undo, or Redo does not move revisions, change stack
  topology, or change dirty state.
- A no-op or cancelled interaction creates neither a transaction nor a
  revision.
- Multi-package transitions update revision metadata and dirty booleans as one
  manager operation after the underlying transaction succeeds.

### Saved checkpoints and dirty synchronization

- A successful package load/activation establishes a clean initial revision
  when the package is known to match persistent storage.
- A save records `SavedRevision = CurrentRevision` only after
  `Asset::SavePackage()` succeeds. Save failure preserves the previous
  checkpoint and dirty state.
- For a valid checkpoint, the transaction manager synchronizes the existing
  package boolean to `CurrentRevision != SavedRevision`.
- Undoing to the saved revision clears package dirty. Redoing away from it sets
  dirty. If the saved point lies in the middle of history, movement on either
  side follows revision equality rather than stack depth.
- Saving at an undone history position does not clear the Redo stack. Redo
  leaves the new save point and therefore marks the package dirty.
- A new edit after Undo clears Redo as today and receives a fresh revision. If
  the saved revision existed only on the discarded branch, the package remains
  dirty until another successful save or until history reaches the same
  still-reachable revision through a valid transition.
- History-size eviction removes undoability, not revision identity. Evicting an
  entry must not silently move or invalidate the current or saved revision.

### Conservative untracked-edit fallback

- Any persistent mutation that bypasses `FEditorTransactionManager` must both
  mark the package dirty and invalidate its saved checkpoint through one
  explicit editor API.
- Once invalidated, Undo or Redo must not clear dirty merely because two stored
  revision IDs match. Only a successful save or a fresh known-clean package
  activation re-establishes a valid checkpoint.
- Transaction-backed mutation paths declare their affected packages explicitly;
  the manager is the only component that may clear dirty as a consequence of
  revision equality.
- Existing cancel/no-change behavior remains intact. In particular, Transform
  Gizmo cancellation may restore the pre-drag boolean state because no
  transaction or persistent state change survives the interaction.
- Stage 2 must audit all Level Editor calls that mutate persistent level data.
  Any path that cannot provide trustworthy affected-package metadata uses the
  invalidation fallback; it must not be silently treated as tracked.

### Document and history lifecycle

- Activating a different level first terminates any interactive edit, then
  clears transaction history and package revision metadata belonging to the old
  document, and finally establishes the new level's initial checkpoint.
- Discard/close and package unload clear their revision metadata; they do not
  manufacture a saved checkpoint for unsaved in-memory values.
- Revision state is editor-session metadata and is neither reflected nor
  serialized into `DPackage`.
- `DPackage::IsDirty()` remains the compatibility boundary. Callers do not need
  to know whether the boolean was set directly or synchronized from revision
  state.

## Current Foundations and Gaps

- `FEditorTransactionManager` already serializes Execute, CommitApplied, Undo,
  and Redo and assigns monotonic transaction IDs. It has no package ownership or
  state-revision metadata.
- History entries already move atomically between Undo and Redo stacks only
  after an operation succeeds, which provides the correct point for moving
  revisions.
- `FReflectedPropertyTransaction` retains a stable object target and marks its
  package dirty during Undo/Redo, but it does not expose the package to the
  manager.
- `FTransformGizmo` commits one applied transaction for a completed drag and
  restores the original dirty boolean for a cancelled or net-zero drag.
- `FLevelDocumentController::SaveCurrentLevel()` has an explicit successful-save
  boundary suitable for advancing the checkpoint. `ActivateLevel()` already
  clears global transaction history during level replacement.
- `MLevelEditor` reads `DPackage::IsDirty()` for root-window state and close
  confirmation, so synchronizing that boolean avoids a parallel dirty query
  path.
- The current reflected-property architecture explicitly states that Undo
  conservatively leaves packages dirty until a saved-revision model exists.

## Implementation Stages

### Stage 1: Package revision model in the transaction manager

Outcome: the shared transaction manager can represent package-scoped history
states and saved checkpoints without changing editor behavior yet.

Dependencies: none.

- [ ] Add a nonzero, session-monotonic editor revision ID and package revision
      state owned by `FEditorTransactionManager`.
- [ ] Extend transaction metadata so every transaction can report a stable,
      deduplicated set of affected packages.
- [ ] Store before/after revision transitions on each history entry while
      retaining the transaction ID used by notifications and expected-head
      checks.
- [ ] Add explicit APIs to establish a known-clean package, record a successful
      save, invalidate a package checkpoint, query tracked state for tests, and
      forget package/document state.
- [ ] Advance revisions only after successful Execute/CommitApplied/Undo/Redo
      operations, preserving existing failure events and stack behavior.
- [ ] Synchronize `DPackage` dirty state from revision equality or checkpoint
      invalidation without exposing revision metadata through Runtime
      serialization.
- [ ] Define safe package lifetime handling and release all retained metadata on
      history/document reset.
- [ ] Preserve monotonic allocation across `Clear()`; clearing history must not
      make a stale revision ID reusable.

#### Acceptance Gate

- The transaction manager can model an initially saved package, multiple
  commits, a saved point in the middle of history, Undo/Redo on both sides, and
  a new branch with unambiguous revision identities.
- Failure paths leave both the history head and all package revision/dirty state
  unchanged.
- Clearing history or forgetting a package releases package ownership without
  resetting the session revision allocator.

### Stage 2: Level Editor transaction and save integration

Outcome: every supported transactional scene edit participates in saved-revision
dirty tracking, and every untracked edit is conservatively dirty.

Dependencies: Stage 1.

- [ ] Make reflected-property transactions report the target object's package,
      including Undo and Redo restoration.
- [ ] Make actor transform transactions report all deduplicated packages touched
      by a gizmo operation.
- [ ] Audit other Level Editor transaction implementations and add affected
      packages to each one.
- [ ] Audit persistent Level Editor mutations that bypass transaction history
      and route them through the explicit checkpoint-invalidation fallback.
- [ ] Establish the active level's initial known-clean revision after successful
      load/activation, while preserving an already-dirty package as invalidated.
- [ ] Advance the saved checkpoint only after `SaveCurrentLevel()` receives a
      successful `Asset::SavePackage()` result.
- [ ] Forget old package revision state during document discard, replacement,
      close, and unload in the same lifecycle that clears transaction history.
- [ ] Keep Level Editor chrome and close confirmation reading
      `DPackage::IsDirty()`; do not add a second UI-only interpretation.
- [ ] Preserve Transform Gizmo cancel and net-zero-drag restoration semantics.

#### Acceptance Gate

- Editing a saved level makes it dirty; Undoing exactly to the save point makes
  it clean; Redoing away makes it dirty again.
- Saving after several edits makes that middle history position clean without
  destroying valid Undo or Redo history.
- A direct untracked mutation cannot become clean through Undo/Redo.
- Switching or discarding a level cannot carry saved-revision metadata into a
  different package.

### Stage 3: Automated revision and integration coverage

Outcome: deterministic tests protect revision topology, failure behavior, and
Level package dirty synchronization.

Dependencies: Stages 1-2.

- [ ] Add focused transaction-manager unit tests for initial checkpoint,
      commit, save, Undo, Redo, save-in-the-middle, and multi-package
      transitions.
- [ ] Add branch tests proving a post-Undo commit uses a new revision and cannot
      collide with a saved revision on the discarded Redo branch.
- [ ] Add tests for invalid checkpoints, successful re-save, failed save
      handoff, failed Undo/Redo, no-op/cancelled edits, and `Clear()`.
- [ ] Add a bounded-history test proving eviction does not change current/saved
      revision comparison.
- [ ] Extend reflected-property transaction tests to assert package dirty state
      at the save point across Undo and Redo.
- [ ] Extend transform transaction coverage for completed, cancelled, and
      net-zero drags.
- [ ] Add Level document lifecycle coverage for activation, save success,
      simulated save failure, replacement, and discard.

#### Acceptance Gate

- Native tests deterministically cover every row in the Validation Matrix that
  does not require interactive UI.
- Existing transaction event, target-lifetime, no-op, cancellation, and
  Undo/Redo tests remain green.

### Stage 4: Editor validation and contract update

Outcome: the user-visible Level Editor behavior is verified and the temporary
architecture limitation is replaced by the implemented contract.

Dependencies: Stage 3.

- [ ] Follow the repository build and test workflow in
      `Documentation/Development/Build/BuildAndRun.md`.
- [ ] Run the relevant native test target, then complete the required full
      `all` build for the user-visible editor change.
- [ ] Interactively verify the clean/dirty indicator and close prompt for edit,
      save, Undo, Redo, branch, cancel, and failed-save scenarios.
- [ ] Update `ReflectedPropertyEditing.md` to describe saved revisions,
      affected-package metadata, and the conservative invalidation fallback.
- [ ] Record final evidence in this plan and move any broader lasting
      transaction/package contract to the appropriate editor architecture
      document.
- [ ] Run `.\DocTool.bat validate --scope all`.

#### Acceptance Gate

- The full editor build succeeds and the verified editor executable demonstrates
  that dirty clears only at a trustworthy saved revision.
- Architecture documentation no longer describes conservative always-dirty
  Undo behavior as current.
- Plan and documentation validation succeeds.

## Validation Matrix

| Scenario | Expected current/saved relationship | Expected dirty |
| --- | --- | --- |
| Freshly loaded saved level | Current equals valid Saved | Clean |
| First committed edit | Current differs from Saved | Dirty |
| Undo first edit | Current equals Saved | Clean |
| Redo first edit | Current differs from Saved | Dirty |
| Save after multiple edits | Saved advances to Current | Clean |
| Undo from a middle save point | Current differs from Saved | Dirty |
| Redo back to the middle save point | Current equals Saved | Clean |
| Redo past the middle save point | Current differs from Saved | Dirty |
| New edit after Undo | Fresh After revision; Redo discarded | Dirty unless explicitly saved |
| Saved revision was on discarded Redo branch | Saved remains unequal/unreachable | Dirty |
| Transaction Undo or Redo fails | Revisions and stacks do not move | Unchanged |
| Package save fails | Saved does not advance | Unchanged, normally dirty |
| No-op or cancelled interaction | No new revision | Restored to pre-interaction state |
| Untracked persistent mutation | Saved checkpoint invalid | Dirty |
| Undo/Redo after untracked mutation | Checkpoint remains invalid | Dirty |
| Successful save after invalidation | Saved becomes valid at Current | Clean |
| Old history entry is evicted | Current/Saved identities remain | Based on equality |
| Switch to another level | Old metadata forgotten; new clean state established | Matches new package |
| Transaction affects two packages | Both transition after one successful operation | Per-package equality |

## Definition of Done

- Dirty state returns to clean when and only when the active package reaches a
  valid last-saved revision.
- Transaction entries carry stable per-package before/after revision
  transitions, including multi-package transactions.
- Redo truncation and history eviction cannot create revision aliasing.
- Save, Execute, Undo, and Redo failures do not advance checkpoints or revision
  heads.
- Untracked Level Editor mutations invalidate the checkpoint and remain
  conservatively dirty until a successful save.
- Level activation, replacement, discard, and unload do not leak revision
  metadata or package lifetime.
- Automated tests, the full `all` build, interactive Level Editor checks, and
  documentation validation pass.
- The lasting architecture contract is documented outside the active plan.

## Deferred Follow-ups

- Adopt the shared saved-revision APIs in Material Editor, Texture Editor, and
  other package-editing workspaces.
- Add a reusable editor document/history abstraction if multiple simultaneously
  open editable documents require independent transaction stacks.
- Convert remaining invalidation-only Level Editor operations into full
  transactions where user-facing Undo support is independently justified.

## Related Documentation

- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Editor/EditorTransaction.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorTransaction.cpp`
- `Engine/Source/Editor/DurinEd/Private/Editor/ReflectedPropertyEditing.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Documents/LevelDocumentController.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/TransformGizmo.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Package.h`
- `Engine/Tests/Native/EngineTests/Private/Editor/ReflectedPropertyTransactionTests.cpp`
