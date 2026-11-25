# Editor Transactor Core

Summary: Define UE-shaped native recording, synchronous Undo/Redo, package dirty state, history notifications, and collector ownership.

Modules: Core, CoreDObject, Engine, DurinEd

Last reviewed: 2026-11-24

## Service Ownership

The process has one active `DEditorEngine`, published during activation rather
than object construction. It owns one transient `DTransBuffer` in `Trans`.
Windows, documents, tools and script sessions share this ordered history.
`DEngine` exposes BeginTransaction, EndTransaction, CancelTransaction and
CanTransact; the editor implements them through its transactor.

Core defines the game-thread recording pointer `GUndo` and the
[runtime capture interface](../../../Runtime/ObjectSystem/Transactions.md).
Outermost Begin publishes its transaction there. While it is published, another
buffer cannot start recording. Object edits capture through virtual `Modify`.
Commands submit already-applied changes through `StoreUndo`. Host callbacks
request GC and the host collects after the operation returns.

A failed whole-object snapshot invalidates the entire recording. The outer End,
Cancel or Reset reports the original capture error and terminates the process.

## Transactions And Scoped Recording

Commands use `bShouldTransact` to request a transaction when needed.
`ResolveEditorTransactor(false)` returns the active editor recorder during an
enclosing transaction and null outside one. Objects modified by a nested
command participate in the enclosing history entry. Recorded commands require
an available editor. Long-lived edit sessions retain their original host.

Recording and replay run synchronously on GameThread. `Begin(Context,
Description)` returns the previous active-action count: zero for the outermost
scope, one for its first nested scope. Unavailable Begin returns -1. `End()`
returns the count before closing one action, or zero without an active recording.
Only outermost End captures final object values and commits one history entry.
Nested contexts do not replace the outer context. `SetPrimaryUndoObject` sets the
first primary object for presentation and retention; it is not an affected package.

The outermost End synchronously broadcasts `OnPreTransactionFinalized()` with the
current transaction context while the recording depth is one and GUndo still points to
the recording. Listeners may open and close nested transactions and call Modify.
Those edits join the outer entry. A nested End returns its previous depth and
does not broadcast again. A direct recursive End at depth one is refused.
If a listener retires the recording, the original End leaves any replacement
recording alone. A listener exception on the current recording follows the
recording failure contract. After listeners return, End decrements the depth
and finalizes at zero.

