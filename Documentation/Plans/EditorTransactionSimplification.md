# Editor Transaction Simplification Plan

Summary: Align recording and scope semantics with UE 5.8, isolate fallible command callbacks and move mounted-content discovery out of the history service.

Last reviewed: 2026-11-21

Status: Completed
Completed: 2026-11-21

## Current Status

All four stages are complete. A subsequent simplification retired the editor-session journal introduced in Stage 4 and the history command flag for mounted-content changes. The Asset and File data sources publish item updates to the browser. AssetRegistry retains its catalog revision and history retains package checkpoints. The Stage 4 checklist and validation below record the original migration.

The command-failure follow-up keeps the UE ordering of object baselines and
custom undo records. ApplyEditorChange prepares command metadata before the
edit, then appends the command after any nested Modify captures. Durin keeps
checked publication and recovery for native commands that can fail. The
transaction and reinstancing cases cover package declarations and GC ownership.

On base `465fbcdd0`, the command follow-up built `all` on Win64-Debug-DurinEditor.
Across EditorOperationTests, LevelMutationTests, SkyBoxTests, AssetPackageReloadTests,
EditorPropertyTests, ViewportTests, EditorHostToolTests, PythonToolIntegrationTests,
RoadEditorIntegrationTests, MaterialGraphEditingTests and MaterialEditingPersistenceTests,
981 cases passed. Fault injection covered metadata rejection before Apply and failed
history publication after a successful edit, both standalone and inside an outer
scope. Additional cases covered nested commands, clean failure and recovery
with retained references. One intermediate map-key interaction test failed once.
The property suite passed on immediate retry and on the final build.

The interface follow-up removed the buffer detail lookup and checked public PIE
query. Checkpoint validity derives from the saved revision. Package transitions
are private to retained history and its buffer. On base `af49af358`, the
Win64-Debug-DurinEditor `all` build succeeded and 681 cases passed across
EditorOperationTests, AssetPackageReloadTests, ViewportTests, EditorPropertyTests,
RoadEditorIntegrationTests and MaterialEditingPersistenceTests.

The follow-up on base `982884ea6` built `all` on Win64-Debug-DurinEditor.
Across 15 relevant native targets, 1,350 cases passed and two browser
symlink cases skipped because the process lacked Windows symlink privileges.
SceneImportTests passed all 38 cases after the material fix. Browser coverage
includes immediate asset and directory Reveal before the next subsystem tick,
selection after delivery, and restoring a browser directory after a closed-panel
import. Transaction coverage includes command replay and saved checkpoints.

On 2026-11-21 the stage 4 working tree on base `bc7fbb455` built the incremental `all` target and passed 1,215 cases on Win64-Debug-DurinEditor across EditorOperationTests, ContentBrowserWorkflowTests, ContentBrowserDataTests, EditorPropertyTests, MaterialGraphEditingTests, MaterialEditingPersistenceTests, ViewportTests, SplineTests, RoadEditorIntegrationTests, EditorShellTests and EditorAssetWorkflowTests. Two existing browser symlink tests skipped because the process lacked Windows symlink privileges. Coverage includes external paths, history full-refresh batches, no-op, cancellation, failed commit and replay, nested publication and save/undo/redo checkpoints with a throwing subscriber.

Additional SceneImportTests coverage passed 15 of 38 cases. The same 23 cases failed at the same assertions on the unmodified stage 3 base, during import before mounted-content notification. The affected material imports report an invalid object in the replacement graph.

Stage 1 passed on 2026-11-20. EditorOperationTests passed all 399 tests in 15 suites on Win64-Debug-DurinEditor, using the stage 1 working tree on base `6bc1a7919`. This covers guarded admission, optional replay details, required callback failure and guarded Undo without Redo.

Stage 2 is complete. The stage 3 scope interface was brought forward at review and the mixed-field capture bypass was removed.

