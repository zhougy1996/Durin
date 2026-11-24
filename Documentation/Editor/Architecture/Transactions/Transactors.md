# Editor Transactor Core

Summary: Define UE-shaped native recording, synchronous Undo/Redo, package dirty state, history notifications, and collector ownership.

Modules: Core, CoreDObject, Engine, DurinEd

Last reviewed: 2026-11-24

A reflected editor action rechecks recording after marking its target dirty and before invoking native code.

## Service Ownership

The process has one active `DEditorEngine`, published during activation rather
than object construction. It owns one transient `DTransBuffer` in `Trans`.
Windows, documents, tools and script sessions share this ordered history.
`DEngine` exposes BeginTransaction, EndTransaction, CancelTransaction and
CanTransact; the editor implements them through its transactor.

DTransactor requires all recording, history and package-recording capabilities
through pure virtual functions. Concrete implementations cannot inherit silent
no-op or unsupported behavior. The base owns listener registration and history identity. Callers open a transaction before editing and use StoreUndo for an already-applied custom change.

Core defines the [runtime transaction contracts](../../../Runtime/ObjectSystem/Transactions.md),
including custom changes, context, `ITransaction`, and ordinary
GameThread pointer `GUndo`. Outermost Begin publishes the pending `FTransaction`;
End, Cancel, owner retirement and destruction clear it before releasing storage.
A competing buffer cannot begin while another recording is published. There is
no candidate registry or host recorder binding stack. Object capture logs serialization errors. CoreDObject has no editor dependency.
FTransaction collaborates privately with the buffer on object capture. Ordinary callers use Modify(),
scopes and transaction results; DTransactor exposes no object-capture method or
recording-identity query. ITransaction::SaveObject returns void. A failed whole-object snapshot invalidates the recording across nested scopes. The outer End, Cancel or Reset diagnoses the original capture error and terminates the process. ContainsObject includes additional command and array participants whose whole-object baseline may still fail to serialize.

Host callback scopes guard execution and teardown, not transaction routing.
Callbacks request GC; the host collects after the complete operation returns.
Ending a callback never ends recording. Without a current recording, Modify
keeps its normal dirty behavior and returns false. Undo/Redo reject an active
recording and do not expose replay as a new recording.

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
and finalizes at zero. History listeners receive queued events after publication.

