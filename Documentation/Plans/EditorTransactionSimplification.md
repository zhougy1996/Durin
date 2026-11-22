# Editor Transaction Simplification Plan

Summary: Align recording and scope semantics with UE 5.8, isolate fallible command callbacks and move mounted-content discovery out of the history service.

Last reviewed: 2026-11-22

Status: Completed
Completed: 2026-11-21

## Current Status

On base aea9b9f54, Actor batches resolve editable references before dispatching PostLoad once per created object. Only Actor roots restore cross-Actor links. Regression coverage includes a forward mesh reference and shared material visible during PostLoad, candidate retirement, internal and external component attachments, world transforms and single-step Undo/Redo. Win64-Debug-DurinEditor passed 1,152 cases across LevelMutationTests, CoreObjectTests, WorldTests, SplineTests, ViewportTests, EditorOperationTests, PythonReflectionIntegrationTests and RoadEditorIntegrationTests. The incremental `all` build passed.

On base 96293fd47, cross-Actor duplication resolves the parent Actor by source name and attaches to its root, preserving world space. Retirement during either PostLoad pass skips the affected candidate and leaves one Undo entry for the surviving copy. Win64-Debug-DurinEditor passed 661 cases across LevelMutationTests, ViewportTests, EditorOperationTests, PythonReflectionIntegrationTests and RoadEditorIntegrationTests. The incremental `all` build passed.

On base 1504c8f18, duplication publishes each admitted Actor, then restores cross-Actor links by source parent name with world space preserved. Tests cover child-first selection, missing components, rejected or renamed parents, root retirement during PostLoad and single-step history. Win64-Debug-DurinEditor passed 1,205 cases across LevelMutationTests, CoreObjectTests, WorldTests, SkyBoxTests, ViewportTests, EditorOperationTests, PythonReflectionIntegrationTests, PythonToolIntegrationTests, EditorHostToolTests and RoadEditorIntegrationTests. The incremental `all` build passed.

On base 41c034e5c, all 1,026 tests passed across LevelMutationTests, WorldTests, SkyBoxTests, ViewportTests, EditorOperationTests, PythonReflectionIntegrationTests, PythonToolIntegrationTests, EditorHostToolTests and RoadEditorIntegrationTests. The final `all` build passed.

On base e4660b191, editor-owned buffers consult their host before replay. Object identity mutations enforce sibling names and absent Actors restore component ownership without activation. Combined attachment validation remains. Win64-Debug-DurinEditor passed 1,366 cases across CoreObjectTests, CoreTransactionContractTests, WorldTests, LevelMutationTests, SkyBoxTests, EditorOperationTests, AssetPackageReloadTests, PythonReflectionIntegrationTests, MaterialGraphEditingTests and AssetPackageTests.

On base 5eaa1c645, Actor publication and removal use separate Level APIs. The Level snapshot restores order. Regression coverage observes the external parent at component registration, cancellation after the first boundary detach, and retirement of an unpublished duplicate in PostLoad. Win64-Debug-DurinEditor passed all 1,024 cases across LevelMutationTests, WorldTests, SkyBoxTests, ViewportTests, EditorOperationTests, PythonReflectionIntegrationTests, PythonToolIntegrationTests, EditorHostToolTests and RoadEditorIntegrationTests. The final all build passed.

On base c1ff3b11a, membership history follows serialized references and annotations. The destination Level array identifies actual publications, and instance component replay restores the recorded list. LevelMutationTests, WorldTests, SkyBoxTests, EditorOperationTests and AssetPackageReloadTests passed all 754 cases on Win64-Debug-DurinEditor, including GC of Outer-only children, reference restoration, detached-owner admission, occupied names and attachment cycles.

Actor and instance-component membership now use scoped object snapshots for creation, deletion and duplication. Level and Actor state, component creation method, lifecycle and attachment edges replay through existing transaction records. Replay retains unpublished identities across GC, validates world availability before structural writes, and leaves SkyBox texture retirement independent of Actor publication. On base 04f99be03, Win64-Debug-DurinEditor passed all 1,239 tests across LevelMutationTests, WorldTests, SkyBoxTests, ViewportTests, EditorOperationTests, PythonReflectionIntegrationTests, PythonToolIntegrationTests, AssetPackageReloadTests, EditorHostToolTests, RoadEditorIntegrationTests, EditorPropertyTests and MaterialGraphEditingTests. The final all build passed.

