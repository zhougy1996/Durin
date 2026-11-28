# Editor Transactor Core

Summary: Define UE-shaped native recording, synchronous Undo/Redo, package dirty state, history notifications, and collector ownership.

Modules: Core, CoreDObject, Engine, DurinEd

Last reviewed: 2026-11-28

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
A listener that retires the recording returns control to the initiating caller
before anyone begins an independent recording. A listener exception on the
current recording follows the recording failure contract. After listeners return, End decrements the depth
and finalizes at zero.

`Cancel(StartIndex)` cancels the entire recording, including from an inner
scope. Applied edits remain. The start index is a nesting depth. The baseline
for these semantics is [UE 5.8.0, CL 53629095](../../../Plans/Archive/2026-10/UE58TransactionArchitecture.md#reference-identity-and-reproduction).

A scoped recording keeps its Begin index until Cancel or destruction. A disabled
scope leaves the enclosing recording available. After external cancellation or
Reset, callers must unwind all outstanding scopes before beginning an independent
recording. A late scope destructor can otherwise end the new recording.
This applies across property gesture events and Python yields. Canceling a scope
releases its own index immediately. Scopes are noncopyable and nonmovable.
Independent buffer users pair Begin with End or Cancel directly.

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

`DTransactor::GetQueueLength()` reports the committed queue length. `GetTransaction(Index)` reads an entry in that history.
`DTransBuffer::GetUndoCount()` counts retained transactions after the cursor,
including expired entries crossed by Undo. `GetQueueLength() - GetUndoCount()`
counts entries before the cursor. Redo moves entries across the cursor.
Throughout recording, the cursor and Redo suffix retain their committed positions.
An effective outermost End replaces the Redo suffix with the new entry. IDs remain monotonic across
Reset. Ordinary `Undo(bool bCanRedo = true)` and `Redo()` need no expected ID;
`Undo(false)` discards the redo suffix. Delayed notification actions use
`UndoExpected` and `RedoExpected` with the nonzero ID captured from their event.
The action is available only when its ID matches the checked context at the
head. Invocation checks the ID again before advancing history.
`CanUndo`/`CanRedo` can explain unavailability;
Reset retains a caller-supplied reason. GetUndoContext/GetRedoContext return
owned context values for the adjacent queue entry, including an expired entry. GetUndoContext() is empty
when Undo is unavailable. GetUndoContext(false) returns the committed entry before the cursor, including during
recording. The current recording exposes its context through `GUndo`. GetRedoContext() is empty when Redo is unavailable. Workspace label
adapters derive owned strings from these contexts, so labels survive history
replacement/reset. The transactor has no separate description-query interface.

`FCommandChange::HasExpired()` and captured object annotations can
expire an entire entry when any one record reports expiry. The buffer advances past expired
entries without publishing Undo or Redo events and applies the next eligible
entry within the undo barrier. Each candidate crosses the cursor before its
expiry check. During Apply, GetUndoCount and GetUndoContext(false) see that
position; checked contexts and nested replay are unavailable. Undo(false)
drops the Redo suffix after each candidate, including each expired entry.
Guarded replay refuses a mismatched or expired head. A read-only record refusal
leaves the rejected candidate on its original side; already crossed expired
entries stay consumed in that direction.
Expiry is evaluated once per candidate on each operation. Crossing an expired entry
leaves its objects and package checkpoints untouched. An admitted transaction
replays every record and exchanges its package dirty checkpoints after success.

Command details describe the recorded edit in its original direction. Undo and Redo treat these strings as optional presentation data. A
throwing detail provider emits a diagnostic and contributes an empty string;
other records can still supply details. An exception from required replay work with unknown effects follows the execution-failure contract below.

IsActive() reports whether this buffer's recording depth is positive. ActiveCount
and the pending transaction's ownership of GUndo identify the recording.
CanTransact() lets callers resume editor work outside capture and guarded history
operations. Callers own the lifetime of outstanding scopes across suspension.
The pre-finalization listener can still open and close nested edits.

Undo and Redo share the same replay admission check: the buffer must be idle,
GUndo must be clear, and the host must permit replay. The requested direction
selects the history head and completion delegate. Object events use UndoRedo in either direction. Both directions
guard the buffer against reentrant edits and replay. Cleanup and destruction
also block mutation, inspection prevents recursive head queries, and finalization
hides entries while the buffer publishes or retires them.

During `UndoRedoStarted`, the buffer's notification state and `GIsTransacting`
prevent listeners from starting an edit or another replay. The transaction's
own busy flag protects record capture, command insertion, finalization, replay
preparation and application, and inspection callbacks that can access the
transaction directly through `ITransaction`.

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
examines the independent pending transaction and committed entries on both sides of the cursor.
It follows declared strong references from targets, primary context, commands,
annotations, array values and package transitions. Weak and soft references
retain their usual semantics. The buffer query conservatively returns true on a
busy traversal or inspection error.

Outermost End treats an entry
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
capture supplies the terminal annotation.
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
current annotation. Each whole-object record notifies only its target. Finalize
computes its field changes from the initial and final state.
During a multi-frame edit, the property editor delivers interactive updates and its
terminal property notification. Transaction object events begin at finalization.

No-op records without annotations, canceled recordings and custom-only commands
produce no serialized object event. A failed record stops further record application. Synchronous notifications already delivered remain observable.
During capture and object-event dispatch, Begin, End, Cancel, capture, record changes
and operation closure cannot invalidate the in-flight records. Modify still reports
existing membership and may mark a package dirty; it is not an edit veto.

Domains call `StoreUndo(Target, Change)` after applying the edit. The record
retains the explicit target for membership, notification and package tracking,
and passes the resolved target to `Apply`, `Revert` and `HasExpired`.

A command recorded first claims capture for its target.
A later `Modify()` on that participant adds no baseline, so the command must cover
the edits that need replay. Collector-only dependencies remain eligible for capture.
To combine a whole-object baseline with a command, call `Modify()` before the first
write, then append the command. Undo reverts the command before loading the snapshot.
Redo loads the snapshot before applying the command. Commands that also write a
snapshotted field must restore explicit endpoints.

Finalize uses the first recorded entry for each object's event data. Undo and Redo
use the last entry visited in their respective traversal. With a snapshot followed
by one command, Finalize and Undo use the snapshot. Redo selects the command and
runs the completion hook without a field-diff event. Command-only participants
receive one pre- and post-undo hook per replay in either direction.

Open the owning scope before a synchronous domain edit. `StoreUndo` appends to
the active recording, including a nested recording, and rejects idle or foreign
buffers. The scope owner controls finalization. Viewport transform drags open a recording on press and
call `Modify()` on the component before each preview write. The transaction captures its whole-object baseline once. Interactive material parameter edits
open or join a scope when the user commits their preview.

During admission, command package enumeration excludes reentrant
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
leave the rejected entry and its package dirty flags intact. If any whole-object
target is unavailable, preflight rejects the entry before object hooks or writes.
Restoring access to the recorded target allows retry with the same transaction ID.
A recorded deletion grants replay access to the retained generation during preflight.

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

Outermost Begin preflights the soft budget when the cursor is at the end of history.
It counts the oldest entries to evict so the retained suffix of undoable history fits strictly below the budget. A zero budget schedules all old entries
for eviction. A Redo suffix takes precedence over budget eviction. Only the
positive-budget preflight measures bytes. Overflow or a throwing size callback
rejects Begin before recording starts. End and replay do not measure history.

| Transition | Queue visible to synchronous listeners |
| --- | --- |
| Outermost Begin | The buffer owns the pending recording separately. `TransactionStarted` receives its context. Committed entries, cursor, barriers and byte count stay visible. Replay is disabled. |
| Effective outermost End | `PreTransactionFinalized` admits nested edits at depth one. `TransactionFinalized` sees the pending context before publication. Publication replaces the Redo suffix or evicts the preflighted prefix, remaps barriers, appends the entry and advances the cursor. One `OnUndoBufferChanged` follows publication, then `PostTransactionFinalized` carries the committed ID. |
| Transient End | Empty, unchanged and PIE recordings are discarded. The committed queue stays intact. `PostTransactionFinalized` carries the discarded ID. |
| Cancel | `TransactionCanceled` receives the pending context. The buffer closes the recording and releases its records. |

Reset retains the caller's unavailability reason.

## Collector And Shutdown Contract

The reachable buffer enumerates committed transactions and the pending recording, including their
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