The scope, capture ownership and filtering reference is the installed Epic
UE 5.8.0 source, CL 53629095, branch `++UE5+Release-5.8`. Relevant UE files are
`Engine/Source/Editor/UnrealEd/Private/EditorTransaction.cpp` (`SaveObject`,
`StoreUndo`, `UTransBuffer::End/Cancel`), its `Classes/Editor/TransBuffer.h`
(`BeginInternal`), `Private/ScopedTransaction.cpp`,
`Engine/Source/Runtime/CoreUObject/Private/UObject/Obj.cpp` (`Modify`) and
`Property.cpp` (`ShouldSerializeValue`), and
`Engine/Source/Runtime/Engine/Private/Actor.cpp` (`Modify`). The
[reference identity and behavior matrix](../../../Plans/Archive/2026-10/UE58TransactionArchitecture.md#reference-identity-and-reproduction)
records source fingerprints and explicitly separates implemented rules from
future architecture work. This source version's `Cancel(StartIndex)` cancels the entire active recording,
including cancellation by an inner scope. It does not restore applied edits.
The index is an activity count, never a history ID or an isolated savepoint.

`FScopedTransaction` provides UE-shaped description and full-context constructors
using the active editor. A disabled scope or unavailable editor remains inert.
Scope construction, destruction and cancellation use the active editor.
Passing false skips Begin for that scope. With an outer recording, a subsequent
`Modify()` or object-bound `StoreUndo` still participates in the outer entry.
Canceling or destroying the disabled scope leaves the outer recording active.
Independent buffer users pair Begin with End or Cancel directly.
After an enabled inner scope cancels, the outer scope can still report
`IsOutstanding()` while `IsRecordingCurrent()` is false. Subsequent writes
require a new recording to enter history. Durin checks the recording identity
when the old scope unwinds so it cannot close a replacement transaction.
Ordinary edit results describe the applied model change.
Recording finalization runs synchronously, including from scoped destructors. An execution fault produces a terminal native diagnostic.
`Reset(reason)` clears history. Map replacement, package unload and PIE check `CanTransact()` before proceeding and report admission errors at their own entry points. PIE clears history before copying changes into the editor world. Package callbacks can request a stop during copy-back. Teardown waits for copy-back to exit, including on failure, and nested copy-back is rejected. ContainsPieObjects returns bool and GetUndoSize
reports owned history bytes. A failed or busy PIE inspection conservatively
returns true. History events carry their own presentation details. Runtime
`ITransaction` capture/inspection remains checked so recording and replay never
mistake a failed traversal for an empty reference set.
A scope stores its start index, recording ID and weak owner. It is noncopyable and
nonmovable. `IsOutstanding()` reports successful acquisition until explicit
cancellation. `IsRecordingCurrent()` checks whether the original recording is
still active on its editor. Destruction and cancellation only act on that
recording, including after external End, Cancel, Reset or host replacement. Before releasing the editor, shutdown cancels property previews and releases suspended Python scopes. Ordinary native callers must unwind their lexical scopes before host replacement.

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
owned context values for the next replayable entry. GetUndoContext() is empty
when Undo is unavailable. GetUndoContext(false) returns the active queue tail while
recording. GetRedoContext() is empty when Redo is unavailable. Workspace label
adapters derive owned strings from these contexts, so labels survive history
replacement/reset. The transactor has no separate description-query interface.

`FCommandChange::HasExpired(DObject*)` and captured object annotations can
declare a record ineligible for replay. The buffer advances past wholly expired history
entries without publishing Undo or Redo events and applies the next eligible
entry within the undo barrier. Guarded replay checks the eligible entry's ID.
The transaction selects the records once per replay. Object events use that
selection and its completion annotations. The buffer consumes the transaction's
package outcomes and event details after success. Skipped records retire in
both directions. A package touched only by skipped records keeps its
dirty state. A package touched by both kinds is marked dirty.

Undo and Redo treat command detail strings as optional presentation data. A
throwing detail provider emits a diagnostic and contributes an empty string;
other records can still supply details. An exception from required replay work with unknown effects follows the execution-failure contract below.

IsActive() reports whether this buffer's recording depth is positive. ActiveCount
and the pending transaction's ownership of GUndo identify the recording.
CanTransact() permits new or nested edits outside capture and guarded history
operations. The pre-finalization listener can still open and close nested edits.

Undo and Redo share the same replay admission check: the buffer must be idle,
GUndo must be clear, and the host must permit replay. The requested direction
selects the history head and operation reported to listeners. Both directions
guard the buffer against reentrant edits and replay. Cleanup and destruction
also block mutation, inspection prevents recursive head queries, and finalization
hides entries while the buffer publishes or retires them.

Reset cancels the complete active recording and purges history, barriers, queued
events and package tracking without reverting applied edits or clearing package
dirty flags. It snapshots the reason before invoking record destructors and
blocks reentrant recording, reset and inspection throughout cleanup. Unwind existing scopes before starting the next recording. Even an empty reset invalidates prepared history publication.
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
Barrier and budget changes invalidate a prepared replacement operation.
Barrier refusal has its own rejection reason, separate from an unexpected head
ID. CanTransact, CanUndo and CanRedo all reject another buffer's live GUndo;
unavailable history queries return a nonempty explanation.

DPackage::IsPlayInEditor is an explicit session flag, independent of world type,
asset persistence and object Transient flags. Editor Play creates a dedicated
transient package for its copied world graph and retires that package after the
render fence. Ordinary runtime worlds do not implicitly become PIE packages.
Initialize asset packages before applying the session flag; the flag is not saved.

FTransaction::ContainsPieObjects inspects targets and both serialized sides.
Durin deliberately extends UE's record/reference-table check to every declared
strong history edge: primary context, custom participants and dependencies,
annotations, array values and package transitions. It does not recursively scan
the live DObject graph or promote weak/soft edges. Queries inspect captured history
sides; uncaptured live edits enter inspection at finalization. A buffer query
includes pending,
Undo and Redo entries. Checked inspection reports callback exceptions as errors;
the ordinary buffer bool query reports possible PIE references on failure.
Public queries guard both the buffer and each inspected transaction
against mutation and recursive inspection. Pending, Undo and Redo references are
inspected in stable states; a busy traversal returns a checked error (true through
the bool facade). The previous state is restored on every
exit. Query and history callbacks must request GC instead of executing it.
Finalization reuses the guarded transaction query and keeps its own
non-reentrant phase through publication or discard. GC requests
remain pending until the owner reaches a safe point.
Raw entry and checked head queries are available to the synchronous lifecycle
notifications. They are unavailable during internal publication and from record
destructors. Replay checks remain inside their replay phase guards.
Ordinary Cancel and transient End restore the entries set aside at Begin.
An affirmative PIE inspection during End selects the transient path. A failed
inspection, capture, required callback or dirty notification
during finalization reports the transaction ID, phase and original cause and
terminates the process. The same boundary applies to exceptions from synchronous
lifecycle listeners, including PostTransactionFinalized after the entry settles.
Typed array capture
rejects PIE owners before the adapter mutates the array.

Checked replay also inspects existing data and newly prepared inverses before
publishing them. Whole-object inverses reject PIE before loading; ordered range
inverses are inspected at their actual replay step. A PIE reference already
present in history rejects admission. A fault during inverse capture or ordered application is terminal. After the final record check, PostEditUndo and object observers can change
references. The completed operation retains its history and reports success.
A later replay checks the selected entry again at admission.

### Synchronous Object Events

`PostTransacted` receives Finalized before the recording operation closes and
`GUndo` clears. UndoRedo follows all object loads and native PostEditUndo
completion. Components complete before their owning Actor. Events carry the
transaction ID, live operation ID, actual changed fields, original identity and
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
Queued history listeners retain their separate idle delivery contract.

`FCommandChange` owns synchronous `Apply()`/`Revert()` behavior, descriptive data,
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
interactive material parameter edits suppress package dirtying during live
preview and open or join a scope when the user commits. This leaves the global
recorder available to other edits between input events.

During admission, command package and participant enumeration exclude reentrant
recording changes. A metadata exception returns a failed submission and leaves
the owner's recording and retained history intact. The caller owns any write
performed before admission.

Ordinary editor writes call `Modify` on the target before mutation. An explicitly
object-bound command suppresses subsequent whole-object capture for that owner.
A command that needs both forms of replay captures the owner before binding its
change. Commands that report affected objects without binding an owner leave
those participants eligible for later ordinary edits, including single-participant
commands. This permits sparse domain changes and staged graph edits to compose
with ordinary changes without forcing an extra owner serialization. Material
layout, parameter defaults and configuration record their authored owners
through `Modify`.
Transactional CallInEditor targets and ordinary Actor/root edits use virtual Modify.
NonTransactional CallInEditor targets can still execute without gaining history.

The remaining custom commands have domain responsibilities beyond a field load:

| Command family | Modified participants and retained responsibility |
| --- | --- |
| Transform gizmo | Declared target objects; arbitrary `ITransformGizmoTarget` also permits native-only targets and fallible transform setters, so the adapter retains gesture replay. |

Each custom change separately enumerates collector dependencies and owned bytes.
Material Details uses shared reflected edits for constant values. Outliner
visibility and primary-camera edits already use scoped `Modify`; Python scopes
use the same application recording and job cleanup, and script label setters
route through the Actor subsystem. No alternate history is introduced for these
consumers.

Actor creation, deletion and duplication capture the Level, each edited Actor and
the scene components whose attachments cross the selection boundary. Prepared
Actors append to the Level and register before the batch restores cross-Actor
attachments. Candidates retired during preparation acquire no history record.
Duplication records admitted Actors and the final attachments between Actor roots
in the same transaction and returns the surviving copies in selection order. Runtime spawn and editor placement
share publication completion, including component activation and notification. Removal checks the recording before each boundary
detach. If a callback cancels it, subsequent links and Level members stay in place.

The Level snapshot owns Actor order and the primary camera. Level completion
preserves independently published Actors. The editor host admits history replay
only outside play and while its editor World is stopped and open. Its buffer
applies that rule to queries, Undo and Redo, including direct callers. Standalone
buffers have no host World. Object renames and Outer changes reject occupied
sibling names. Replay still validates historical identity changes and the combined
destination attachment graph before writing. Actor annotations restore tick and
component registration. Viewport placement uses the same subsystem. Spawn
configuration runs once before publication and Redo restores the recorded object.
Presence captures the Actor through the base object implementation of Modify. Its
native root needs a separate snapshot only when an attachment changes. This lets
SkyBox Redo use the surviving component with an empty texture after asset retirement.

Component creation, deletion and duplication capture the owning Actor and the
instance component. Deletion also captures detached children. An instance has
its creation method before its first snapshot. Actor completion reconciles the
serialized instance list with native ownership and lifecycle. All participants
join the caller's scope, including ordinary edits from lifecycle callbacks.
Cancel discards the recording and leaves completed model writes in place.

In UE 5.8, LevelActor.cpp captures the Level before inserting a spawned Actor.
UWorld::RemoveActor in World.cpp can call ModifyLevel before removal.
ULevel::Serialize includes its Actors array. ComponentEditorUtils.cpp calls
Modify on the target Actor when adding or duplicating an instance component
and on the component during deletion. FTransaction::SaveObject in
EditorTransaction.cpp creates the per-object record on first capture. Durin retains absent object identities for earlier property
history and uses transaction annotations to reconstruct native lifecycle state.

Batch attachment and detachment preflight the selection, then capture and edit each
eligible root in one scope. Later rejections leave earlier edits available for one
Undo. The return value reports whether any root changed.

Actor and Material commands retain their domain-specific errors.

Property replay restores owned historical payloads directly after structural,
type, and exact-identity checks. It bypasses new-edit proposal validation and
normalization. Distinct target objects receive balanced PreEditUndo/PostEditUndo
around the complete replay. Per-property notifications precede completion;
domains retain their derived-state synchronization. See
[reflected property editing](../PropertyEditing/ReflectedPropertyEditing.md#object-notification-contract).

Undo and Redo return false on admission refusal or exhaustion of expired entries.
Host, state, barrier, expected-head and read-only record validation refusals
preserve history and package dirty flags. Custom commands can implement
ValidateReplay to check requirements before any PreEditUndo callback. The first
selected record can check its live source immediately. Later records may depend
on the intermediate state produced by earlier records.

Once execution begins, a required callback exception or a failed inverse,
record application, completion, notification or publication is a programming or
execution fault. The native diagnostic includes transaction ID, direction, cause
and the record index when available, then terminates the process. Completion
hooks run only on the normal path. Failure triggers no extra PostEditUndo,
package dirtying, history clearing or branch restoration. Successful replay
publishes the live inverse for the opposite direction, exchanges the recorded
package dirty state and advances the cursor. Undo(false) discards the Redo suffix.

History events are queued during transitions and delivered by DispatchEvents
to independent listeners after the transactor reaches Idle.
UI notifications register as a listener; observers receive the same immutable
transition data without stealing events. The queue is not a persistent journal.

## Package Dirty State

The transaction records each affected package's dirty bit and save count on its
first capture. The outermost commit marks affected packages dirty. The primary
context alone does not make its package dirty.

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

The production soft budget is 64 MiB of owned native transaction data, without
an entry cap. SetMemoryBudget changes the limit while idle. The next outer Begin
applies the limit. A zero budget selects the entire old prefix when there is no
Redo branch. Accounting includes context, record storage,
payloads/reference tables, capture metadata, and reported custom allocations;
it excludes the transitive retained DObject graph. GetUndoSize measures the visible
queue on demand and returns SIZE_MAX on arithmetic overflow or when a nonempty
queue is busy with an internal traversal. With a positive budget and no Redo
suffix, Begin measures the old entries before changing the queue. Overflow or a
throwing size callback rejects that Begin and preserves the heads and barriers.
A Redo suffix or a zero budget determines the entries to set aside without
measuring them. End, Undo and Redo can complete with an unrepresentable size.

The outermost Begin temporarily removes the entire Redo suffix, or, when there is
no Redo branch, the oldest entries until the retained total is below budget.
It appends the new transaction, sets GUndo to the queue tail, and broadcasts
TransactionStarted on OnTransactionStateChanged, followed by OnUndoBufferChanged. Nested scopes share that entry.
GetQueueLength, GetTransaction and GetUndoSize describe the visible queue. During
recording GetUndoCount is zero, GetUndoContext(false) identifies the candidate,
and checked Undo/Redo heads are unavailable. Temporarily removed entries keep
their strong references.

Outermost End broadcasts PreTransactionFinalized at depth one, then captures final
values and broadcasts TransactionFinalized at depth zero with the operation still
open. Listeners can inspect the queue and its checked heads during this callback.
Replay and new recordings remain guarded until finalization completes. An effective End keeps the tail and retires the removed entries. An empty,
unchanged or PIE transaction restores the original entries, order and cursor and
broadcasts OnUndoBufferChanged. Both paths finish with PostTransactionFinalized
carrying only the transaction ID. Cancel broadcasts TransactionCanceled while the
candidate is visible. It clears GUndo, restores the queue, broadcasts
OnUndoBufferChanged, and then clears the recording depth. For a transient End,
the restored-queue notification sees depth zero and GUndo still refers to the
retired candidate until the callback returns.
Executed, Discarded and Evicted records remain queued for idle dispatch. Undo/Redo
do not trim the budget. Reset clears queued history events and retains its explicit unavailability reason; it emits no synthetic
record event.

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
currently replaying. Retirement discards the entire mixed entry, publishes a
Discarded event, remaps barriers, and marks its affected asset packages dirty. Package forgetting applies the same conservative rule to surviving
affected packages. A module cannot unload while retained custom executable code
requires it. Clearing history releases the references held by its records.

## Related Documentation

- [Transaction records](TransactionRecords.md)
- [Reflected property editing](../PropertyEditing/ReflectedPropertyEditing.md)
- [Editor function actions](../PropertyEditing/EditorFunctionActions.md)
- [Editor tools](../Tools/EditorTools.md)
