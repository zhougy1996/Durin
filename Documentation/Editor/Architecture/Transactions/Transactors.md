# Editor Transactor Core

Summary: Define UE-shaped native recording, synchronous Undo/Redo, package checkpoints, history notifications, and collector ownership.

Modules: Core, CoreDObject, Engine, DurinEd

Last reviewed: 2026-10-25

## Service Ownership

The process has one active `DEditorEngine`, published during activation rather
than object construction. It owns one transient `DTransBuffer` in `Trans`.
Windows, documents, tools and script sessions share this ordered history.
`DEngine` exposes BeginTransaction, EndTransaction, CancelTransaction and
CanTransact; the editor implements them through its transactor.

DTransactor requires all recording, history and package-revision capabilities
through pure virtual functions. Concrete implementations cannot inherit silent
no-op or unsupported behavior. The base retains listener registration, result
storage and convenience forwarding. ApplyEditorChange is declared only in
Transactions/Transactor.h; consumers include that header directly.

Core defines the [runtime transaction contracts](../../../Runtime/ObjectSystem/Transactions.md),
including custom changes, context, `ITransaction`, and ordinary
GameThread pointer `GUndo`. Outermost Begin publishes the pending `FTransaction`;
End, Cancel, owner retirement and destruction clear it before releasing storage.
A competing buffer cannot begin while another recording is published. There is
no candidate registry or host recorder binding stack. Object capture retains
Durin's typed failures through the owning buffer; CoreDObject has no editor dependency.
Buffer object capture and exact recording identity are private collaboration
hooks for FTransaction and FScopedTransaction. Ordinary callers use Modify(),
scopes and transaction results; DTransactor exposes no object-capture method or
recording-identity query. ITransaction::SaveObject remains the runtime boundary.

Host callback scopes protect execution and GC lifetime, not transaction routing.
Ending a callback never ends recording. Without a current recording, Modify
keeps its normal dirty behavior and returns false. Undo/Redo reject an active
recording and do not expose replay as a new recording.

## Transactions And Scoped Recording

ResolveEditorTransactor is the editor command admission boundary. Record requires
the active editor to accept recording. NotTransactable returns no service only
when no GUndo is published, preventing unrecorded commands from joining another
recording. This policy does not suspend GUndo or change low-level Modify behavior.
Property UI and ordinary material commands carry this mode instead of selecting
a service; exact owners remain inside long-lived edit sessions.