The previous membership command path shared one DurinEd-private function and retained the recording ID locally across callbacks. Known post-write recording refusals, retained-size overflow and confirmed PIE contamination retire history while preserving the model. Unknown native effects or incomplete compensation keep recovery locked. On base `da7427f51`, Win64-Debug-DurinEditor passed 1,192 cases across EditorOperationTests, LevelMutationTests, EditorPropertyTests, AssetPackageReloadTests, WorldTests, SkyBoxTests, PythonToolIntegrationTests, ViewportTests, EditorHostToolTests, MaterialGraphEditingTests and RoadEditorIntegrationTests. The final all build passed. One map-key interaction case failed once and then passed individually and in the complete property suite with the same binaries.

SaveObject follows the UE void interface. A failed snapshot invalidates the outer recording, preceding Undo and Redo branches and affected saved checkpoints. Modify reports admission and editors still honor explicit cancellation from dirty callbacks. On base `82c0afa74`, Win64-Debug-DurinEditor passed 1,248 cases across EditorOperationTests, LevelMutationTests, EditorPropertyTests, CoreObjectTests, CoreTransactionContractTests, MaterialGraphEditingTests, WorldTests, SkyBoxTests and ViewportTests.

PIE copy-back owns its session through package notifications. Nested copy-back is rejected and a requested stop runs when the outer operation exits. On base `b965c043d`, Win64-Debug-DurinEditor passed 241 WorldTests including stop from a dirty callback.

On base `99baa4ab5`, SaveObject reports capture success and logs its own failure. Reset follows the UE void contract. Lifecycle callers own admission and PIE clears history before copying editor values. A completed replay with overflowing history size preserves the applied edit and permits fresh history. Win64-Debug-DurinEditor passed 1,927 cases across the 18 changed test targets plus CoreTransactionContractTests and EditorAssetWorkflowTests, and the all build. Regression coverage verifies overflow event delivery, a new undoable edit after overflow, and selected Actor transform copy-back through the PIE editor entry point.

On base `3f21c5f8e`, editor callers check admission and capture directly. End takes no arguments and returns only the previous depth. Finalization queues history events. The public transactor has no shared result type or last-operation state. Whole-object capture reports its own outcome even for existing participants. Failure to capture an eligible native Actor root rejects the edit and preserves earlier work in a joined scope. Win64-Debug-DurinEditor passed 2,036 cases across EditorOperationTests, EditorPropertyTests, CoreObjectTests, CoreTransactionContractTests, LevelMutationTests, ViewportTests, SkyBoxTests, PythonToolIntegrationTests, MaterialGraphEditingTests, EditorAssetWorkflowTests, AssetDetailCustomizationTests, MaterialEditingPersistenceTests, MaterialEditorInteractionTests, AssetPackageReloadTests, WorldTests, SplineTests, StaticMeshMaterialTests, EditorShellTests, RoadEditorIntegrationTests, EditorHostToolTests, ContentBrowserWorkflowTests, TextureTests and the final all build.

On base `23414b4c0`, reflected actions check recording after dirty callbacks and history availability queries explain recovery lockout. Win64-Debug-DurinEditor passed all 78 EditorPropertyTests and 408 EditorOperationTests.

On base `3719a9a51`, the index-only editor scope passed the Win64-Debug-DurinEditor all build and 1,472 cases across EditorOperationTests, EditorPropertyTests, WorldTests, AssetPackageReloadTests, EditorShellTests, RoadEditorIntegrationTests, PythonToolIntegrationTests, ViewportTests, LevelMutationTests, SkyBoxTests, SplineTests, EditorHostToolTests, MaterialGraphEditingTests, MaterialEditingPersistenceTests, PythonReflectionIntegrationTests and MaterialEditorInteractionTests. Regression coverage includes editor replacement with a property preview or suspended Python job, same-host yield, disabled nested recording and native buffer management without an editor.

On base `551e0ed0d`, LevelMutationTests, EditorPropertyTests and EditorOperationTests passed 545 cases, including package dirty listeners canceling actor and component edits before their writes.

On base `2d8420ddb`, the UE index-based scope contract passed the Win64-Debug-DurinEditor all build and 1,256 cases across EditorOperationTests, LevelMutationTests, PythonToolIntegrationTests, EditorPropertyTests, MaterialGraphEditingTests, SkyBoxTests, ViewportTests, MaterialEditingPersistenceTests, EditorHostToolTests, RoadEditorIntegrationTests, EditorShellTests and WorldTests. Coverage includes immediate replacement after a sole scope cancels itself, component callbacks and suspended Python scopes unwinding before replacement, and post-mutation recovery retaining the original transaction ID.

