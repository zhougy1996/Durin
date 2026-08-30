# Editor Transactor Core Plan

Summary: Build the reflected editor transactor, GC-visible bounded history buffer, transaction container, and deterministic scoped recording core beside the legacy history manager.

Last reviewed: 2026-08-30

Status: Completed
Completed: 2026-08-30

## Current Status

P1 is complete. DurinEd now exposes reflected abstract `DTransactor`, concrete
`DTransBuffer`, move-only `Editor::FTransaction` and `FScopedTransaction`,
deterministic nested savepoints, structural validation-only Undo/Redo, bounded
entry-and-byte history, pull events, and collector traversal through pending,
undoable, and redoable records. `DEditorEngine` owns the buffer through the
transient reflected `Trans` edge and clears it safely during shutdown.

The lasting service, recording, history, accounting, GC, and coexistence rules
are recorded in [Editor Transactor Core](../Editor/Architecture/Transactors.md),
while [Transaction Record Foundation](../Editor/Architecture/TransactionRecords.md)
remains authoritative for exact identity and focused payload behavior.

Qualification passed on the `macos-xcode-arm64` registered Debug editor profile:
`EditorOperationTests` (35 tests), `EditorShellTests` (47 tests),
`CoreObjectTests` (85 tests), and `EditorPropertyTests` (32 tests), followed by
a complete `all` build. Changed-document, all-document, all-plan, and
all-roadmap validation passed. Code search confirms that production consumers
still reference only `FTransactionManager`; the new transactor is referenced
outside its implementation only by `DEditorEngine`, the public aggregate, and
qualification tests.

## Goal

Deliver a production-owned but unserved editor transactor core that:

- exposes abstract reflected `DTransactor` and concrete `DTransBuffer` services;
- owns non-reflected `Editor::FTransaction` entries and reports all of their
  strong references to ordinary GC;
- provides move-only `Editor::FScopedTransaction` Begin/End/Cancel recording
  with deterministic nesting and no-op behavior;
- maintains synchronous Undo/Redo position, barriers, identifiers, events, and
  bounded memory accounting;
- is retained by `DEditorEngine` through one transient reflected strong edge;
  and
- can run and shut down beside the legacy manager without becoming a second
  user-visible history.

## Scope

This plan changes `DurinEd` and its focused native tests. It may extend the P0
record types with explicit owned-byte accounting required by the buffer. It may
move transaction identifiers or event value types into a shared header only
when the legacy `Transaction.h` API remains source compatible.

The plan covers data-only transaction record storage, synchronous structural
Undo/Redo transitions, nested scope savepoints, redo-branch replacement,
entry-and-byte limits, reference collection, engine ownership, reset, and
shutdown.

## Non-Goals

- Do not route application, workspace, Activity History, property editor, or
  save/checkpoint commands through `DTransBuffer`.
- Do not migrate `FPropertyEditSession`, `FPropertyTransaction`, or another
  `ITransaction` consumer.
- Do not mutate live reflected values from focused records. P2 owns validated
  before/after application, hooks, notifications, and property-path behavior.
- Do not add a public executable custom-change interface, module-owned callback,
  async completion, filesystem compensation, or module lease. P3 owns those
  contracts.
- Do not move package revision, dirty-state, mounted-content mutation, or
  recovery-required behavior away from `FTransactionManager`.
- Do not remove or weaken the legacy manager, its manual roots, or its tests.
- Do not persist transaction history or add object creation/deletion records.

## Selected Decisions

### Public Ownership And Namespaces

- Reflected `DTransactor` and `DTransBuffer` live in `Durin`; non-reflected
  `FTransaction`, `FScopedTransaction`, context, limits, results, and event data
  live in `Durin::Editor`.
- The public contracts live in a new `Editor/Transactor.h` surface rather than
  extending the legacy manager's implementation file. `DurinEd.h` exposes the
  new surface without changing current includes of `Editor/Transaction.h`.