The stage 3 audit covered Engine, Sandbox and RoadWeaver source, tests, tools and Python. The Level Editor transform gizmo was the only production caller using implicit standalone submission. It now records the final preview in an explicit release scope. Opening a scope across input events would let unrelated editor edits join the drag. Interactive material parameter previews use the same commit boundary. The synchronous material parameter command now opens its scope before changing the value. Graph presentation, material configuration and ApplyEditorChange already had scopes. Tools and Python use domain commands or explicit scopes. The buffer now requires an active recording for StoreUndo.

Stage 3 passed on 2026-11-21 on the working tree based on `2934ba6e3`. On Win64-Debug-DurinEditor, 1,031 cases passed across EditorOperationTests, ViewportTests, SplineTests, MaterialGraphEditingTests, MaterialEditorInteractionTests, MaterialEditingPersistenceTests, EditorPropertyTests, RoadEditorIntegrationTests, PythonToolIntegrationTests and EditorHostToolTests. Coverage includes idle submission rejection, joined admission failure, foreign edits during drag, discard at release, cancellation during synchronous parameter publication and Python continuation after an injected finalization failure.

On 2026-11-20 the scope migration on base `97ac906b6` passed 836 cases on Win64-Debug-DurinEditor across EditorOperationTests, EditorPropertyTests, MaterialGraphEditingTests, MaterialEditingPersistenceTests, LevelMutationTests, SkyBoxTests, AssetPackageReloadTests, PythonToolIntegrationTests, RoadEditorIntegrationTests and EditorHostToolTests. The Python fault injection now verifies successful script continuation and an independently failed history entry. The property integration case verifies the applied value and the buffer's recovery state.

On 2026-11-20 the stage 2 working tree on base `bef2f1422` passed 581 tests on Win64-Debug-DurinEditor across EditorOperationTests, CoreTransactionContractTests, EditorPropertyTests, EditorShellTests, AssetPackageReloadTests and RoadEditorIntegrationTests. The DurinLauncher editor host target also built successfully. The batch cases verify one baseline serialization per object and bound the number of hash bucket entries seen by repeated probes at 64 and 256 objects.

On 2026-11-20 the stage 3 capture migration on base `d570aff80` passed 825 cases on Win64-Debug-DurinEditor across EditorOperationTests, EditorPropertyTests, LevelMutationTests, SkyBoxTests, MaterialGraphEditingTests, MaterialEditingPersistenceTests, EditorShellTests, EditorHostToolTests and RoadEditorIntegrationTests. The material tests cover both edit orders in a shared scope, sparse graph movement and grouped configuration with no whole-object record. Runtime whole-object replay requires a captured name. A configuration-only command also replays on an unnamed owner through its domain record.

## Goal

Keep ordinary edits expressible through a scope, Modify and ordered records. Use the local UE 5.8 UnrealEd sources as the reference, particularly `Classes/Editor/Transactor.h`, `Private/EditorTransaction.cpp` and `Public/ScopedTransaction.h`. Runtime object capture remains independent of DurinEd. The owning contracts are [transactors](../Editor/Architecture/Transactions/Transactors.md) and [records](../Editor/Architecture/Transactions/TransactionRecords.md).

| Surface | UE 5.8 baseline | Durin decision |
| --- | --- | --- |
| Scope and buffer | RAII, Begin/End return previous depth, outermost End finalizes, Cancel discards the entire recording and leaves applied edits in place. A disabled scope permits an existing outer recording. | Adopt the UE scope surface. Keep stale-scope identity checks private and expose history diagnostics on the buffer. |
| Object capture | SaveObject returns void. ContainsObject queries participation. SaveObject, StoreUndo, SaveArray and SnapshotObject have distinct roles. Ordered records and an object index coexist. | Preserve these roles. Keep reflected array paths for vector relocation. Ordinary callers use Modify. Explicitly bound changes own capture for their object. Durin commands may instead declare participants for field-specific replay. |
| Command execution | Object-bound StoreUndo records an existing change. Replay applies ordered changes and reconstructs objects with synchronous hooks and annotations. | StoreUndo requires an active recording. Callers own scope lifetime. Checked failure, compensation and explicit recovery serve fallible native commands. |
| History publication | Undo/Redo operate on a bounded history with barriers, expiry and object replacement. | Keep successful-commit trimming and package checkpoints. Move the mounted-content journal to its editor owner. |