On base `6cc0b94d1`, buffer-owned membership recovery passed all 797 cases across LevelMutationTests, EditorOperationTests, SkyBoxTests, ViewportTests, PythonReflectionIntegrationTests, PythonToolIntegrationTests, AssetPackageReloadTests, EditorHostToolTests and RoadEditorIntegrationTests on Win64-Debug-DurinEditor. The mutation tests cover capture order, rejection before mutation, failure after mutation and replacement of a canceled scope.

On base `c4a3179f8`, LevelEditor built and all 249 cases passed across SkyBoxTests, LevelMutationTests, ViewportTests and PythonReflectionIntegrationTests on Win64-Debug-DurinEditor. Coverage includes busy-host rejection on a same-texture request and host rejection after spawn configuration.

On base `09915f87f`, required recorded spawning and membership-owned recovery passed 797 cases across LevelMutationTests, EditorOperationTests, SkyBoxTests, ViewportTests, PythonReflectionIntegrationTests, PythonToolIntegrationTests, AssetPackageReloadTests, EditorHostToolTests and RoadEditorIntegrationTests on Win64-Debug-DurinEditor. Fault injection verified the original recording ID on a failed append after mutation. Cancellation followed by replacement recording and Undo also passed.

On base `75719378c`, the private viewport placement migration built LevelEditor and passed 249 cases across SkyBoxTests, ViewportTests, LevelMutationTests and PythonReflectionIntegrationTests on Win64-Debug-DurinEditor. Coverage includes host rejection, texture retirement on Redo and same-texture placement after reload without a new history entry.

On base `dc7b1d81d`, viewport SkyBox placement moved to the Actor subsystem and configured spawning became private to DurinEd. The Win64-Debug-DurinEditor all build passed. SkyBoxTests, LevelMutationTests, ViewportTests, EditorOperationTests, PythonReflectionIntegrationTests, PythonToolIntegrationTests, AssetPackageReloadTests, EditorHostToolTests and RoadEditorIntegrationTests passed 798 cases. Coverage includes read-only and foreign Levels, creation and replacement during a history transition, explicit standalone edits, and live-scene Redo with a retired texture.

On base `8f27ddb3b`, attachment and detachment adopted UE-style per-Actor execution. A rejected later Actor leaves earlier changes grouped for one Undo. On Win64-Debug-DurinEditor, LevelMutationTests, PythonReflectionIntegrationTests and ViewportTests passed 233 cases, including a callback introducing a cycle before the second Actor, no-op return values and membership recording failure recovery.

On base `aa651ac82`, configured spawning replaced prepared Actor publication. The Win64-Debug-DurinEditor all build passed. The nine targets passed 798 cases across SkyBoxTests, LevelMutationTests, EditorOperationTests, ViewportTests, PythonReflectionIntegrationTests, PythonToolIntegrationTests, RoadEditorIntegrationTests, AssetPackageReloadTests and EditorHostToolTests. Live-scene replay restored the original Actor and published an empty texture reference after retirement of the cube. Standalone placement and transaction rejection diagnostics also passed.

On base `10c6100cb`, all 60 LevelMutationTests passed on Win64-Debug-DurinEditor, including a synchronous observer redirecting an Actor after its inverse attachment succeeded.

An earlier Stage 3 follow-up moved SkyBox creation onto the prepared Actor entry point. Membership recording and incomplete-edit failure handling are internal to DurinEd. Failure-only dependencies become collectible, and incomplete attachment compensation disables history.

On base `bb8b94057`, the Win64-Debug-DurinEditor all build and 796 tests passed across SkyBoxTests, LevelMutationTests, EditorOperationTests, AssetPackageReloadTests, EditorHostToolTests, ViewportTests, PythonToolIntegrationTests, PythonReflectionIntegrationTests and RoadEditorIntegrationTests. At that revision the SkyBox tests covered direct placement and rejected Redo after texture retirement.

Actor attachment, detachment and component reparenting now use scoped component snapshots. Replay checks the destination graph for cycles before writes. On base `47f130c27`, LevelMutationTests (55) and EditorOperationTests (413) passed on Win64-Debug-DurinEditor, including mixed membership and snapshot replay.

All four stages are complete. A subsequent simplification retired the editor-session journal introduced in Stage 4 and the history command flag for mounted-content changes. The Asset and File data sources publish item updates to the browser. AssetRegistry retains its catalog revision and history retains package checkpoints. The Stage 4 checklist and validation below record the original migration.