- `DTransactor` is an abstract `DObject` interface. `DTransBuffer` is the
  concrete default implementation and owns all active, undo, and redo record
  storage.
- `DEditorEngine` owns a `DPROPERTY(Transient) TObjectPtr<DTransactor>` named
  `Trans`, constructed as an `EObjectFlags::Transient` `DTransBuffer` subobject.
  A new getter exposes it for qualification and later migration. The existing
  `GetTransactionManager()` remains authoritative in P1.

### Transaction Data Boundary

- One `FTransaction` owns a monotonic session identifier, stable context name,
  user-facing description, optional primary `FPersistentObjectRef`, and
  non-polymorphic record storage.
- P1 record storage contains data and reference identities only. It contributes
  collector edges, deterministic validation, and owned-byte accounting; it
  contains no function pointer, module-owned deleter, or Undo/Redo callback.
- The P0 focused record is the first supported payload. P1 may wrap it in an
  internal envelope so P2 can add before/after application semantics without
  changing buffer ownership. An envelope is not a public custom change.
- Undo and Redo in P1 validate the selected transaction and move the history
  position synchronously. They do not restore a live object. This structural
  behavior is deliberately unavailable to application commands until P2 adds
  executable object records.
- A stale, garbage, or incompatible required record makes the transition fail;
  the cursor, byte accounting, and success events remain unchanged.

### Recording And Nesting

- Recording is game-thread only. Begin, record insertion, End, Cancel, Undo,
  Redo, Reset, limit changes, and event consumption enforce that boundary when
  the game-thread identity is initialized.
- The outermost Begin creates the pending `FTransaction`; its context,
  description, and primary object remain authoritative. Nested Begin calls add
  savepoints to the same transaction and never create a second history entry.
- Each savepoint captures record count and accounted bytes. Canceling an inner
  scope discards records added since that savepoint and leaves its parent open.
  Canceling the outermost scope discards the complete pending transaction.
- Ending an inner scope only closes its savepoint. Ending the outermost scope
  commits exactly one entry when records remain; an empty or fully canceled
  scope produces no history entry or success event.
- `FScopedTransaction` is move-only. Its destructor ends its still-active scope;
  `Cancel()` is idempotent; moved-from and explicitly ended instances perform
  no action.

### History, Limits, And Events

- `DTransBuffer` owns one ordered history with a cursor rather than independent
  authoritative stacks. Entries before the cursor are undoable and entries at
  or after it are redoable.
- Committing after Undo erases the redo suffix and releases its object edges in
  the same operation. Reset, eviction, and destruction likewise erase records
  before returning.
- `FTransactionBufferLimits` carries both maximum entries and maximum owned
  bytes. Production defaults are 256 entries and 64 MiB; tests inject smaller
  limits before the buffer records anything.
- Accounted bytes include the transaction object, owned strings and containers,
  record envelopes, encoded payload capacity, and reference tables. They do not
  pretend to measure the transitive managed-object graph reached by GC.
- Checked arithmetic rejects an unrepresentable size. A single entry larger
  than the byte limit is finalized but not retained; the result states that no
  Undo entry was created. Otherwise the buffer evicts oldest undoable entries
  until both limits hold and never evicts the newly committed entry.
- Transaction identifiers are monotonic and are not reused after Reset.
  Structural committed, undone, redone, failed, discarded, and evicted outcomes
  are available through a pull-based event queue owned by the new buffer. P1
  does not publish them to the existing Activity History consumer.

### Operation Barriers And Shutdown

- The buffer has explicit idle, recording, undoing, redoing, and destroying
  states. Undo/Redo is rejected while recording; Begin is rejected during a
  transition or destruction; recursive transitions fail without moving the
  cursor.
- A failed transition keeps the selected entry on its original side of the
  cursor and emits only a failure event. Event callbacks are not invoked from
  inside the buffer; consumers pull immutable event values after the state
  transition finishes.
- `DTransBuffer::AddReferencedObjects(...)` calls its superclass then traverses
  the pending transaction and every retained history entry. A merely referenced
  `DTransBuffer` therefore owns all live target and hard-value edges without a
  manual root.
