# Material Graph Transaction Alignment Plan

Summary: Align material and function editing with UE 5.8 by recording authored objects before mutation and retiring both material-specific transaction builders.

Last reviewed: 2026-11-20

Status: Active
Completed:

## Current Status

Planned on base `71a4ecc65`. The completed [material graph object migration](MaterialEditorGraphObjects.md) supplies graph, node and schema entry points. Simple connections and same-class constants already reach object recording. Compound edits still use `FGraphEditSession`, and several ordinary operations use the deferred writes in `FExpressionEdit`. This was the starting point for implementation.

The [editor transaction plan](EditorTransactionSimplification.md) owns the shared buffer and scoped command submission. This plan owns MaterialEditor callers, the data they transact and graph reconstruction. It can proceed on the current scoped transaction and object recording APIs.

Stage 1 migrated constants, port metadata, renames, ordinary links and input defaults to synchronous object recording at the command boundary. The workspace inventory found implementation callers in MaterialEditor and direct compound-session fixtures in Engine tests. Material and function owners store expression membership, expressions own links and numeric fields, and preview/clipboard/structural callers still use provisional graph validation. Anonymous owners require separate replay coverage in stage 2. On base `2214932e3` the Win64-Debug-DurinEditor MaterialGraphEditingTests passed 114 cases. The strengthened nested swizzle case also passed with two recorded objects, a rejected inner edit and unchanged undo depth.

The stage 2 material prototype was withdrawn to isolate the shared GC change. It was developed on base `69a3a6d28`. In the Win64-Debug-DurinEditor prototype, 114 of 115 MaterialGraphEditingTests passed. The new create-and-edit case fails on Redo after a successful Undo. The node is absent at the start of Redo even though the same transaction will reinsert it. Validating each historical expression as a current graph member rejects that transaction. The prototype is not stage acceptance.

The isolated retention change on base `c253ba185` passed the Win64-Debug-DurinEditor all-target build, 404 EditorOperationTests, 10 EngineGarbageCollectionTests and 19 CoreTransactionContractTests. The new regression exercises a redo entry across two garbage collections, checked rejection of an externally deleted target, and physical release after history reset. The existing class-default recreation test verifies that a logical reference can resolve a replacement after its cached leaf expires.

The shared lifecycle prerequisite now records pending-kill changes on a whole-object `Modify()` and restores them during directional replay. The protected `FTransaction::FObjectRecord` holds whole-object, array or custom change data, matching the UE 5.8 transaction boundary. Durin retains its reflected array locator and checked replay compensation. At this prerequisite milestone, material membership migration, anonymous-owner coverage and editor completion were still open. On base `cea527596`, the Win64-Debug-DurinEditor all-target build and five native targets passed: EditorOperationTests (408), CoreTransactionContractTests (19), EngineGarbageCollectionTests (10), EditorPropertyTests (76) and MaterialGraphEditingTests (114). The mixed array and whole-object regression verifies two Undo/Redo cycles across GC after deleting the range owner.

Stage 2 replaces the material graph custom history entry and its expression membership annotation with native owner and expression records. On base `255188d5e`, the graph tests cover mixed creation/edit and edit/deletion across GC, atomic nested paste rejection, and separate same-GUID replacements on Undo and Redo. Both material and function collections use the Durin `ValidateTransactionSource` metadata check on existing serialized membership. This preserves the accepted stale-edit contract without storing a second membership snapshot. Toolkit completion follows UE's `PostUndo` and `PostRedo` placement. Temporary command validation remains in the existing session until stage 4.

Stage 2 acceptance passed on Win64-Debug-DurinEditor. The all-target build passed across the workspace. MaterialGraphEditingTests passed 116 cases, followed by a separate passing physical-loss case using a standalone transaction without GC ownership. MaterialEditorInteractionTests passed 76, MaterialEditingPersistenceTests 40, MaterialFunctionTests 31, EditorOperationTests 408 and CoreTransactionContractTests 19. The open caller graph follows function port names through Undo and Redo. The working-copy tests cover global history publication, isolation and save/reload.

### Stage 2 prerequisite contracts