Membership edits now own their first mutation and append custom undo data after nested object capture. History no longer executes first edits or tracks executing commands. Admission failures precede writes. An incomplete mutation with unknown effects locks history. A known refusal to append an applied change invalidates its recording.

On base `b8c81b2f8`, the membership follow-up passed the all build on Win64-Debug-DurinEditor.
The 979 passing cases spanned LevelMutationTests, EditorOperationTests, SkyBoxTests,
AssetPackageReloadTests, ViewportTests, EditorHostToolTests, EditorPropertyTests,
PythonToolIntegrationTests, RoadEditorIntegrationTests, MaterialGraphEditingTests and
MaterialEditingPersistenceTests. Fault injection checked admission before writes,
failed StoreUndo after native publication, GC retention during recovery and capture
order during component creation, Undo and Redo. The previously observed map-key
interaction case failed on one run and the full property suite passed on retry.
Engine, Sandbox and RoadWeaver contain no callers of the retired execution API.

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
| Scope and buffer | RAII, Begin/End return previous depth, outermost End finalizes, Cancel discards the entire recording and leaves applied edits in place. A disabled scope permits an existing outer recording. | Adopt the UE scope surface. Use the Begin index for scope lifetime. Unwind remaining scopes after cancellation before starting the next recording on the host. Publish finalization diagnostics through history events. |
| Object capture | SaveObject returns void. ContainsObject queries participation. SaveObject, StoreUndo, SaveArray and SnapshotObject have distinct roles. Ordered records and an object index coexist. | Keep serialization failure inside the recording. Preserve ContainsObject as the participation query. Keep reflected array paths for vector relocation. Ordinary callers use Modify. Explicitly bound changes own capture for their object. Durin commands may instead declare participants for field-specific replay. |
| Command execution | Object-bound StoreUndo records an existing change. Replay applies ordered changes and reconstructs objects with synchronous hooks and annotations. | StoreUndo requires an active recording. Callers own scope lifetime. Checked failure, compensation and explicit recovery serve fallible native commands. |
| History publication | Undo/Redo operate on a bounded history with barriers, expiry and object replacement. | Keep successful-commit trimming and package checkpoints. Move the mounted-content journal to its editor owner. |

## Implementation Stages

### Stage 1: Protect command admission and replay

Contract adjustment. Command admission metadata is read under a closed mutation boundary. A rejected joined submission preserves the outer recording. A standalone failure releases its own recording and preserves the existing history. Neither path can reverse an arbitrary write made before submission. Replay presentation details are optional. Essential replay failures with uncertain effects close the buffer for recovery.

- [x] Protect custom command metadata acquisition and insertion against exception and reentry. Keep existing public signatures.
- [x] Isolate Undo/Redo detail failures. Preserve existing record compensation and prevent unexpected required callback failures from stranding the buffer in a replay state.
- [x] Exercise standalone and joined failures, callback cancellation, and throwing Undo and Redo details. Verify model values, active recording, history heads and subsequent operations. Run the transaction history suite and update the owning contract.

### Stage 2: Unify active object capture

Depends on stage 1. Preserve UE capture and participation roles. The Stage 3 follow-up invalidates the complete recording on failed whole-object capture regardless of prior participation. Use an active object index keyed by persistent identity and preserve record order. Track primary custom ownership separately from additional participants and whole-object baselines. Stage 3 retires the temporary mixed-field admission API as callers move to Modify.

- [x] Route ordinary and mixed-field capture through one internal first-baseline operation. Remove redundant scans, insertion APIs and friendship once callers migrate.
- [x] Verify repeated Modify, custom-before-Modify, Modify-before-custom, additional participants, snapshots, both array/whole orderings and exact/logical identity. Verify one first serialization per object and linear lookup growth for a batch without wall-clock thresholds.
- [x] Release recording-only index data at scope completion. Verify retained history, references, replacement and undo/redo still work after release.

### Stage 3: Align scope lifetime with UE

Scope cleanup can proceed after stage 1 independently of the stage 2 object index. UE's ScopedTransaction.cpp ends through destruction. UTransBuffer::End returns the previous depth, finalizes the outer recording and broadcasts state changes. Adopt that public scope contract. A completed domain edit remains applied when history publication fails. Preserve checked admission, native mutation errors and buffer diagnostics. CallInEditor reports invocation. Explicit history-management commands report success and can provide failure text.