- `DEditorEngine::BeginDestroy()` resets the new buffer before base teardown,
  while preserving the existing legacy-manager shutdown order. The reflected
  edge is then released by ordinary object destruction; no external callback or
  pending asynchronous completion exists in P1.

## Implementation Stages

### Stage 0: Freeze The Public Core And Accounting Contract

- [x] Add the `DTransactor`, `DTransBuffer`, `FTransaction`,
  `FScopedTransaction`, transaction context, result, limit, state, and event API
  shapes under `DurinEd/Public/Editor` with the selected namespace boundary.
- [x] Keep shared identifiers and existing `FTransactionEvent` consumers source
  compatible if common value types move out of the legacy header.
- [x] Define the non-polymorphic P1 record envelope and its validation,
  `AddReferencedObjects(...)`, and owned-byte accounting contract without an
  executable callback surface.
- [x] Add overflow-safe allocated-size reporting to the focused record and its
  payload/reference containers, counting owned capacity consistently.
- [x] Add compile-time and focused unit coverage for abstract/concrete class
  identity, move-only scope behavior, default limits, and accounting.

Stage 0 is complete when the public surface compiles, its P1/P2/P3 boundaries
are explicit in API comments, and one source of truth accounts every P1-owned
record allocation.

### Stage 1: Implement Recording, Savepoints, And Barriers

- [x] Implement outermost Begin and nested savepoint creation with one pending
  `FTransaction` and outer-context authority.
- [x] Implement record insertion through the transactor boundary, rejecting
  calls without an active scope or during a state transition.
- [x] Implement inner and outer End/Cancel semantics, including partial discard,
  fully canceled transactions, empty transactions, and deterministic results.
- [x] Implement `FScopedTransaction` construction, move, explicit End, Cancel,
  and destructor behavior without double-closing a scope.
- [x] Cover mismatched close order, recursive Begin/Undo/Redo attempts, Reset
  while recording, and destruction with an open scope.

Stage 1 is complete when nested scopes create at most one candidate history
entry, every cancellation boundary has tests, and invalid or reentrant calls
cannot corrupt the pending transaction.

### Stage 2: Implement Bounded History And Structural Undo/Redo

- [x] Commit non-empty outer scopes into ordered history with monotonic IDs and
  an exact cursor; expose CanUndo/CanRedo, head identity, and descriptions.
- [x] Implement validation-only synchronous Undo/Redo, failure preservation,
  and operation barriers without applying live reflected state.
- [x] Implement redo-suffix removal on a new commit, Reset, oldest-first
  eviction, oversized-entry discard, and checked byte totals.
- [x] Implement pull-based events with deterministic ordering and enough result
  data to distinguish committed, failed, canceled/no-op, oversized, evicted,
  undone, and redone outcomes.
- [x] Test cursor movement, branch replacement, ID monotonicity, exact-limit and
  over-limit cases, byte-accounting release, failure invariants, and event
  ordering using injected small limits.

Stage 2 is complete when entry and byte bounds hold after every public call,
failed operations never advance history, and all history mutations release
their removed data before returning.

### Stage 3: Integrate GC Reachability And Editor-Engine Ownership

- [x] Override `DTransBuffer::AddReferencedObjects(...)` to traverse pending,
  undoable, and redoable transactions through the P0 record contract.
- [x] Prove targets and hard payload references survive collection through
  `DTransBuffer`, while weak/soft references do not gain retention.
- [x] Prove inner cancellation, outer cancellation, redo-branch replacement,
  eviction, Reset, and buffer destruction release the corresponding edges on
  the next collection; marked-garbage objects remain unrescuable.
- [x] Add the transient reflected `DEditorEngine::Trans` edge, construct its
  `DTransBuffer` instance, expose the new getter, and reset it during shutdown.
- [x] Prove the engine owns the buffer through reflection and that shutdown with
  open or retained transactions leaves no native callback or object edge.
