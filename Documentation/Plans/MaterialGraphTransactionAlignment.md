# Material Graph Transaction Alignment Plan

Summary: Align material and function editing with UE 5.8 by recording authored objects before mutation and retiring both material-specific transaction builders.

Last reviewed: 2026-11-20

Status: Active
Completed:

## Current Status

Planned on base `71a4ecc65`. The completed [material graph object migration](MaterialEditorGraphObjects.md) supplies graph, node and schema entry points. Simple connections and same-class constants already reach object recording. Compound edits still use `FGraphEditSession`, and several ordinary operations use the deferred writes in `FExpressionEdit`. This was the starting point for implementation.

The [editor transaction plan](EditorTransactionSimplification.md) owns the shared buffer, standalone command submission and mounted-content journal. This plan owns MaterialEditor callers, the data they transact and graph reconstruction. It can proceed on the current scoped transaction and object recording APIs.

Stage 1 migrated constants, port metadata, renames, ordinary links and input defaults to synchronous object recording at the command boundary. The workspace inventory found implementation callers in MaterialEditor and direct compound-session fixtures in Engine tests. Material and function owners store expression membership, expressions own links and numeric fields, and preview/clipboard/structural callers still use provisional graph validation. Anonymous owners require separate replay coverage in stage 2. On base `2214932e3` the Win64-Debug-DurinEditor MaterialGraphEditingTests passed 114 cases. The strengthened nested swizzle case also passed with two recorded objects, a rejected inner edit and unchanged undo depth.

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

- [ ] Trace the serialized membership owner for materials and functions. Migrate creation and deletion first. Record membership before mutation, keep historical nodes reachable through GC and reconcile ownership and back-references after replay. Handle stale members following external replacement before applying writes.
- [ ] Use create/delete as the acceptance gate for replacement, duplication, clipboard paste and function-binding edits. Record all affected owners and endpoints in the same action. Preflight naming, IDs, cycles and link compatibility wherever failure would otherwise leave a partial edit.
- [ ] Verify delete, GC, Undo and Redo restore the original node identity, fields, order and links, including a detached node modified by a retained reference. Verify multi-node paste is one step, invalid paste preserves an enclosing transaction, and a function port change refreshes an open caller. Observers must see a queryable, complete graph on execute and replay.

### Stage 3: Align layout and gesture recording with data ownership

- [ ] Map persisted positions and labels to independently recordable node state. Specify writeback and reconstruction across graph rebuild, function reopen and working-copy Apply. If serialization ownership changes, migrate existing GUID-keyed layout on load and verify save/reopen before removing its former representation.
- [ ] Move layout, parameter and configuration history to the objects that own the authored values. Keep temporary pointer/slider feedback and Escape restoration local to the gesture. Audit each remaining material `StoreUndo` call against a concrete replay requirement. Document any required Durin-specific command, its affected state and its regression test.
- [ ] Verify one released multi-node drag gives one undo entry. Undo a move on A after an independent change on B and verify B's position survives. Check cancellation, a stale gesture, parameter preview and retry, mixed ordinary/configuration edits in both orders, and an anonymous configuration-only owner. Layout edits must leave the semantic compile revision unchanged.

### Stage 4: Remove generic graph replay and verify editor consumers

- [ ] Migrate remaining preview, creation, clipboard and tool callers. Remove `FGraphEditSession`, its per-member snapshots and `FGraphObjectChange` after the preceding gates pass. Search all workspace projects for both retired builders, equivalent write queues and generic material replay. Record the final disposition of any specialized commands from stage 3.
- [ ] Verify material and function working copies, Apply, Save, reopen, undo/redo, external replacement and preview cancellation through their normal editor entry points. Refresh the implemented [graph operations](../Editor/Architecture/Materials/MaterialGraphOperations.md) and [editor lifecycle](../Editor/Architecture/Materials/MaterialEditorLifecycle.md) contracts.
- [ ] Build the owning and affected consumer targets. Run MaterialGraphEditingTests, MaterialEditorInteractionTests and MaterialEditingPersistenceTests, plus the affected property, transaction and tool cases selected through the [test workflow](../Agents/Testing.md). Record revision, configuration, results and remaining UE differences before closing the plan.