| Responsibility | UE 5.8 reference and Durin change | Acceptance |
| --- | --- | --- |
| Object lifecycle and history | UE's transactor normally reports retained references to GC and flips pending-kill state on resolved objects. Durin guarantees the authored result of a supported Undo, including node GUID, class, values, ordering and links. Callers reacquire nodes from the restored graph. The allocation address is an implementation detail. The shared undo buffer must retain allocations reached through the history references it reports to GC, including explicitly garbage participants. Ordinary lookup continues to hide them. | Create then edit, and edit then delete, in one outer transaction. Undo and Redo across normal GC restore the authored result. If a required target was actually destroyed and cannot be resolved, preflight rejects the entire action before writes. |
| Membership and preflight | The material or function owns its ordered expression collection. The expression owns its fields. Check the live starting state once for the requested direction and validate the complete set of target references before writes. A participant that this entry will restore is admissible. An independent replacement with the same authored GUID is stale. Keep schema and reference checks for both historical payloads. | For both Undo and Redo, external replacement fails before any writes, notifications or cursor movement. A legitimate same-entry insertion or removal succeeds. |
| Deleted-node access | The earlier test contract allows retained callers to mutate a deleted expression and requires history to discard those writes. Align callers with object liveness: an expression removed and marked as garbage is unavailable for editing. Read restored values through the graph and its authored node ID. Retire the prototype detached-field command when native lifecycle coverage passes. | A caller holding a reference cannot edit a deleted node through supported APIs. Restoring the node recovers the fields captured by the transaction. |
| Editor completion | UE `FMaterialEditor::PostUndo(bool)` repairs editor references and refreshes views after success. `PostRedo(bool)` delegates to it. Put view refresh in the material editor, retain owner invariants when no toolkit is open, and use the existing change subscription for ordinary graph observers. | Current material and function views, including an open caller of a changed function, observe the settled graph. A failed replay leaves the views unchanged. |

First test the mixed recording with no GC. It already fails in that configuration, so its failure does not establish a GC requirement. Exercise ordinary GC and actual target loss separately. Use UE's collector policy on existing history edges. Eviction or reset releases those edges. Keep garbage-state restoration and directional replay validation as separate prerequisites for material integration. A membership generation, cached replay owner, per-transaction material registry or second detached-value history would duplicate this responsibility.

## Goal

A user action opens or joins one scoped transaction, calls virtual `Modify()` on each authored object before its first write, applies the edit, then publishes the completed graph. Undo and Redo restore recorded objects through the shared buffer. Material code reconciles references, rebuilds derived pins and refreshes observers. Delete `FGraphEditSession`, `FExpressionEdit` and the generic graph change assembled from member snapshots. A shared helper such as UE's `ModifyMaterial()` may enumerate known owners directly. A replacement session that queues writes or computes generic before/after diffs would miss the goal.

### UE 5.8 reference and Durin contracts

UE paths below are relative to `Engine/Source/Editor` in the local UE 5.8 source tree.

| Concern | UE reference or Durin requirement | Contract for this migration |
| --- | --- | --- |
| Command scope and capture | In `MaterialEditor/Private/MaterialEditor.cpp`, the new-expression action opens an `FScopedTransaction` and calls `FMaterialEditor::ModifyMaterial` before creation. That helper records the graph, material, editor-only data and applicable function objects. | Put the boundary at the editing action. Use the actual Durin owner of each write, including downstream expressions affected by type propagation. Ordinary expression undo adopts whole-object granularity. A later independent write to another transactional field of that same object may be restored with it. |
| Connections and replay | `UMaterialGraphSchema::TryCreateConnection` in `UnrealEd/Private/MaterialGraphSchema.cpp` delegates the pin edit and then updates the material. `FMaterialEditor::PostUndo` repairs back-references and refreshes editor state. | Keep admission and link writeback in the schema/graph. Restore historical authored values before rebuilding derived state. Reuse recorded widths, defaults and port IDs during replay. |
| Failure and nesting | Durin currently restores provisional member and membership writes in the session destructor. Scoped cancellation discards recording and leaves applied writes in place. | Move fallible validation ahead of writes. Where a command can still partially apply, that command owns compensation. A rejected nested operation preserves earlier outer edits. Keep domain rejection distinct from failure to publish history after an edit has succeeded. |
| Working copy and gestures | Durin has explicit transaction modes, typed command results, preview sessions and change notifications. | Preserve Apply/Save isolation, one undo step per gesture and the preview/cancel behavior. Standalone preview or `NotTransactable` operations follow their existing admission rules. Publish semantic changes once per affected owner after all writes. Layout-only changes preserve unrelated nodes and avoid semantic compilation. |

