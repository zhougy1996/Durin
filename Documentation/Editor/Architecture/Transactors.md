# Editor Transactor Core

Summary: Define UE-shaped native recording, synchronous Undo/Redo, package checkpoints, history notifications, and collector ownership.

Modules: Core, CoreDObject, Engine, DurinEd

Last reviewed: 2026-10-19

## Service Ownership

The process has one active `DEditorEngine`, published during activation rather
than object construction. It owns one transient `DTransBuffer` in `Trans`.
Windows, documents, tools and script sessions share this ordered history.
`DEngine` exposes BeginTransaction, EndTransaction, CancelTransaction and
CanTransact; the editor implements them through its transactor.

Core defines the runtime-neutral `ITransaction` capture interface and ordinary
GameThread pointer `GUndo`. Outermost Begin publishes the pending `FTransaction`;
End, Cancel, owner retirement and destruction clear it before releasing storage.
A competing buffer cannot begin while another recording is published. There is
no candidate registry or host recorder binding stack. Object capture retains
Durin's typed failures through the owning buffer; CoreDObject has no editor dependency.

Host callback scopes protect execution and GC lifetime, not transaction routing.
Ending a callback never ends recording. Without a current recording, Modify
keeps its normal dirty behavior and returns false. Undo/Redo reject an active
recording and do not expose replay as a new recording.

## Transactions And Scoped Recording

Recording and replay run synchronously on GameThread. `Begin(Context,
Description)` returns the previous active-action count: zero for the outermost
scope, one for its first nested scope. Unavailable Begin returns -1. `End()`
returns the count before closing one action, or zero without an active recording.
Only outermost End captures final object values and commits one history entry.
Nested contexts do not replace the outer context. `SetPrimaryUndoObject` sets the
first primary object for presentation and retention; it is not an affected package.

The reference is UE 5.3.2 at mirror revision
`c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf`:
[EditorTransaction.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Source/Editor/UnrealEd/Private/EditorTransaction.cpp),
[ScopedTransaction.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Source/Editor/UnrealEd/Private/ScopedTransaction.cpp),
[Obj.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Source/Runtime/CoreUObject/Private/UObject/Obj.cpp),
[Actor.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Source/Runtime/Engine/Private/Actor.cpp), and
[Property.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Source/Runtime/CoreUObject/Private/UObject/Property.cpp).
This source version's `Cancel(StartIndex)` cancels the entire active recording,
including cancellation by an inner scope. It does not restore applied edits.
The index is an activity count, never a history ID or an isolated savepoint.

`FScopedTransaction` provides UE-shaped description and full-context constructors
using the active editor. A disabled scope or unavailable editor remains inert.
Simple panel actions such as actor visibility and primary-camera selection use
these constructors rather than passing the process editor's transactor through
panel contexts. Reusable property/material backends and suspended script scopes
retain explicit transactors where exact recording ownership is part of their contract.
The explicit-transactor overload supports native composition and independent
low-level buffer tests; it cannot open a competing concurrent recording.
Cancel(), IsOutstanding() and optional End are retained, and destruction ends
an outstanding action.
Scope helpers retain a GC-visible exact transactor reference and its existing
active transaction ID. End, Cancel, IsOutstanding and destruction first validate
that same live recording. Cancel/End followed by a replacement Begin makes old
helpers inert; their nested action count cannot close or cancel the replacement.
Destroyed transactors invalidate helper cleanup. There is no public owner token,
private history, extra recording generation, object snapshot or checked preparation
in the helper. Explicit scopes may remain open between host execution boundaries;
all callers on the active editor join that global recording.

### Everyday Object Editing

```cpp
Editor::FScopedTransaction Transaction("Move Actor");
Actor->Modify();
Actor->SetActorTransform(NewTransform);
```

`DObject::Modify(bool bAlwaysMarkDirty = true)` returns whether the active
transaction captured the object. False does not veto an edit. Capture requires
an eligible live object with `EObjectFlags::Transactional`. Editor actor/component
creation establishes the flag, children inherit it from their Outer, and known
saved package activation flags its loaded graph. Explicit low-level callers may
set or clear it. Templates are excluded from Durin's ordinary object editing.

`FTransaction` owns first-before capture and deduplicates objects across every
nested action. Final-after capture happens at outermost End, so 0, outer write 1,
inner write 2, outer write 3 replays as Undo 0 / Redo 3. Focused property records
for the same object/member/index share first-before and latest-after state with
whole-object recording. AActor overrides Modify to include its native root
component, matching the pinned Actor.cpp behavior; generated roots are rebuilt
by construction. Direct references to Generated objects are excluded from
automatic capture. Derived component membership, reverse attachment lists, and
world-transform caches are NonTransactional and rebuilt by replay hooks.
Final capture includes reflected Transient fields;
`NonTransactional` and Deprecated fields are excluded, including reflected
fields inside struct payloads. Transaction archives carry their own filtering
mode through capture and restore. Transaction participation
and persistence exclusion are independent policies.

Successful capture marks the package dirty. With `bAlwaysMarkDirty`, an
uncaptured call also marks it dirty, including calls outside a recording.
An unchanged Modify can therefore leave a dirty package without a history entry.
No-op pruning and cancellation do not promise to clear dirtiness.

## Structural History And Barriers

`DTransactor::GetQueueLength()` reports the retained history size, including
both Undo and Redo entries. `GetTransaction(Index)` reads an entry in that history.
`DTransBuffer::GetUndoCount()` follows UE's meaning: the number of transactions
already undone and available for Redo. `GetUndoableCount()` reports entries before
the cursor that can be undone. Their sum equals `GetQueueLength()`. For example,
after three commits and two Undo operations, the counts are two undone and one
undoable. Redo decreases the undone count; committing a new branch discards the
remaining Redo entries and resets that count to zero.

