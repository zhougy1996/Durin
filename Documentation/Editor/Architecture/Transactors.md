# Editor Transactor Core

Summary: Define the reflected editor transactor service, executable property records, deterministic scoped recording, bounded history, and collector ownership contract.

Modules: DurinEd

Last reviewed: 2026-10-17

## Service Ownership

`DTransactor` is the abstract reflected recording boundary and `DTransBuffer`
is its default concrete implementation. `DEditorEngine` constructs one
transient `DTransBuffer` subobject and retains it through the reflected
`TObjectPtr<DTransactor> Trans` property.

This object is the editor session's only ordered history. Application Undo/Redo,
Activity History, notifications, package checkpoints, mounted-content
invalidation, property editing, and domain commands all call the same
`DTransactor` service. There is no parallel native manager or ID bridge.

## Transactions And Scoped Recording

One `Editor::FTransaction` owns a monotonic session identifier, the outermost
scope's stable context and description, an optional primary object identity,
and move-only record envelopes. A property object record stores an exact object
identity, stable top-level member locator, owned member-to-leaf path, change
kind, logical identity, and retention-neutral before/after payloads. Envelopes
contain no live container address. A custom-change record owns one
`ITransactionCustomChange` and exposes synchronous or deferred Undo/Redo,
details, affected packages, mounted-content mutation, collector traversal,
native byte accounting, and an optional owning-module tag.

Recording is game-thread-only after the game-thread identity is initialized.
The outermost Begin creates the pending transaction. Nested Begin calls push
savepoints containing the current record count and accounted bytes; their
context cannot replace the outer context. Scopes must close in reverse Begin
order.

Ending an inner scope keeps its records and closes only that savepoint.
Canceling an inner scope erases records added since its savepoint. Ending the
outer scope commits one non-empty transaction, while canceling it discards the
entire pending transaction. Empty scopes are no-ops and do not create history.

`Editor::FScopedTransaction` is move-only. Its destructor ends an active scope,
explicit End and Cancel are idempotent after a successful close, and a close
rejected for invalid nesting leaves the scope active so the caller can restore
valid close order.

### Everyday Object Editing

Ordinary synchronous editor code uses the active editor transactor implicitly:

```cpp
Editor::FScopedTransaction Transaction("Move Actor");
Transaction.Modify(Actor);
return Actor->SetActorTransform(NewTransform);
```

`Modify(...)` captures every non-transient reflected member before the caller
mutates the object. Ending the scope captures final values, removes unchanged
member records, and commits the remaining executable before/after records as
one transaction. Calling `Modify(...)` repeatedly for the same object in one
scope is idempotent. The convenience API is best-effort: an unavailable or
rejected transactor does not prevent the caller's mutation. Operations that
require rollback when history recording fails use the advanced record API or a
domain-specific `CommitApplied(...)` change. `Cancel()` discards the history
records; it does not revert mutations the caller has already applied.

`FScopedTransaction::BeginChecked` and `TryModify` expose structured preparation
results for callers that must reject execution before mutation. A default scope
starts inactive; an absent transactor or a second begin on an active scope is a
typed failure. The existing constructors and `Modify` retain their best-effort
behavior. [Editor function actions](EditorFunctionActions.md) use this checked
path, handle finalization explicitly, and distinguish invocation from recording.

Tests, independent tools, and hosts without `GEditor` may use the explicit
`FScopedTransaction(DTransactor*, FTransactionContext)` constructor. Direct
`Record(...)` and `UpdateRecord(...)` are advanced property-editing entry points
bound to the exact scope token. Custom file, asynchronous, creation/deletion,
and domain-compensation operations continue to use `Execute(...)` or
`CommitApplied(...)`. Custom changes explicitly declaring `IsSynchronous()`
may join an existing recording transaction through either entry point. The
declaration requires Execute, Undo and Redo to finish synchronously; deferred
changes retain their standalone-only execution contract.

Execute applies a joined change immediately; CommitApplied records an already
applied change without replaying it. Both contribute one record to the innermost
scope and return the outer transaction ID and current scope ID. Joining preserves
the outer context, description, prior records and redo branch until the
outermost End commits one history entry. Without an outer scope, Execute keeps
its standalone behavior. Joined replay runs behind the Executing barrier;
recursive recording, execution, Undo and Redo remain rejected. Preparation or
restored execution failure leaves the outer recording usable, while incomplete
compensation retains both prior records and the failed participant in
RecoveryRequired. A purportedly synchronous change starting deferred work is
also a contract failure requiring recovery. Cancel continues to discard
records without reverting applied mutations. Python transaction grouping is
not exposed by this native composition support.

