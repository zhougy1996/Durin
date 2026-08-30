# Editor Transaction System Roadmap

Summary: Replace transaction-owned manual roots with a GC-integrated editor transactor, object records, and one extensible Undo/Redo history.

Last reviewed: 2026-08-30

Status: Active
Completed:

## Current Status

Durin currently has a command-oriented `Editor::ITransaction` contract and an
ordinary `Editor::FTransactionManager` owned by `DEditorEngine`. The manager
provides bounded Undo/Redo history, deferred completion, package revision and
dirty-state tracking, mounted-content mutation revisions, and user-visible
events. Reflected property editing adds focused before/after snapshots, while
property snapshots and manager package records keep referenced objects alive
through counted `FScopedObjectRoot` instances.

CoreDObject already provides the foundation needed for a collector-integrated
design: reachable `DObject` instances may report native strong edges through
`AddReferencedObjects(FReferenceCollector&)`, and reflected strong references
participate in the same mark phase. No transactor object, persistent transaction
object reference, automatic object record, scoped transaction, or transaction
buffer currently exists.

No child implementation plan is active. The first plan may start after its P0
entry gate is satisfied.

## Outcome

Provide one editor-session transaction system with the familiar Durin equivalents
of Unreal's transaction concepts:

- `DTransactor`, the abstract transient editor transaction service;
- `DTransBuffer`, the default bounded Undo/Redo buffer;
- `FTransaction`, one transaction containing object records and optional custom
  changes;
- `FScopedTransaction`, the RAII Begin/End/Cancel boundary; and
- `FPersistentObjectRef`, the transaction-safe identity and reachability record
  for a `DObject` participant.

`DEditorEngine` owns the active transactor through a transient reflected strong
reference. `DTransBuffer::AddReferencedObjects(...)` traverses its non-reflected
`FTransaction` history, and each transaction reports the strong object edges
required by its records. Undo history therefore participates in ordinary GC
reachability without adding one root-set entry per snapshot or tracked package.

The completed system retains the behavior already owned by DurinEd: one global
ordered history, focused interactive property edits, package revision and dirty
state, deferred operations, mounted-content mutation notifications, activity
events, and safe module shutdown. Object snapshots and domain-specific custom
changes coexist in one `FTransaction`; neither is a second history stack.

## Program Decisions

- Adopt the public concept names `DTransactor`, `DTransBuffer`, `FTransaction`,
  `FScopedTransaction`, and `FPersistentObjectRef`. Durin semantics remain
  authoritative; source compatibility with Unreal Engine is not a goal.
- Keep the reflected transactor classes in `Durin` and the editor-facing helper
  APIs in `Durin::Editor`, consistent with the existing reflected-object and
  editor-contract namespace boundary.
- Maintain one transactor per `DEditorEngine`, shared by Level and asset editor
  workspaces. Transaction context and primary-object identity describe the
  producing tool; they do not create per-workspace history stacks.
- Make GC reference enumeration, not manual rooting, the normal retention path
  for committed transaction history. `AddToRoot` and `FScopedObjectRoot` remain
  valid general lifecycle tools but are not the transaction ownership model.
- Preserve command/custom-change support for filesystem mutations, async
  operations, graph edits, object creation and deletion, and other behavior
  that cannot be represented safely as a reflected property snapshot.
- Keep `FAssetMutationJournal` independent. Undo/Redo is editor-session history;
  the mutation journal is the crash-recovery protocol for authoritative files.
- Do not persist the ordinary Undo buffer in this roadmap. Crash/session replay
  requires a separately selected durable event format and recovery policy.
- Migrate consumers before removing `Editor::ITransaction` or
  `Editor::FTransactionManager`; no milestone may leave two user-visible global
  histories.

## Scope

Required work includes GC-visible transaction ownership, stable participant
references, object records, nested scoped transactions, custom changes, history
memory policy, package revision integration, editor-engine ownership, migration
of existing property and command transactions, and removal of transaction-owned
manual roots.

This roadmap does not redesign runtime asset mutation atomicity, add persistent
crash recovery for ordinary property edits, make gameplay mutations
transactional, or require every domain command to become a whole-object
snapshot.

## Milestones

- [ ] **P0: Transaction reference and record foundation.** Create the
  `EditorTransactionReferenceFoundation` plan after inventorying every current
  transaction-owned root, raw participant pointer, object creation/deletion
  command, and module-owned transaction implementation. Deliver
  `FPersistentObjectRef` semantics, collector traversal for non-reflected
  transaction records, and focused serialization/restore primitives without
  changing the active editor history. Completion requires GC tests proving that
  referenced participants survive collection, weak or evicted participants do
  not, stale generations never resolve to reused slots, and Outer/subobject
  identity behavior is explicit. This milestone depends only on the implemented
  CoreDObject collector and handle contracts.

- [ ] **P1: Transactor and buffer core.** Create the `EditorTransactorCore` plan
  after P0 completes. Deliver abstract transient `DTransactor`, concrete
  `DTransBuffer`, `FTransaction`, `FScopedTransaction`, Begin/End/Cancel nesting,
  Undo/Redo barriers, memory-bounded history, event publication, and a transient
  `DEditorEngine` ownership edge. Completion requires the new buffer to pass
  lifecycle, nesting, cancellation, GC, history eviction, and shutdown tests
  while running beside—but not yet serving—the legacy manager. This milestone
  depends on P0.