## Implementation Stages

### Stage 1: Protect command admission and replay

Contract adjustment. Command admission metadata is read under a closed mutation boundary. A rejected joined submission preserves the outer recording. A standalone failure releases its own recording and preserves the existing history. Neither path can reverse an arbitrary write made before submission. Replay presentation details are optional. Essential replay failures with uncertain effects close the buffer for recovery.

- [x] Protect custom command metadata acquisition and insertion against exception and reentry. Keep existing public signatures.
- [x] Isolate Undo/Redo detail failures. Preserve existing record compensation and prevent unexpected required callback failures from stranding the buffer in a replay state.
- [x] Exercise standalone and joined failures, callback cancellation, and throwing Undo and Redo details. Verify model values, active recording, history heads and subsequent operations. Run the transaction history suite and update the owning contract.

### Stage 2: Unify active object capture

Depends on stage 1. Preserve the UE void SaveObject and participation query. Use an active object index keyed by persistent identity and preserve record order. Track primary custom ownership separately from additional participants and whole-object baselines. Stage 3 retires the temporary mixed-field admission API as callers move to Modify.

- [x] Route ordinary and mixed-field capture through one internal first-baseline operation. Remove redundant scans, insertion APIs and friendship once callers migrate.
- [x] Verify repeated Modify, custom-before-Modify, Modify-before-custom, additional participants, snapshots, both array/whole orderings and exact/logical identity. Verify one first serialization per object and linear lookup growth for a batch without wall-clock thresholds.
- [x] Release recording-only index data at scope completion. Verify retained history, references, replacement and undo/redo still work after release.

### Stage 3: Align scope lifetime with UE

Scope cleanup can proceed after stage 1 independently of the stage 2 object index. UE's ScopedTransaction.cpp ends through destruction. UTransBuffer::End returns the previous depth, finalizes the outer recording and broadcasts state changes. Adopt that public scope contract. A completed domain edit remains applied when history publication fails. Preserve checked admission, native mutation errors and buffer diagnostics. CallInEditor and explicit history-management commands expose history information in their own results.

- [x] Remove the public mixed-field capture bypass. Verify both edit orders with ordinary Modify and custom records, including one baseline per object.
- [x] Inventory native, tool and Python consumers including standalone StoreUndo. Migrate already-applied commands to an owning scope before mutation where feasible. Remove the standalone extension when no supported caller needs it; document any remaining caller and reason.
- [x] Remove public scoped End and use destruction for ordinary edits and Python scope exits. Keep native transactor End for depth-based management. Keep cancellation, stale scope protection and diagnostic history results.
- [x] Verify outer and nested completion, disabled scope, inner cancel, no-op and recovery. Exercise property and Python finalization failures with independent assertions for the model or script outcome and buffer diagnostics. Validate migrated consumers across the workspace.

### Stage 4: Move mounted-content discovery to its owner

Depends on stage 3. Keep package checkpoints synchronized with successful history transitions. Content discovery accepts the existing content change batch from both history and external editors. Preserve synchronous object notifications and reconstruction order.

- [x] Move journal ownership and revision queries to the editor content service. Migrate MainFrame, LevelEditor, history and test consumers and remove their transactor forwarding APIs.
- [x] Verify revision and batch contents for execute, undo, redo, external change, no-op, cancel and failure. Verify dirty checkpoints across save, undo and redo, including notification failure behavior.
- [x] Audit Engine, Sandbox and RoadWeaver source and tests for retired APIs. Build affected owners and consumers, run relevant history, property, material and content tests, and update implemented contracts.

Use the [build](../Agents/BuildAndRun.md) and [test](../Agents/Testing.md) workflows for bounded selections and evidence reuse.