`FTransactorResult` and `CommitApplied(...)` are `[[nodiscard]]`. A caller that
applies a mutation before recording it must handle a rejected history commit,
normally by restoring the prior state.

## Structural History And Barriers

`DTransBuffer` stores one ordered history and one cursor. Entries before the
cursor are undoable; entries at or after it are redoable. A new commit after
Undo discards the complete redo suffix before retaining the new entry. IDs are
never reused by Reset.

The buffer has Idle, Recording, Executing, Undoing, Redoing, RecoveryRequired,
and Destroying states. Begin
is rejected during transitions and destruction. Public buffer rejection gates
classify the reason with `ETransactorRejectionReason` and retain an owned
`RejectionCause`: current state, requested identifier, active transaction/scope,
Undo/Redo heads, history count, owned bytes and
module name. `FTransactorResult` stores structured causes and notice kinds/counts
without a message field. `FormatTransactorResult` generates presentation text at
UI, event and log boundaries; decisions never depend on that text. `FailureCause` separately
classifies scoped capture/preparation, expired objects and byte-accounting
failures, retaining snapshot/record causes and relevant owner/member,
array index, byte bounds or module facts. Scope cleanup results are retained
as `CleanupCause`; when cleanup rejection takes precedence, `PreparationCause`
preserves the original preparation failure. Undo, Redo and Reset
are rejected while recording. Undo validates all records before
writing, then applies property records in reverse order; Redo applies them
forward. Partial failure rolls back records already applied and leaves the
cursor unchanged. `FTransactionRecordResult` aliases
`std::expected<void, std::string>`: the record boundary formats property/custom
errors once, and callers receive an owned message without nested diagnostics.
`FTransactionApplyResult` aliases `std::expected<void, FTransactionApplyError>`.
Success has no payload. Failure retains a disposition (validation failed,
execution restored, or recovery required), the failed record index and message,
and each failed rollback's index and message. Transaction identity and direction
remain with the executing transaction and outer transactor operation.
`FTransactorResult::ApplyCause` retains only this flat failure data, including the
sole rollback-failure list. Deferred operation failures also retain an owned
message in `FTransactionCompletionError`, not a record-specific cause chain.
Record implementations must
leave their own state unchanged or compensate internally when returning failure.

Failed compensation puts the buffer in RecoveryRequired, invalidates affected
package checkpoints, emits one error log and a failed event, and returns every
rollback cause to the caller. Ordinary Begin/Execute/Undo/Redo and Reset remain
rejected. Resetting history does not repair partially applied data; recovery
requires restoring affected data and recreating the editor session. Destruction
still releases history normally. Expected-ID transitions reject an ordering mismatch.
Explicit entry removal supports targeted retirement, while branch replacement,
eviction, package forgetting, Reset, and shutdown release records directly.

Before invoking a custom change, the buffer installs its deferred completion
hook and transition identity. A pending change holds the non-reentrant state
until completion; success alone moves the cursor and publishes success.
Failure leaves the cursor unchanged. Completion callbacks receive
`FTransactionCompletionResult`, whose success derives from its typed error.
Operation failures retain the record cause and the buffer stamps the exact
transaction ID and operation before forwarding the result. Execute finalization
failures retain the complete transactor result as a separate cause. Formatting
occurs only when publishing a failed history event. A successful retry does not
alter a previously returned error. Module shutdown drains every retained
change tagged with that module and rejects retirement while one is active.

Consumers pull immutable committed, undone, redone, failed, discarded, and
evicted event values. The buffer invokes no event callback during a state
transition.

## Package And Discovery Revisions

Each finalized transaction stores collector-visible package references and the
before/after session revision assigned to every affected package. Execute,
Undo, and Redo apply those revisions only after all records complete
successfully; a failed or deferred transition leaves the cursor, revisions,
dirty state, mounted-content revision, and success-event stream unchanged.

`DTransBuffer` owns the current revision, saved revision, and checkpoint
validity for each tracked package. Successful save establishes the current
revision as saved. Known-clean activation establishes a checkpoint, persistent
out-of-band edits invalidate it, and forgetting a package removes its state and
collector edge. Dirty state is synchronized from revision equality only while
the checkpoint is valid.

For a valid checkpoint, Undo back to the saved revision clears `DPackage::IsDirty()`;
Redo away marks it dirty. Saving at any history position preserves valid Redo
history. A new edit after Undo receives a fresh revision, so a discarded saved
branch cannot alias its replacement. An invalid checkpoint stays conservatively
dirty until a successful save or known-clean activation establishes a new one.
Revision metadata is editor-session state and is not serialized into `DPackage`.