## Implementation Stages

### Stage 1: Record ordinary edits at the command boundary

- [x] Inventory both builder types across Engine, Sandbox and RoadWeaver source and tests. Map each caller to its authored owner, affected objects, last fallible step and transaction mode. Include function and texture previews. Identify any owner that cannot replay a whole-object baseline, including unnamed working objects.
- [x] Migrate ordinary fields, input defaults, expression renames and nonstructural links. Compute candidate values, affected inputs and downstream numeric types first. Record objects before applying accepted values. Delete the deferred write list and `FExpressionEdit` once its consumers migrate.
- [x] Verify one history entry and one baseline per changed object for a compound field/link action. Verify nested edits, undo/redo of inferred widths and retained defaults, and the agreed same-object field granularity. A no-op or rejected request leaves graph, history and package dirtiness intact.

### Stage 2: Restore graph membership and node identity through object history

- [x] Trace the serialized membership owner for materials and functions. Migrate creation and deletion first. Record membership before mutation, reconcile ownership and back-references after replay and let callers reacquire nodes by authored ID. Handle stale members following external replacement before applying writes.
- [x] Use create/delete as the acceptance gate for replacement, duplication, clipboard paste and function-binding edits. Record all affected owners and endpoints in the same action. Preflight naming, IDs, cycles and link compatibility wherever failure would otherwise leave a partial edit.
- [x] Verify delete, GC, Undo and Redo restore authored node GUIDs, classes, fields, order and links for a valid entry. Verify actual target loss rejects the complete entry before writes and cursor movement. Verify multi-node paste is one step, invalid paste preserves an enclosing transaction, and a function port change refreshes an open caller. Observers must see a queryable, complete graph on execute and replay.

### Stage 3: Align layout and gesture recording with data ownership

- [ ] Map persisted positions and labels to independently recordable node state. Specify writeback and reconstruction across graph rebuild, function reopen and working-copy Apply. If serialization ownership changes, migrate existing GUID-keyed layout on load and verify save/reopen before removing its former representation.
- [ ] Move layout, parameter and configuration history to the objects that own the authored values. Keep temporary pointer/slider feedback and Escape restoration local to the gesture. Audit each remaining material `StoreUndo` call against a concrete replay requirement. Document any required Durin-specific command, its affected state and its regression test.
- [ ] Verify one released multi-node drag gives one undo entry. Undo a move on A after an independent change on B and verify B's position survives. Check cancellation, a stale gesture, parameter preview and retry, mixed ordinary/configuration edits in both orders, and an anonymous configuration-only owner. Layout edits must leave the semantic compile revision unchanged.

### Stage 4: Remove generic graph replay and verify editor consumers

- [ ] Migrate remaining preview, creation, clipboard and tool callers. Remove `FGraphEditSession`, its per-member snapshots and `FGraphObjectChange` after the preceding gates pass. Search all workspace projects for both retired builders, equivalent write queues and generic material replay. Record the final disposition of any specialized commands from stage 3.
- [ ] Verify material and function working copies, Apply, Save, reopen, undo/redo, external replacement and preview cancellation through their normal editor entry points. Refresh the implemented [graph operations](../Editor/Architecture/Materials/MaterialGraphOperations.md) and [editor lifecycle](../Editor/Architecture/Materials/MaterialEditorLifecycle.md) contracts.
- [ ] Build the owning and affected consumer targets. Run MaterialGraphEditingTests, MaterialEditorInteractionTests and MaterialEditingPersistenceTests, plus the affected property, transaction and tool cases selected through the [test workflow](../Agents/Testing.md). Record revision, configuration, results and remaining UE differences before closing the plan.