- [x] Demonstrate in tests and code search that application/workspace Undo/Redo
  and every legacy consumer still use only `FTransactionManager`.

Stage 3 is complete when the exact chain
`DEditorEngine -> DTransBuffer -> FTransaction -> focused record -> DObject`
passes ordinary GC qualification and all removal paths make the retained
objects collectible without adding a transaction-owned root.

### Stage 4: Qualification And Lasting Contracts

- [x] Add the implemented transactor, nesting, history, accounting, event,
  ownership, and coexistence rules to the owning editor architecture contract;
  keep P0 identity details authoritative in Transaction Record Foundation.
- [x] Update Documentation routing only if the transactor contract becomes a
  distinct long-lived document, and update the parent roadmap with exact P1
  completion evidence and the P2 entry gate.
- [x] Run `EditorOperationTests` for transaction state, accounting, events, and
  GC traversal, and `EditorShellTests` for reflected editor-engine ownership and
  shutdown.
- [x] Run `CoreObjectTests` for the unchanged collector/handle foundation and
  `EditorPropertyTests` for the unchanged legacy property-history behavior.
- [x] Build the complete registered profile because the plan adds reflected
  DurinEd classes and changes the public `DEditorEngine` contract.
- [x] Run changed-document, all-plan, and all-roadmap validation and record the
  exact evidence in this plan.

Stage 4 is complete when focused tests and the full build pass, lasting
documentation owns the implemented contracts, P1 is complete, and the roadmap
activates no P2 work before this plan's exit gates pass.

## Acceptance Gates

- [x] `DTransactor` is abstract, `DTransBuffer` is the default reflected
  implementation, and `DEditorEngine` retains it through one transient
  reflected strong reference.
- [x] Nested Begin/End creates one history entry; savepoint cancellation,
  outer cancellation, no-op scopes, move-only RAII, and invalid close order are
  deterministic and covered.
- [x] Undo/Redo barriers reject recording-time and recursive transitions;
  failed validation does not move the cursor or emit success.
- [x] Redo replacement, Reset, eviction, oversize rejection, and destruction
  maintain exact entry/owned-byte totals and release removed records before
  returning.
- [x] Reachable pending, undoable, and redoable records retain only their target
  and hard references through collector traversal; all tested removal paths
  permit collection and no new transaction-owned manual root exists.
- [x] P1 production record storage contains no custom executable callback or
  module-owned deleter, and live reflected state is not restored by the buffer.
- [x] `FTransactionManager` remains the sole user-visible history; its package,
  deferred, mounted-content, event, and legacy root behavior is unchanged.
- [x] Engine shutdown clears both histories safely, with no P1 callback into a
  retired module and no retained transaction edge after destruction.

## Validation

Select and run native tests through the
[Agent Testing Workflow](../Agents/Testing.md), and configure/build through the
[Agent Build And Run Workflow](../Agents/BuildAndRun.md). The required P1 lane
is:

1. `EditorOperationTests` for the transactor state machine, scopes, memory
   policy, events, reference traversal, and release paths;
2. `EditorShellTests` for `DEditorEngine` reflected ownership and shutdown;
3. `CoreObjectTests` for the stable-handle and collector foundation;
4. `EditorPropertyTests` for unchanged legacy property Undo/Redo and root
   behavior; and
5. a complete registered-profile build after focused tests pass.

## Related Code

- [`Transaction.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/Transaction.h)
- [`Transactor.cpp`](../../Engine/Source/Editor/DurinEd/Private/Editor/Transactor.cpp)
- [`TransactionRecord.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/TransactionRecord.h)
- [`TransactionRecord.cpp`](../../Engine/Source/Editor/DurinEd/Private/Editor/TransactionRecord.cpp)
- [`EditorEngine.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h)
- [`EditorEngine.cpp`](../../Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp)
- [`TransactionRecordTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Editor/TransactionRecordTests.cpp)
- [`ReflectedPropertyTransactionTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/Editor/ReflectedPropertyTransactionTests.cpp)