Saved package reload registers a `DTransBuffer` native replacement participant.
Preparation is accepted only while Idle and records every whole transaction whose
collector-visible payload, context, custom change, or package transition retains a
target graph. Validation repeats that set immediately before commit. Atomic commit
removes those transactions in full rather than rewriting their payloads; unrelated
entries remain. If a retired transaction also changed another package, its applied
content and Dirty state remain, but its save checkpoint is invalidated. After
successful old-graph retirement, the editor establishes the replacement package as
the new saved checkpoint. A precommit reload failure leaves history, cursor, package
revisions, checkpoints, and Dirty state unchanged.

Custom changes that report mounted-content mutation advance one monotonic
discovery revision after each successful Execute, Undo, or Redo. Ordinary
in-memory object and property edits do not advance it.

## Bounds And Accounting

Production uses a fixed 64 MiB owned-native-byte budget, without an entry-count
cap or a dynamic-limit API. `GetMemoryBudget` exposes the configured budget;
only private native-test setup can select a different initial budget.
Accounted size includes each transaction object, owned context and description
capacity, record-container capacity, record envelopes, encoded payload
capacity, hard-reference tables, and each custom-change object plus its
reported native allocations. It excludes the transitive managed graph
reached through collector edges. All size aggregation is overflow-checked.

Custom Execute checks candidate accounting before replay. If replay makes
accounting invalid or overflows size aggregation, the buffer compensates the executed change
before rejecting it and retains the existing redo branch. Failed compensation
requires recovery and retains the pending participant graph. An internal custom
change reporting incomplete rollback or throwing during replay also locks history into RecoveryRequired.
Deferred compensation cannot complete a synchronous finalization failure: it
requires recovery and retains the pending entry. Pending recovery entries block
module draining until the owning buffer is retired.
Already-applied property recordings are not automatically compensated.
The byte budget never rejects a valid edit. Before appending a successful new
transaction, the buffer trims oldest existing entries while that old history
occupies at least the budget. The incoming transaction does not participate in
that comparison, so it remains undoable even when oversized. There is no special
minimum-entry rule. Replacing a redo branch removes that suffix and skips byte
trimming of the remaining undo prefix. Undo and Redo do not trim history.
Canceled, no-op and failed submissions leave history unchanged. Removed record
data and object edges are erased before successful finalization returns.

UE 5.3.2 source confirms that a newly completed oversized transaction remains
undoable. `UTransBuffer::BeginInternal` trims existing history before appending
the next transaction, while `End` finalizes without a byte-limit rejection.
See Epic's source in the pinned public mirror:
[TransBuffer.h](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Source/Editor/UnrealEd/Classes/Editor/TransBuffer.h#L70)
and [EditorTransaction.cpp](https://github.com/chenyong2github/UnrealEngine/blob/c865e168d0935b8e5f4bd865ddcc1c733c8ce7cf/Engine/Source/Editor/UnrealEd/Private/EditorTransaction.cpp#L1275).
Durin uses the same old-history byte comparison and redo-branch alternative,
but delays destructive trimming until successful finalization, immediately before
appending the new transaction. This avoids UE's additional removed-transaction
storage and restoration on Cancel or a transient transaction. During recording,
old history remains visible and retained; after successful commit, byte-based
history selection follows the inspected UE algorithm. No dynamic limit changes
or count-based evictions are supported.
This source check covers UE 5.3.2, not uninspected later versions. UE's public
[FChange::GetSize](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FChange/GetSize)
also documents approximate accounting and oldest-entry trimming.

## Collector And Shutdown Contract

`DTransBuffer::AddReferencedObjects` calls the superclass collector and then
traverses the pending transaction and every undoable and redoable transaction.
Each transaction reports its optional primary object and delegates to its
focused, property object, or custom-change records. Targets, transaction
package transitions, tracked package state, and explicitly reported hard custom
participants are therefore retained through
ordinary GC reachability; weak and
soft references do not gain retention, and marked-garbage objects are not
rescued.

Inner or outer cancellation, redo replacement, eviction, Reset, buffer
destruction, and editor-engine shutdown erase their corresponding native
records and collector edges. Property records own no callback. Custom changes
disconnect completion during destruction, and module drains ensure their
executable code and deleters do not outlive the owning module.

## Related Documentation

- [Transaction Record Foundation](TransactionRecords.md)
- [Reflected property editing](ReflectedPropertyEditing.md)