`Cancel(StartIndex)` cancels the entire recording, including from an inner
scope. Applied edits remain. The start index is a nesting depth. The baseline
for these semantics is [UE 5.8.0, CL 53629095](../../../Plans/Archive/2026-10/UE58TransactionArchitecture.md#reference-identity-and-reproduction).

`FScopedTransaction` pairs Begin with End or Cancel on the original editor
and recording. A disabled scope leaves an enclosing recording available to
Modify and StoreUndo. After external cancellation, `IsOutstanding()` can
still be true while `IsRecordingCurrent()` is false. The old scope can unwind
safely after a replacement recording starts. Scopes are noncopyable and
nonmovable. Independent buffer users pair Begin with End or Cancel directly.

### Everyday Object Editing

```cpp
FScopedTransaction Transaction("Move Actor");
Actor->Modify();
Actor->SetActorTransform(NewTransform);
```

`DObject::Modify(bool bAlwaysMarkDirty = true)` returns whether the object
was admitted to capture. False does not veto an edit. Capture requires
an eligible live object with `EObjectFlags::Transactional`. Editor actor/component
creation establishes the flag, children inherit it from their Outer, and known
saved package activation flags its loaded graph. Explicit low-level callers may
set or clear it. Templates are excluded from ordinary editor property/script
editing; native default-graph transactions have explicit
[runtime admission](../../../Runtime/ObjectSystem/Transactions.md#coredobject-boundary).

`FTransaction` owns first-before capture and deduplicates objects across every
nested action. Final-after capture happens at outermost End, so 0, outer write 1,
inner write 2, outer write 3 replays as Undo 0 / Redo 3. Property editing and
explicit whole-object recording use that same first-before baseline. AActor overrides Modify to include its native root
component, following the pinned Actor.cpp traversal. A failed capture of either eligible object invalidates the recording. Generated roots are rebuilt by construction. Direct references to Generated objects are excluded from
automatic capture. Derived component membership, reverse attachment lists, and
world-transform caches are NonTransactional and rebuilt by replay hooks.
Final capture includes reflected Transient fields;
`NonTransactional` and Deprecated fields are excluded, including reflected
fields inside struct payloads. Transaction archives carry their own filtering
mode through capture and restore. Transaction participation
and persistence exclusion are independent policies.

General capture calls virtual Serialize once for the initial state and once at
finalization; repeated Modify does not duplicate the object record. Replay also
uses save traversals for schema checks and inverse capture, so serializers must
keep saving free of domain mutations. Each object has one canonical whole-object
replay owner; there is no focused undo or promotion path. [Transaction records](TransactionRecords.md) defines inverse capture and replay failure behavior.

`Modify(false)` requests capture without directly marking the package dirty.
A committed edit marks its affected packages dirty at outermost End.
The default `Modify()` also requests package dirtiness, even outside a recording.
An unchanged or canceled default call can therefore leave a dirty package without a history entry.

## Structural History And Barriers

`DTransactor::GetQueueLength()` reports the visible queue length, including
the active entry during recording. `GetTransaction(Index)` reads an entry in that history.
`DTransBuffer::GetUndoCount()` counts retained transactions after the cursor,
including expired entries crossed by Undo. `GetQueueLength() - GetUndoCount()`
counts entries before the cursor. Redo moves entries across the cursor.
Outermost Begin hides the Redo suffix and sets `GetUndoCount()` to zero.
An effective End permanently discards that suffix. IDs remain monotonic across
Reset. Ordinary `Undo(bool bCanRedo = true)` and `Redo()` need no expected ID;
`Undo(false)` discards the redo suffix. Delayed notification actions use
`UndoExpected` and `RedoExpected` with the nonzero ID captured from their event.
The action is available only when its ID matches the checked context at the
head. Invocation checks the ID again before advancing history.
`CanUndo`/`CanRedo` can explain unavailability;
Reset retains a caller-supplied reason. GetUndoContext/GetRedoContext return
owned context values for the adjacent queue entry, including an expired entry. GetUndoContext() is empty
when Undo is unavailable. GetUndoContext(false) returns the active queue tail while
recording. GetRedoContext() is empty when Redo is unavailable. Workspace label
adapters derive owned strings from these contexts, so labels survive history
replacement/reset. The transactor has no separate description-query interface.

`FCommandChange::HasExpired(DObject*)` and captured object annotations can
declare a record ineligible for replay. The buffer advances past wholly expired history
entries without publishing Undo or Redo events and applies the next eligible
entry within the undo barrier. Each candidate crosses the cursor before its
expiry check. During Apply, GetUndoCount and GetUndoContext(false) see that
position; checked contexts and nested replay are unavailable. Undo(false)
drops the Redo suffix after each candidate, including each expired entry.
Guarded replay refuses a mismatched or expired head. A read-only record refusal
leaves the rejected candidate on its original side; already crossed expired
entries stay consumed in that direction.
The transaction selects the records once per replay. Object events use that
selection and its completion annotations. The buffer consumes the transaction's
package outcomes and event details after success. Expiry is evaluated again for the next operation. A package touched only by skipped records keeps its
dirty state. A package touched by both kinds is marked dirty.

Command details describe the recorded edit in its original direction. Undo and Redo treat these strings as optional presentation data. A
throwing detail provider emits a diagnostic and contributes an empty string;
other records can still supply details. An exception from required replay work with unknown effects follows the execution-failure contract below.

IsActive() reports whether this buffer's recording depth is positive. ActiveCount
and the pending transaction's ownership of GUndo identify the recording.
CanTransact() permits new or nested edits outside capture and guarded history
operations. The pre-finalization listener can still open and close nested edits.

Undo and Redo share the same replay admission check: the buffer must be idle,
GUndo must be clear, and the host must permit replay. The requested direction
selects the history head and completion delegate. Object events use UndoRedo in either direction. Both directions
guard the buffer against reentrant edits and replay. Cleanup and destruction
also block mutation, inspection prevents recursive head queries, and finalization
hides entries while the buffer publishes or retires them.

Reset cancels the complete active recording and purges history, barriers and per-entry package records without reverting applied edits or clearing package
dirty flags. It snapshots the reason before invoking record destructors and
blocks reentrant recording, reset and inspection throughout cleanup.
A reset requested during replay is refused.

### Undo Barriers And PIE References

SetUndoBarrier pushes the committed cursor, RemoveUndoBarrier pops one bound,
and ClearUndoBarriers removes all bounds. Only the top bound restricts Undo;
Redo is unrestricted. GetCurrentUndoBarrier returns `-1` without a
barrier. CanUndo, Undo, UndoExpected and checked context queries all honor the
same bound. Controls require Idle with no global recording, preventing replay
callbacks from changing admission. Reset clears the stack. Prefix eviction,
branch removal, package/module retirement and checked reinstancing remap every
bound by the removed queue positions; a bound never points beyond retained data.
Reinstancing uses the current history and remaps the current barriers at publication.
Barrier refusal has its own rejection reason, separate from an unexpected head
ID. CanTransact, CanUndo and CanRedo all reject another buffer's live GUndo;
unavailable history queries return a nonempty explanation.

PIE membership comes from the package's session flag. `ContainsPieObjects()`
examines the visible queue, including the active entry and both recorded sides.
It follows declared strong references from targets, primary context, commands,
annotations, array values and package transitions. Weak and soft references
retain their usual semantics. The buffer query conservatively returns true on a
busy traversal or inspection error.

Typed arrays reject PIE owners before mutation. Outermost End treats an entry
with PIE references as transient. During replay, an existing PIE reference
rejects admission. All inverses are inspected after capture and before the first load. A PIE
reference at that point is an execution fault. Load and completion callbacks can
change references after that check. The operation succeeds and retains its history. The next replay
checks its selected entry at admission.

Record `CaptureAfter()` and transaction `Finalize()` return `void`. Each whole
record marks itself finalized before capturing its terminal state. A capture
fault reports the transaction ID, Finalize phase, original record index and cause.
A second direct Finalize, including one invoked by a Finalized listener, can
deliver the stored primary delta again in the current operation. Only the first
capture supplies the terminal annotation and its additional-object events.
Listeners that call Finalize must bound their own recursion.

Required capture, inspection, synchronous lifecycle or dirty-notification faults
during finalization terminate with the transaction ID, phase and original cause.

### Synchronous Object Events

`PostTransacted` receives Finalized before the recording operation closes and
`GUndo` clears. Finalize selects the first record for each object. Replay calls PreEditUndo on
first encounter in replay order and selects the last visited record for each
object. After all loads, each object's PostEditUndo immediately precedes its
UndoRedo notifications. Both finalization and replay visit components first,
preserving encounter order within each group. Events carry the
transaction ID, live operation ID, recorded delta, original identity and
current annotation. Record events precede their annotation's additional events;
`CreatedBy` distinguishes these sources, matching the pinned UE implementation.
An object can receive both sources; additional events carry no primary annotation.
Finalized annotation deltas compare both the initial annotation and the last
snapshot annotation so returning to the initial state still closes an observed edit.

`SnapshotObject` observes an existing serialized record during active recording.
Selected top-level properties accumulate; an unfiltered request stays unfiltered.
Uncaptured, custom-only and finalized records are skipped, as in UE.
Each call compares with its previous observation, initially the undo before-state.
The observation baseline never changes undo storage or creates another history
entry. Whole records retain a full observation and filter reported property names;
native and identity changes remain visible. Observation storage is released
after finalization.

No-op records without annotations, canceled recordings and custom-only commands
produce no serialized object event. A failed record stops further record application. Synchronous notifications already delivered remain observable.
During capture and object-event dispatch, Begin, End, Cancel, capture, snapshot, record changes
and operation closure cannot invalidate the in-flight records. Modify still reports
existing membership and may mark a package dirty; it is not an edit veto.

`FCommandChange` owns synchronous `void Apply(DObject*)` and `void Revert(DObject*)` behavior, descriptive data,
package effects, module ownership, byte accounting, and GC references. Domains
execute changes and call `StoreUndo(Object, Change)` to retain already-applied
undo data. The following rules apply to the same explicitly bound object across
all scopes in one recording. UE 5.8 checks the object's existing records in
`SaveObject` and appends each command in `StoreUndo`.

| First call | Later call | Retained records and replay |
| --- | --- | --- |
| `StoreUndo(Object, Change)` | `Object->Modify()` | The command claims this object. Modify can report true while adding no whole-object baseline. Undo calls Revert and Redo calls Apply. Later writes that the command does not describe have no separate record. |
| `Object->Modify()` | `StoreUndo(Object, Change)` | The whole-object baseline stays first and the command is appended. Undo reverts the command before restoring the object. Redo restores the object before applying the command. Callers must account for the whole object's captured fields when defining a custom change. |

Composite changes may use the `StoreUndo(Change)`
convenience and must declare modified objects through `GetModifiedObjects()`.
The explicit object argument binds the command to its owner. Recording freezes
the declared participants separately from collector-only dependencies.

Open the owning scope before a synchronous domain edit. `StoreUndo` appends to
the active recording, including a nested recording, and rejects idle or foreign
buffers. The scope owner controls finalization. Viewport transform drags and
interactive material parameter edits preserve the authored package’s dirty state during live
preview and open or join a scope when the user commits. This leaves the global
recorder available to other edits between input events.

During admission, command package and participant enumeration exclude reentrant
recording changes. A metadata exception returns a failed submission and leaves
the owner's recording and retained history intact. The caller owns any write
performed before admission.

The editor host admits replay while its editor World is open, stopped and
outside Play. Standalone buffers have no host World. Domain owners define the
participants of structural edits and rebuild derived state on completion.
See [Actor structure commands](../LevelEditing/ViewportEditing.md#actor-structure-commands)
and [component structure history](../LevelEditing/LevelEditorDetails.md#component-structure-history).

### Replay Outcomes

Property replay restores owned historical payloads directly after structural,
type, and exact-identity checks. It bypasses new-edit proposal validation and
normalization. Distinct target objects receive balanced PreEditUndo/PostEditUndo
around the complete replay. Per-property notifications precede completion;
domains retain their derived-state synchronization. See
[reflected property editing](../PropertyEditing/ReflectedPropertyEditing.md#object-notification-contract).

A standalone transaction's `Apply()` returns void, starting with Undo and flipping
direction after each application. `Validate()` checks admission for its next
direction. The buffer selects direction from the cursor, including after it
crosses expired entries. Undo and Redo return false on admission refusal or
exhaustion of expired entries.
Host, state, barrier, expected-head and read-only record validation refusals
leave the rejected entry and its package dirty flags intact. An unresolved whole-object snapshot is skipped for
this operation. A transaction containing only that snapshot still advances
the cursor and reports success. Array records require a valid owner and range.
Custom changes decide expiry through HasExpired. A fully expired entry is
crossed while searching for the next eligible transaction.

Once execution begins, a required callback exception or a failed inverse,
record application, completion, notification or publication is a programming or
execution fault. The native diagnostic includes transaction ID, direction, cause
and the record index when available, then terminates the process. Completion
hooks run only on the normal path. Failure triggers no extra PostEditUndo,
package dirtying, history clearing or branch restoration. Successful replay
publishes the live inverse for the opposite direction, exchanges the recorded
package dirty state and advances the cursor. Undo(false) discards the Redo suffix.

Subscribe before editing. Recording observers use `OnTransactionStateChanged`.
Replay observers bind to `OnUndo` or `OnRedo` and receive the transaction
context and success flag synchronously. On success, the cursor has moved and
the operation ID is live. `UndoRedoStarted` precedes application and
`UndoRedoFinalized` follows the directional delegate. An unavailable head
or a rejected candidate reports false with an empty context. A record-admission
refusal restores the candidate cursor before the directional delegate and emits
no replay state events. Exhaustion of expired entries also reports an empty context. A
reentrant request during notification returns false without a nested broadcast.
Durin carries command feedback in the completion context's `Details` field. Queue observers use `OnUndoBufferChanged`.

## Package Dirty State

The transaction records each affected package's dirty bit and save count on its
first capture. The outermost commit marks affected packages dirty. The primary
context alone does not make its package dirty.

During buffer Undo/Redo, `GIsTransacting` suppresses `DObject::MarkPackageDirty()`
through expiry checks, object callbacks and successful completion notifications.
Outside replay, a transient object or transient Outer cannot dirty its package through this entry point. Ordinary component removal leaves callback edits free to mark other packages dirty.
The buffer updates recorded packages directly.

On successful Undo or Redo, the buffer restores the captured dirty bit if the
save count still matches. Crossing a persistent save keeps the package dirty.
The dirty bit and count from the start of replay become the baseline for the
opposite direction. Checked replay publishes these changes after object replay
succeeds.

The transaction module counts persistent saves and resident package deletions
by package object identity. It subscribes to the package lifecycle events
on first use and drops expired identities after collection on the next lookup.
Persistent saves clear the dirty bit before notifying subscribers.
Editing an already dirty package does not advance the count. Editors do not
register saves or document activation with the transactor.

## Bounds And Accounting

The default soft budget is 64 MiB of owned native history data. This covers
records, payloads, reference tables, metadata and reported custom allocations.
The transitive object graph is excluded. `GetUndoSize()` measures the visible
queue on demand. An empty queue reports zero. Arithmetic overflow or a busy
nonempty queue reports `SIZE_MAX`.

Outermost Begin sets aside the Redo suffix. With no Redo suffix it sets aside
the oldest entries until the retained size is strictly below budget. Zero
budget sets aside every old entry. Only the positive-budget trimming path
measures history. Overflow or a throwing size callback rejects Begin before it
changes the queue or barriers. End and replay do not measure history.

| Transition | Queue visible to synchronous listeners |
| --- | --- |
| Outermost Begin | The new entry is the tail and `GUndo` points to it. `TransactionStarted` precedes `OnUndoBufferChanged`. Removed entries keep their strong references. |
| Outermost End | `PreTransactionFinalized` runs at depth one and admits nested edits. `TransactionFinalized` runs at depth zero with the operation open and checked heads visible. An effective End keeps the entry and retires the entries set aside at Begin. |
| Transient End | An empty, unchanged or PIE entry restores the previous order and cursor. `OnUndoBufferChanged` sees depth zero while `GUndo` still points to the retiring candidate. Both End paths finish with `PostTransactionFinalized` carrying the transaction ID. |
| Cancel | `TransactionCanceled` sees the candidate. Cancel clears `GUndo`, restores the queue and broadcasts `OnUndoBufferChanged` before clearing the recording depth. |

Reset retains the caller's unavailability reason.

## Collector And Shutdown Contract

The reachable buffer enumerates visible transactions and entries set aside at Begin,
primary objects, targets, capture participants, package transitions, tracked
package states, and domain-declared hard dependencies. Persistent references use
exact object generations. Undo and Redo records retain explicitly reported
hard references across GC even after a participant is marked garbage. Its
garbage flag still controls ordinary access. Weak and soft payloads remain non-owning. See [transaction records](TransactionRecords.md).

Cancellation, branch replacement, eviction, Reset, module retirement, destruction,
and editor shutdown erase the corresponding record data and collector edges.
Module history retirement requires Idle, so callbacks cannot erase a record
currently replaying. Retirement discards the entire mixed entry, remaps barriers, and marks its affected asset packages dirty. Package forgetting applies the same conservative rule to surviving
affected packages. A module cannot unload while retained custom executable code
requires it. Clearing history releases the references held by its records.

## Related Documentation

- [Transaction records](TransactionRecords.md)
- [Reflected property editing](../PropertyEditing/ReflectedPropertyEditing.md)
- [Editor function actions](../PropertyEditing/EditorFunctionActions.md)
- [Editor tools](../Tools/EditorTools.md)
