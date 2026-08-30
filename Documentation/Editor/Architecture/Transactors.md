# Editor Transactor Core

Summary: Define the reflected editor transactor service, executable property records, deterministic scoped recording, bounded history, and collector ownership contract.

Modules: DurinEd

Last reviewed: 2026-08-31

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
if (!Transaction.Modify(Actor)) return false;
Actor->SetActorTransform(NewTransform);
```

`Modify(...)` captures every non-transient reflected member before the caller
mutates the object. Ending the scope captures final values, removes unchanged
member records, and commits the remaining executable before/after records as
one transaction. Calling `Modify(...)` repeatedly for the same object in one
scope is idempotent. `Cancel()` discards the history records; it does not revert
mutations the caller has already applied.

Tests, independent tools, and hosts without `GEditor` may use the explicit
`FScopedTransaction(DTransactor*, FTransactionContext)` constructor. Direct
`Record(...)` and `UpdateRecord(...)` are advanced property-editing entry points
bound to the exact scope token. Custom file, asynchronous, creation/deletion,
and domain-compensation operations continue to use `Execute(...)` or
`CommitApplied(...)`.

`FTransactorResult` and `CommitApplied(...)` are `[[nodiscard]]`. A caller that
applies a mutation before recording it must handle a rejected history commit,
normally by restoring the prior state.

## Structural History And Barriers

`DTransBuffer` stores one ordered history and one cursor. Entries before the
cursor are undoable; entries at or after it are redoable. A new commit after
Undo discards the complete redo suffix before retaining the new entry. IDs are
never reused by Reset.

The buffer has Idle, Recording, Executing, Undoing, Redoing, and Destroying
states. Begin
is rejected during transitions and destruction. Undo, Redo, Reset, and limit
changes are rejected while recording. Undo validates all records before
writing, then applies property records in reverse order; Redo applies them
forward. Partial failure rolls back records already applied and leaves the
cursor unchanged. Expected-ID transitions reject an ordering mismatch.
Explicit entry removal supports targeted retirement, while branch replacement,
eviction, package forgetting, Reset, and shutdown release records directly.

Before invoking a custom change, the buffer installs its deferred completion
hook and transition identity. A pending change holds the non-reentrant state
until completion; success alone moves the cursor and publishes success.
Failure leaves the cursor unchanged. Module shutdown drains every retained
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

Custom changes that report mounted-content mutation advance one monotonic
discovery revision after each successful Execute, Undo, or Redo. Ordinary
in-memory object and property edits do not advance it.

## Bounds And Accounting

Production limits are 256 retained entries and 64 MiB of owned native bytes.
Accounted size includes each transaction object, owned context and description
capacity, record-container capacity, record envelopes, encoded payload
capacity, hard-reference tables, and each custom-change object plus its
reported native allocations. It excludes the transitive managed graph
reached through collector edges. All size aggregation is overflow-checked.

A finalized entry larger than the byte limit is not retained. Otherwise the
buffer evicts oldest undoable entries until both limits hold, without evicting
the new commit. A limit change that would require deleting the redo branch is
rejected without changing the active limits. Removed record data and object
edges are erased before the operation returns.

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

## Validation

`EditorOperationTests` owns recording, nesting, barriers, cursor, accounting,
event, custom-change, deferred completion, module-drain, GC-retention, and
release-path coverage. `EditorShellTests` owns the
reflected engine edge and open-transaction shutdown coverage.

## Related Documentation

- [Transaction Record Foundation](TransactionRecords.md)
- [Editor Transaction System roadmap](../../Roadmaps/EditorTransactionSystem.md)