- [x] Remove the public mixed-field capture bypass. Verify both edit orders with ordinary Modify and custom records, including one baseline per object.
- [x] Inventory native, tool and Python consumers including standalone StoreUndo. Migrate already-applied commands to an owning scope before mutation where feasible. Remove the standalone extension when no supported caller needs it; document any remaining caller and reason.
- [x] Remove public scoped End and use destruction for ordinary edits and Python scope exits. Keep native transactor End for depth-based management. Keep cancellation and diagnostic history events.
- [x] Verify outer and nested completion, disabled scope, inner cancel, no-op and recovery. Exercise property and Python finalization failures with independent assertions for the model or script outcome and buffer diagnostics. Validate migrated consumers across the workspace.

- [x] Move first execution to Actor and component membership editing, bind StoreUndo to the aggregate owner, and remove the history-side execution API and in-flight command stack.

- [x] Route SkyBox placement through Actor membership editing. Keep the recording helper and incomplete-edit reporting private to DurinEd. Verify failed-command collection, replacement transactions after cancellation and recovery after incomplete batch attachment rollback.

- [x] Verify compensated batch attachment state after inverse notifications finish.

- [x] Share configured Actor spawning with SkyBox, keep placement resource checks in the adapter, and restore Actor identity with an empty texture after cube retirement. Consolidate incomplete-edit reporting and test cancellation.

- [x] Supersede batch inverse compensation with preflight and per-Actor application in a shared scope. Verify partial success, one Undo and no-op results.

- [x] Route viewport SkyBox creation and assignment through the Actor subsystem. Verify host rejection, standalone placement, owned error messages and texture retirement on Redo.

- [x] Check recording after dirty publication before Actor and component writes. Complete property cancellation after restoring the preview.

- [x] Remove shared last-operation diagnostics, merge reset entry points and report history publication through events.
- [x] Stop reflected function dispatch after synchronous recording cancellation and explain recovery lockout in history availability queries.
- [x] Route the nonmovable, index-only scope through the active editor. Retire property previews and Python scopes before host replacement and keep explicitly disabled scopes inert.

- [x] Align IsOutstanding with the UE scope index. Unwind canceled scopes before beginning replacement recording, including across Python yields. Check host activity after synchronous callbacks.

- [x] Move membership recovery into the buffer and remove recorder friendship. Preserve shared Actor/component recording order and original transaction IDs in recovery diagnostics.

- [x] Return concrete Actor admission errors and open the SkyBox replacement scope on the admitting editor. Recheck admission after spawn configuration.

- [x] Retire standalone Level placement. Move the SkyBox policy into LevelEditor and verify that re-dropping the same texture on the reloaded current Level preserves the Actor and history length.

- [x] Require recorded publication for configured editor spawning and handle incomplete writes inside membership recording, with the original recording ID in recovery diagnostics.

- [x] Replace Actor and instance-component membership commands with owner and participant snapshots. Set instance creation method before capture and restore native ownership and lifecycle on replay. Retire the membership recorder and its buffer-only failure paths.

- [x] Follow recorded GC references, restore instance membership from snapshots and determine publication from the destination Level array before validating names, World state and attachments.

- [x] Separate prepared Actor publication from batch removal, append new Actors and restore order through Level snapshots. Verify cancellation between boundary detaches and attachment before registration.

- [x] Gate editor-owned buffer replay on the host World, enforce sibling identity at rename and reparent boundaries, and restore inactive Actor ownership without interpreting Level snapshots in the transactor. Preserve combined attachment-cycle preflight.

- [x] Share Actor publication completion and activation and return the surviving duplicate selection. Verify partial publication, Undo and Redo.
- [x] Resolve cross-Actor links after publication in a second pass, including child-first selection, existing parent lookup and unresolved parents. Restore Actor root links in world space. Remap references before one PostLoad per created object and skip retired candidates. Keep the complete AddActor operation public and registration private.

### Stage 4: Move mounted-content discovery to its owner

Depends on stage 3. Keep package checkpoints synchronized with successful history transitions. Content discovery accepts the existing content change batch from both history and external editors. Preserve synchronous object notifications and reconstruction order.

- [x] Move journal ownership and revision queries to the editor content service. Migrate MainFrame, LevelEditor, history and test consumers and remove their transactor forwarding APIs.
- [x] Verify revision and batch contents for execute, undo, redo, external change, no-op, cancel and failure. Verify dirty checkpoints across save, undo and redo, including notification failure behavior.
- [x] Audit Engine, Sandbox and RoadWeaver source and tests for retired APIs. Build affected owners and consumers, run relevant history, property, material and content tests, and update implemented contracts.

Use the [build](../Agents/BuildAndRun.md) and [test](../Agents/Testing.md) workflows for bounded selections and evidence reuse.