- [ ] **P2: Object and reflected-property transactions.** Create the
  `ObjectTransactionAndPropertyMigration` plan after P1 completes. Deliver
  automatic transaction object records for supported reflected edits and move
  `FPropertyEditSession` and `FPropertyTransaction` onto
  `FScopedTransaction`/`FTransaction`. Preserve detached validation,
  normalization, interactive preview, cancel, exact member-to-leaf paths,
  multi-object edits, container structure changes, notifications, and saved
  revision behavior. Completion requires property snapshots and committed
  property history to stop installing per-reference roots, with all existing
  reflected-property contract tests passing. This milestone depends on P1.

- [ ] **P3: Custom changes and command migration.** Create the
  `EditorCustomTransactionMigration` plan after P2 completes. Deliver one
  custom-change representation inside `FTransaction` and migrate existing
  `ITransaction` consumers, including asset relocation/deletion adapters,
  Content Browser operations, material graph commands, Level mutations,
  transform gizmos, attachment, placement, and deferred operations. Completion
  requires each migrated operation to preserve failure, compensation,
  recovery-required, package revision, mounted-content revision, notification,
  and module-retirement behavior without retaining legacy history entries. This
  milestone depends on P2 and may be split into bounded module-owned child
  plans after the shared custom-change contract lands.

- [ ] **P4: Global cutover and legacy removal.** Create the
  `EditorTransactionGlobalCutover` plan after every required P3 consumer is
  migrated. Route application Undo/Redo, workspace adapters, Activity History,
  saving checkpoints, and test seams through `DEditorEngine::Trans`; remove
  `Editor::FTransactionManager`, `Editor::ITransaction`, and transaction-specific
  root retention. Completion requires one global history in every editor host,
  no remaining legacy symbols or compatibility adapters, clean module unload,
  and updated authoritative editor and GC contracts. This milestone depends on
  all required P3 plans.

- [ ] **P5: Recovery and collaboration evaluation (conditional).** After P4,
  collect evidence for whether Durin needs persistent editor-session replay,
  multi-user transaction exchange, or both. Create a separate roadmap or plan
  only if a selected use case defines durable identity, schema evolution,
  security, storage bounds, and replay conflict policy. Completion for this
  roadmap requires an explicit decision recorded in its Current Status; no
  persistent Undo implementation is required when the evidence does not justify
  one.

## Cross-Milestone Invariants

- A committed history entry retains exactly the strong object graph required to
  apply Undo or Redo; eviction, reset, package forgetting, and transactor
  destruction release those edges before the next collection.
- A transaction never relies on native-stack scanning, raw pointer stability,
  or Outer-to-child reachability.
- Marked-garbage objects are not rescued by transaction history. Undo of object
  deletion uses an explicit creation/deletion record rather than attempting to
  revive a garbage instance.
- Failed Undo or Redo does not advance history position, package revision,
  mounted-content revision, or user-visible success events.
- Nested scopes produce one user-visible transaction unless explicitly canceled;
  a canceled inner or outer scope has deterministic record-discard semantics.
- New edits after Undo discard the redo branch and release its object references
  in the same logical operation.
- Transaction records containing module-owned executable behavior cannot outlive
  the owning module lease. Prefer data-driven shared changes where practical and
  drain remaining module-owned records during retirement.
- Transaction byte accounting, not only entry count, bounds retained history.
- Asset mutation Undo/Redo continues to honor `RecoveryRequired`; editor history
  never conceals or retries a recovery-required filesystem transaction as an
  ordinary object restore.

## Validation Strategy

Each child plan selects the smallest registered native-test targets following
[Agent Testing Workflow](../Agents/Testing.md) and performs the build coverage
required by [Agent Build And Run Workflow](../Agents/BuildAndRun.md). Across the
program, acceptance must cover:

- collector reachability through `DTransBuffer -> FTransaction -> object record`;
- collection after history eviction, reset, branch replacement, and shutdown;
- stable-handle generation reuse, rename, reparent, subobject, and package
  unload behavior;
- nested Begin/End/Cancel, no-op scopes, barriers, and memory limits;
- property value, struct, array, map, hard/weak/soft reference, multi-object,
  interactive, cancel, Undo, and Redo behavior;
- created, deleted, renamed, and reparented object records;
- synchronous and deferred custom changes, including failed compensation;
- package dirty/saved revision restoration at arbitrary history positions;
- one mounted-content mutation revision per successful Execute, Undo, or Redo;
  and
- editor/module shutdown with no retained callback into unloaded code.

## Risks

- A naive whole-object serializer can bypass property hooks or restore transient
  invariants incorrectly. Object records must use the same validated mutation
  and notification contracts as live editing where those contracts apply.
- Strong transaction references can retain large package graphs. Memory
  accounting and eviction policy must land with the buffer, not as later
  optimization.
- Durin does not currently expose Unreal's complete object-reinstancing model.
  `FPersistentObjectRef` must promise only identities that CoreDObject can
  resolve and must not imply unsupported resurrection or reinstancing.
- Existing command transactions encode domain-specific compensation and async
  completion. Forcing them into property snapshots would weaken correctness;
  custom changes are a required part of the target architecture.
- Public UE-like names can suggest behavioral compatibility. Documentation and
  APIs must state Durin-specific lifetime, nesting, persistence, and failure
  contracts explicitly.

## Completion Criteria

The roadmap is complete when P0 through P4 satisfy their exit gates, P5 is
explicitly completed or declined, one `DTransBuffer` owned by `DEditorEngine`
serves all editor Undo/Redo, transaction history participates in GC through
reference collection rather than per-snapshot roots, every current transaction
consumer is migrated, legacy manager interfaces are removed, and lasting
contracts are updated in the owning CoreDObject and editor architecture
documents.

## Related Documentation

- [Garbage Collection](../Runtime/Core/GarbageCollection.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Editor Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Asset Catalog And Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