Recording and replay run synchronously on GameThread. `Begin(Context,
Description)` returns the previous active-action count: zero for the outermost
scope, one for its first nested scope. Unavailable Begin returns -1. `End()`
returns the count before closing one action, or zero without an active recording.
Only outermost End captures final object values and commits one history entry.
Nested contexts do not replace the outer context. `SetPrimaryUndoObject` sets the
first primary object for presentation and retention; it is not an affected package.

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
Simple panel actions such as actor visibility and primary-camera selection use
these constructors rather than passing the process editor's transactor through
panel contexts. Low-level property composition, material gestures and suspended
script scopes retain explicit transactors where exact recording ownership is part of their contract.
The explicit-transactor overload supports native composition and independent
low-level buffer tests; it cannot open a competing concurrent recording.
Cancel(), IsOutstanding() and optional End are retained, and destruction ends
an outstanding action.
The editor management surface follows UE's ordinary call shapes: `Reset(Reason)`
returns void, `ContainsPieObjects()` returns bool, and `GetUndoSize()` reports
owned history bytes through `DTransactor`. `TryReset` and
`TryContainsPieObjects` retain Durin's checked diagnostics when a caller needs
them. A failed bool PIE inspection conservatively returns true. Runtime
`ITransaction` capture/inspection remains checked so recording and replay never
mistake a failed traversal for an empty reference set.
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
set or clear it. Templates are excluded from ordinary editor property/script
editing; native default-graph transactions have explicit
[runtime admission](../../../Runtime/ObjectSystem/Transactions.md#coredobject-boundary).

`FTransaction` owns first-before capture and deduplicates objects across every
nested action. Final-after capture happens at outermost End, so 0, outer write 1,
inner write 2, outer write 3 replays as Undo 0 / Redo 3. Property editing and
explicit whole-object recording use that same first-before baseline. AActor overrides Modify to include its native root
component, matching the pinned Actor.cpp behavior; generated roots are rebuilt
by construction. Direct references to Generated objects are excluded from
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
replay owner; there is no focused undo or promotion path. [Transaction records](TransactionRecords.md) defines flip and recovery
behavior.

Successful capture marks the package dirty. With `bAlwaysMarkDirty`, an
uncaptured call also marks it dirty, including calls outside a recording.
An unchanged Modify can therefore leave a dirty package without a history entry.
No-op pruning and cancellation do not promise to clear dirtiness.

## Structural History And Barriers

`DTransactor::GetQueueLength()` reports the retained history size, including
both Undo and Redo entries. `GetTransaction(Index)` reads an entry in that history.
`DTransBuffer::GetUndoCount()` follows UE's meaning: the number of transactions
already undone and available for Redo. The number of entries available for Undo
is `GetQueueLength() - GetUndoCount()`, with no separate cursor-count API. For example,
after three commits and two Undo operations, the counts are two undone and one
undoable. Redo decreases the undone count; committing a new branch discards the
remaining Redo entries and resets that count to zero.

History entries before the cursor are undoable; entries after it are redoable.
A successful new commit replaces the redo suffix. IDs remain monotonic across
Reset. Ordinary `Undo(bool bCanRedo = true)` and `Redo()` need no expected ID;
`Undo(false)` discards the redo suffix. `UndoExpected` and `RedoExpected` are
optional guarded extensions. `CanUndo`/`CanRedo` can explain unavailability;
Reset retains a caller-supplied reason. GetUndoContext/GetRedoContext are the
canonical virtual history presentation queries. The buffer reads the cursor
directly and returns an owned context value. GetUndoContext() is empty when
Undo is unavailable; GetUndoContext(false) reads the previous entry even while
recording. GetRedoContext() is empty when Redo is unavailable. Workspace label
adapters derive owned strings from these contexts, so labels survive history
replacement/reset. The transactor has no separate description-query interface.

Idle and Recording accept ordinary composition through `CanTransact()` outside
object capture and synchronous transaction callbacks.
Undoing, Redoing, RecoveryRequired, Destroying, Resetting, Inspecting and
Finalizing reject it. Undo/Redo cannot reenter recording. Ordinary scopes provide
neither caller isolation nor an
all-or-nothing execution contract.

Reset cancels the complete active recording and purges history, barriers, queued
events and package tracking without reverting applied edits or clearing package
dirty flags. It snapshots the reason before invoking record destructors and
blocks reentrant recording, reset and inspection throughout cleanup. Package
revision queries hide retiring checkpoint data until cleanup completes. Existing
scopes become inert. Even an empty reset invalidates prepared history publication.
Refusal during replay or recovery preserves the retained graph; it is not a
recovery operation.

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
Public queries defer GC and guard both the buffer and each inspected transaction
against mutation and recursive inspection. Pending, Undo and Redo references are
inspected in stable states; a busy traversal returns a checked error (true through
the bool facade). The previous recording/recovery state is restored on every
exit. Finalization reuses the guarded transaction query and keeps its own
non-reentrant, GC-deferred phase through accounting, publication or discard.
Entry and head queries are unavailable during that phase, including from record
destructors. Replay checks remain inside their replay phase guards.
End prepares command metadata, event payloads, validated byte totals and vector
capacity before replacing Redo or evicting history. Publication reuses those
values without a second metadata traversal. A callback exception before that
boundary discards only the candidate, retains the old branch and original
diagnostic, and exits to Idle. Unknown effects invalidate checkpoints
conservatively. Publication or cleanup faults require recovery and retain the
remaining graph; an exit guard prevents a stranded Finalizing/global recorder.
Failure cleanup uses captured transitions and plain event data rather than
calling failed metadata again. PIE inspection errors retain their checked cause.
Finalization discards PIE entries after synchronous object events, preserves the
existing Redo branch and budget, and leaves already-applied edits intact. An
inspection error fails storage without replacing history. Typed array capture
rejects PIE owners before the adapter mutates the array.

Checked replay also inspects existing data and newly prepared inverses before
publishing them. Whole-object inverses reject PIE before loading; ordered range
inverses are inspected at their actual step and compensated on rejection. The
current record is inspected per step, with complete checks at phase boundaries,
so traversal stays linear in retained reference slots rather than quadratic in
record count. After clean compensation, transient inverses are cleared and
retained references are checked again after balanced native completion. Taint
from that completion or the final synchronous UndoRedo observers requires
recovery; it does not publish a successful cursor transition.
This is Durin's explicit safety extension beyond UE's recording-end PIE filter.

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
produce no serialized object event. Failed replay and its compensation produce
no successful UndoRedo event. A thrown observer or failure after native completion
requires recovery, retains the history graph and leaves the history cursor in
place; already delivered synchronous observations cannot be retracted.
During capture and callbacks, Begin, End, Cancel, capture, snapshot, record changes
and operation closure cannot invalidate the in-flight records. Modify still reports
existing membership and may mark a package dirty; it is not an edit veto.
Queued history listeners retain their separate idle delivery contract.

`FCommandChange` owns synchronous `Apply()`/`Revert()` behavior, descriptive data,
package effects, module ownership, byte accounting, and GC references. Domains
execute changes and call `StoreUndo(Object, Change)` to retain already-applied
undo data. A prior custom record for that object suppresses a later Modify
snapshot, matching the pinned UE per-object rule. A custom record added after
a snapshot remains ordered after it; callers must choose the representation
that describes their operation. Composite changes may use the `StoreUndo(Change)`
convenience and must declare modified objects through `GetModifiedObjects()`.
One declared object automatically binds a single-object command; multiple
participants remain a composite record. Participant references are frozen
separately from collector-only dependencies.
Durin deliberately retains standalone StoreUndo as an editor-owned extension for
already-applied domain writes. Without an active recording it opens a CustomChange
scope and finalizes one entry. It cannot prove ownership of preceding package
edit revisions: those remain outside writes and can invalidate the checkpoint.
Use a scope before mutation to preserve an undoable clean baseline. Standalone
submission does not execute the change or roll back a failed history submission;
callers inspect the result. Within a live scope it joins the outer recording.
StoreUndo never invokes Apply and accepts nested recording without an opt-in flag.

Editor business entrypoints use `RecordObjectForEdit` when an ordinary serialized
edit follows a domain command for the same participant. This Durin admission
helper requires the owning live scope, game thread and an idle capture boundary;
it rejects foreign services and transaction callback reentry. It reuses an
existing canonical object record without another Serialize, or records one
whole-object capture before mutation. Raw record insertion remains private.
Core `Modify` retains UE's custom-object ownership rule; the explicit business
gate must only accompany domain adapters that compose with serialized fields.
Property sessions and transactional CallInEditor targets share this gate, then
call `Modify` for native Actor/root behavior. NonTransactional CallInEditor
targets still execute without gaining history, as with UE Modify. Actor Label,
Actor/Component Name and existing-skybox texture
assignment have no custom history class or executable editor-module dependency.

The remaining custom commands have domain responsibilities beyond a field load:

| Command family | Modified participants and retained responsibility |
| --- | --- |
| Actor presence | Level, affected Actors/components and attachment endpoints; exact identity/Name reservation, native Level publication, reversible absence and activation. The Level's shared object record owns serialized Actor order and primary camera; camera snapshots inside the adapter serve initial-execution compensation only. |
| Component presence | Owner, component, parent and detached children; native live membership, root role, registration, ordered publication and retention of the complete absent Outer subtree. Serialized instance membership may precede this adapter; Actor completion reconciles and validates the final set. |
| Actor attachment / component reparent | Moved roots/components and old/new endpoints; graph admission, transform rules, native attachment publication and compensation. An already-restored destination edge is not published twice. |
| Transform gizmo | Declared target objects; arbitrary `ITransformGizmoTarget` also permits native-only targets and fallible transform setters, so the adapter retains gesture replay. |
| StaticMesh placement / new SkyBox | Level and declared Actor/component participants; prepared asset bindings, creation/identity, native publication and coordinated rollback. Existing SkyBox texture edits use an ordinary component record. |
| Material semantic graph | Owner, changed expressions and added/removed nodes; connection/type adaptation, synchronized parameter references, membership and one validated publication/rollback boundary. Local member snapshots are domain compensation, not the property UI's history backend. |
| Material presentation | Material owner; GUID-keyed sparse position/label changes preserve unrelated presentation entries. |
| Material parameter gesture | Material owner; a stable parameter ID updates synchronized expression-backed values through the owner's `SetParameterValue` publication boundary, rather than one reflected owner scalar. Texture values are collector dependencies. |

Each custom change separately enumerates collector dependencies and owned bytes.
Ordinary material Details edits use the shared property session. Outliner
visibility and primary-camera edits already use scoped `Modify`; Python scopes
use the same application recording and job cleanup, and script label setters
route through the Actor subsystem. No alternate history is introduced for these
consumers.

Actor presence captures its Level through `GUndo->SaveObject` before initial
publication. Level's balanced native completion hooks remember pre-replay
membership temporarily, so an array loaded earlier in the same operation can
be activated without duplicate insertion. A membership change made before
PreEditUndo remains a conflict. Scene components use their existing pre-undo
parent to make the same distinction for attachment edges. These checks supplement
UE's serialized-state flip with Durin's reversible native absence boundaries.

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
[reflected property editing](../PropertyEditing/ReflectedPropertyEditing.md#object-notification-contract).

When a replay record fails, completed records are compensated in reverse order.
Historical flips are published only after all records and native completion
succeed; clean compensation uses the prepared live inverses and leaves history
unchanged for retry. Only successful replay advances the cursor. Incomplete compensation or a custom
exception retains the participant graph and requires recovery. Reset does not
repair data or unlock catastrophic recovery. Diagnostic results retain actionable
failure and rollback messages without requiring ordinary clients to inspect causes.

History events are queued during transitions and delivered by DispatchEvents
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
Durin publishes revision/dirty transitions after checked native completion, so
failed replay can preserve its prior revision instead of borrowing UE's eager
dirty-bit update before loads. Completion callbacks do not establish checkpoints.

Package edit revisions detect modifications outside retained history before the
next recording/replay/save transition. Unchanged or canceled Modify and unrecorded
writes cannot later clear dirty state merely because Undo reaches an old saved
revision. Callers publishing persistent changes outside history should continue
to invalidate the saved checkpoint explicitly. Checkpoint metadata is session-only.

MarkSaved and EstablishSavedState establish clean checkpoints only while Idle
with no global recording. Notifications during recording, replay or recovery
invalidate the checkpoint and keep the package dirty. A save inside an operation
may represent an intermediate state, so neither retained endpoint is inferred to
match disk. Save again at an idle boundary to establish a clean checkpoint.
This conservative rule serves the same safety purpose as UE's package dirty
fence without borrowing its dirty-bit replay model. Rejected standalone recording
does not acknowledge external package revisions or alter the saved baseline.

Saved package reload prepares only while Idle and retires every whole history
entry whose collector-visible data retains a replaced graph. Unrelated entries
remain; surviving modified packages lose invalidated checkpoints. Precommit reload
failure leaves history and package states unchanged. Successful activation
establishes the replacement graph's saved checkpoint.

Custom mounted-content changes advance the discovery revision after successful
commit/Undo/Redo. Ordinary in-memory property edits do not request registry scans.

## Bounds And Accounting

The production soft budget is 64 MiB of owned native transaction data, without
an entry cap. SetMemoryBudget changes the limit while Idle; it takes effect at the next successful commit. Zero evicts the old prefix on the next successful commit without a Redo branch. Accounting includes context, record storage,
payloads/reference tables, capture metadata, and reported custom allocations;
it excludes the transitive retained DObject graph. Aggregation checks overflow.

Before appending a successful new entry, trim oldest existing history while the
old history occupies at least the budget. The incoming entry remains undoable
even when oversized. Redo replacement skips additional prefix trimming. Canceled,
empty, and failed submissions preserve the existing history. Undo/Redo do not trim.
Compared with UE's Begin-time trimming, Durin delays destructive trimming until
successful finalization so no removed-transaction restoration machinery is needed.
Pending entries are absent from GetQueueLength and owned-byte queries; Redo data
remains visible until a successful commit. Executed, Discarded and Evicted buffer
events are queued for idle dispatch, with eviction preceding the incoming Executed
event. This differs from UE Begin-time buffer-change broadcasts. Object Finalized
events still run synchronously before GUndo clears. Reset clears queued history
events and retains its explicit unavailability reason; it emits no synthetic
record event.

## Collector And Shutdown Contract

The reachable buffer enumerates pending, undoable, and redoable transaction data,
primary objects, targets, capture participants, package transitions, tracked
package states, and domain-declared hard dependencies. Persistent references use
exact object generations; weak/soft payloads do not gain retention. Marked-garbage
objects are not rescued. See [transaction records](TransactionRecords.md).

Cancellation, branch replacement, eviction, Reset, module retirement, destruction,
and editor shutdown erase the corresponding record data and collector edges.
Module history retirement requires Idle, so callbacks cannot erase a record
currently replaying. Retirement discards the entire mixed entry, publishes a
Discarded event, remaps barriers, and invalidates saved checkpoints for its affected
asset packages. Package forgetting applies the same conservative rule to surviving
affected packages. A module cannot unload while retained custom executable code
requires it; recovery participants remain until their buffer is retired.

## Related Documentation

- [Transaction records](TransactionRecords.md)
- [Reflected property editing](../PropertyEditing/ReflectedPropertyEditing.md)
- [Editor function actions](../PropertyEditing/EditorFunctionActions.md)
- [Editor tools](../Tools/EditorTools.md)