History entries before the cursor are undoable; entries after it are redoable.
A successful new commit replaces the redo suffix. IDs remain monotonic across
Reset. Ordinary `Undo(bool bCanRedo = true)` and `Redo()` need no expected ID;
`Undo(false)` discards the redo suffix. `UndoExpected` and `RedoExpected` are
optional guarded extensions. `CanUndo`/`CanRedo` can explain unavailability;
Reset retains a caller-supplied reason. Queue, transaction, context, object, and
description queries expose the same native history without consuming it.

Idle and Recording accept ordinary composition through `CanTransact()`.
Undoing, Redoing, RecoveryRequired, and Destroying reject it. Undo/Redo cannot
reenter recording. Ordinary scopes provide neither caller isolation nor an
all-or-nothing execution contract.

`FCommandChange` owns synchronous `Apply()`/`Revert()` behavior, descriptive data,
package effects, module ownership, byte accounting, and GC references. Domains
execute changes and call `StoreUndo(Object, Change)` to retain already-applied
undo data. A prior custom record for that object suppresses a later Modify
snapshot, matching the pinned UE per-object rule. A custom record added after
a snapshot remains ordered after it; callers must choose the representation
that describes their operation. Unbound composite changes may use the
`StoreUndo(Change)` convenience and must enumerate all participants themselves.
Durin additionally opens a standalone scope for StoreUndo without active recording.
StoreUndo never invokes Apply and accepts nested recording without an opt-in flag.

`ApplyEditorChange` is the synchronous domain execution helper. It opens or joins
a scope, defers collection through initial Apply, and stores the change on
success. Invalid initial accounting rejects before mutation. Final capture or
accounting failure can follow applied mutations; ordinary recording does not
compensate them automatically. Actor/Material typed errors stay in their domains;
the generic history boundary owns only a message and recovery disposition.
Deferred completion callbacks, Executing state, synchronous opt-in, public
scope/record tokens, Execute, and CommitApplied are absent.

Property replay restores owned historical payloads directly after structural,
type, and exact-identity checks. It bypasses new-edit proposal validation and
normalization. Distinct target objects receive balanced PreEditUndo/PostEditUndo
around the complete replay. Per-property notifications precede completion;
domains retain their derived-state synchronization. See
[reflected property editing](ReflectedPropertyEditing.md#object-notification-contract).

When a replay record fails, completed records are compensated in reverse order.
Only successful replay advances the cursor. Incomplete compensation or a custom
exception retains the participant graph and requires recovery. Reset does not
repair data or unlock catastrophic recovery. Diagnostic results retain actionable
failure and rollback messages without requiring ordinary clients to inspect causes.

Transaction events are queued during transitions and delivered by DispatchEvents
to independent listeners after the transactor reaches Idle or RecoveryRequired.
UI notifications register as a listener; observers receive the same immutable
transition data without stealing events. The queue is not a persistent journal.

## Package And Discovery Revisions

Each committed transaction owns transitions only for actually affected packages.
The primary context does not implicitly dirty its package. Successful replay
applies before/after session revisions; a valid saved checkpoint clears dirty
state when Undo reaches the saved revision and marks it dirty when Redo leaves.
Saving at a history position preserves redo. A replacement branch receives fresh
revision identities. Invalid checkpoints remain conservative until save/activation.

Package edit revisions detect modifications outside retained history before the
next recording/replay/save transition. Unchanged or canceled Modify and unrecorded
writes cannot later clear dirty state merely because Undo reaches an old saved
revision. Callers publishing persistent changes outside history should continue
to invalidate the saved checkpoint explicitly. Checkpoint metadata is session-only.

Saved package reload prepares only while Idle and retires every whole history
entry whose collector-visible data retains a replaced graph. Unrelated entries
remain; surviving modified packages lose invalidated checkpoints. Precommit reload
failure leaves history and package states unchanged. Successful activation
establishes the replacement graph's saved checkpoint.

Custom mounted-content changes advance the discovery revision after successful
commit/Undo/Redo. Ordinary in-memory property edits do not request registry scans.

## Bounds And Accounting

The production soft budget is 64 MiB of owned native transaction data, without
an entry cap or dynamic limit API. Accounting includes context, record storage,
payloads/reference tables, capture metadata, and reported custom allocations;
it excludes the transitive retained DObject graph. Aggregation checks overflow.

Before appending a successful new entry, trim oldest existing history while the
old history occupies at least the budget. The incoming entry remains undoable
even when oversized. Redo replacement skips additional prefix trimming. Canceled,
empty, and failed submissions preserve the existing history. Undo/Redo do not trim.
Compared with UE's Begin-time trimming, Durin delays destructive trimming until
successful finalization so no removed-transaction restoration machinery is needed.

## Collector And Shutdown Contract

The reachable buffer enumerates pending, undoable, and redoable transaction data,
primary objects, targets, capture participants, package transitions, tracked
package states, and domain-declared hard dependencies. Persistent references use
exact object generations; weak/soft payloads do not gain retention. Marked-garbage
objects are not rescued. See [transaction records](TransactionRecords.md).

Cancellation, branch replacement, eviction, Reset, module retirement, destruction,
and editor shutdown erase the corresponding record data and collector edges.
Module history retirement requires Idle, so callbacks cannot erase a record
currently replaying. A module cannot unload while retained custom executable
code requires it; recovery
participants remain until their buffer is retired.

## Related Documentation

- [Transaction records](TransactionRecords.md)
- [Reflected property editing](ReflectedPropertyEditing.md)
- [Editor function actions](EditorFunctionActions.md)
- [Editor tools](EditorTools.md)
