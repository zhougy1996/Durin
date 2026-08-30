# Editor Transaction Reference Foundation Plan

Summary: Establish exact transaction participant identity, GC-enumerable non-reflected records, and root-free focused snapshot primitives without changing the active editor history.

Last reviewed: 2026-08-31

Status: Completed
Completed: 2026-08-30

## Current Status

P0 is complete. `CoreDObject` now exposes one retention-neutral property
snapshot payload containing owned bytes and exact hard-reference handles. The
legacy `FPropertyValueSnapshot` remains a retaining adapter over that shared
payload and now owns copyable `TStrongObjectPtr` references.

`DurinEd` now provides `FPersistentObjectRef`, a deduplicated transaction
reference set, a stable top-level member locator, and
`FFocusedTransactionObjectSnapshot`. Reachable native snapshot owners enumerate
exact live target and hard-value identities through `FReferenceCollector`;
weak and soft values remain non-retaining. Focused restore resolves and checks
the current member, then decodes only into detached reflected storage.

`CoreObjectTests` (85 tests), `CorePropertyValueSnapshotTests` (17 tests),
`EditorOperationTests` (25 tests), and `EditorPropertyTests` (32 tests) passed
on `MacOS-arm64-Debug-DurinEditor`. The complete registered `all` profile build
also passed. Lasting contracts are recorded in
[Transaction Record Foundation](../Editor/Architecture/TransactionRecords.md)
and [Garbage Collection](../Runtime/Core/GarbageCollection.md). The active
`FTransactionManager` remains the only user-visible history; P1 may now begin.

## Goal

Deliver the reference and record substrate needed by a future `DTransBuffer`:

- `Editor::FPersistentObjectRef` identifies exactly one live object-array
  generation and can report that object as a native GC edge;
- a non-reflected transaction object record can enumerate its target and hard
  snapshot references through `FReferenceCollector`;
- focused reflected-property state can be captured into owned bytes and
  restored into detached storage without installing manual roots; and
- tests prove the lifetime, stale-identity, hierarchy, and hard/weak reference
  behavior before any production Undo/Redo owner consumes the new records.

## Scope

This plan changes `CoreDObject` only where a retention-neutral property snapshot
payload or focused restore primitive is required, and adds the editor-facing
reference and record foundation to `DurinEd`.

It does not add `DTransactor`, `DTransBuffer`, `FTransaction`, or
`FScopedTransaction`; route application Undo/Redo through new code; migrate an
existing `ITransaction`; remove a current root; implement whole-object
serialization; or add object resurrection. Creation/deletion records, history
memory policy, package revisions, custom changes, and module-retirement drains
belong to later roadmap plans.

## Entry Inventory

### Transaction-Owned Manual Retention

| Owner | Current retention | P0 treatment |
| --- | --- | --- |
| [`FPropertyValueSnapshot`](../../Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h) | Copies now retain every hard referenced object with independently owned `TStrongObjectPtr` values. | The retention-neutral payload remains the transaction-record representation. |
| [`FPropertyTransaction`](../../Engine/Source/Editor/DurinEd/Public/Editor/PropertyEditing.h) | Committed history uses exact handles and collector traversal. | The object record uses `FPersistentObjectRef` plus collector traversal. |
| [`FPropertyEditSession`](../../Engine/Source/Editor/DurinEd/Public/Editor/PropertyEditing.h) | Pins the live edit target before using snapshot-container addresses and deferred callbacks. | Temporary native ownership uses `TStrongObjectPtr`. |
| `FTransactionManager::FTrackedPackageState` (removed by P4) | Historically stored a raw `DPackage*` with a scoped manual root. | P1/P4 replaced package-history ownership after the transactor was introduced. |

`FAssetRetentionService`, preview scenes, default materials, package residency,
and general-purpose native strong-reference users are not transaction-owned and
are therefore outside this plan.

### Production Transaction Implementations

| Module | Implementations and retained form | Foundation consequence |
| --- | --- | --- |
| `DurinEd` | `FPropertyTransaction` retains a raw `FPropertyEditTarget`, rooted target, rooted hard-reference snapshots, and raw affected-package pointers. | P0 must cover exact target identity, hard snapshot edges, weak snapshot handles, and focused member state. |
| `AssetTools` | `FAssetMutationEditorTransaction` owns a data-bearing Engine mutation transaction and module-owned Undo/Redo methods. | It is a future custom change, not an object snapshot. The record foundation must not require all transactions to be reflected-property records. |
| `ContentBrowser` | `FContentDeletionTransaction` owns filesystem plans, hooks, staging paths, and an asset-deletion transaction. | It remains a future custom change with explicit recovery behavior and no managed-object resurrection. |
| `LevelEditor` | `FSplineSnapshotTransaction`, `FCreateSkyBoxTransaction`, `FSetSkyBoxTextureTransaction`, `FStaticMeshLevelMutationTransaction`, `FTerrainPlacementTransaction`, `FActorAttachmentTransaction`, `FPrimaryCameraTransaction`, `FActorVisibilityTransaction`, and `FTransformTargetTransaction` retain native `TObjectPtr`, `TWeakObjectPtr`, raw package pointers, or `shared_ptr` target adapters. None of those native holders is automatically enumerated by GC. | Later migration must classify each participant edge as strong, weak, created/deleted identity, or custom data. P0 supplies only the common strong-reference and focused-record vocabulary. |
| `MaterialEditor` | `FMaterialGraphTransaction` retains a weak material handle, raw affected package, and graph/presentation values. | The material owner is intentionally weak today; graph values remain a future data-driven custom change. |

The manager itself also stores raw package pointers in revision transitions and
package-state map keys. `ITransaction::GetAffectedPackages()` exposes raw
package spans. These are package-history concerns for P1/P4, not hidden object
edges to fold into P0.

### Creation And Deletion Paths

- `FCreateSkyBoxTransaction` and `FTerrainPlacementTransaction` spawn on Redo
  and destroy on Undo.
- `FStaticMeshLevelMutationTransaction` applies batched create, delete, rename,
  and update deltas by actor name and value snapshots.
- Content Browser deletion composes `FContentDeletionTransaction` with the
  Engine asset-deletion protocol; relocation and redirector fix-up use
  `FAssetMutationEditorTransaction`. These are authoritative file/package
  operations, not candidates for garbage-object revival.
- Spline point and material graph node creation/deletion mutate value graphs,
  not independent `DObject` instances.

P0 records this boundary but does not implement creation/deletion history.
Future records must recreate explicitly from owned data; a persistent object
reference never revives an object marked as garbage.

## Selected Decisions

- `FPersistentObjectRef` lives in `Durin::Editor` and is implemented by
  `DurinEd`. It stores the exact `FObjectHandle` identity. It does not use an
  object path, name, Outer path, or class lookup as a fallback.
- The reference is not a root and has no retention effect merely by existing.
  `AddReferencedObjects(FReferenceCollector&)` resolves the current handle and
  reports a non-garbage object only when a reachable owner enumerates the
  reference.
- Rename and reparent preserve identity because they do not change the object
  handle. A directly referenced subobject retains itself and its Outer chain;
  referencing an Outer does not retain children or siblings.
- A generation mismatch, physical removal, or garbage state resolves as absent.
  Collector traversal must not rescue an object already marked as garbage.
- Hard references encoded inside a transaction snapshot become enumerable
  `FPersistentObjectRef` values. Weak references remain generation handles and
  are decoded only when still live. Soft references remain paths. Evicting the
  owning record removes all of its strong edges in the same logical operation.
- Reuse the existing reflected-property archive semantics through one
  retention-neutral payload/codec rather than creating a competing wire
  format. Keep the existing `FPropertyValueSnapshot` API and retaining
  behavior as a legacy adapter until P2.
- A focused record stores owned participant identity and a stable top-level
  member locator; it does not retain a snapshot-container address. Restore
  resolves the current member and validates compatibility before decoding.
- P0 restore writes only to detached property storage. It does not directly
  mutate a live `DObject` or bypass `PreEditChangeProperty` and
  `PostEditChangeProperty`; P2 will connect decoded values to the validated
  editor mutation pipeline.
- The non-reflected record exposes explicit `AddReferencedObjects(...)`
  traversal. A test-only reachable `DObject` owner will enumerate records to
  prove the same ownership chain that `DTransBuffer` will use in P1; P0 does
  not introduce a temporary production transactor.
- Capture, resolution, enumeration, and detached restore are game-thread
  operations under the current synchronous GC contract.
- Foundation records contain data and reference identities, not module-owned
  Undo/Redo callbacks. Executable custom changes and their module leases remain
  P3 work.

## Implementation Stages

### Stage 0: Freeze Public Contracts And Retention-Neutral Payload

- [x] Add the public `FPersistentObjectRef` and focused transaction-record API
  shapes under `DurinEd/Public/Editor`, with explicit null, equality, resolve,
  garbage, and collector-enumeration behavior.
- [x] Define the owned member locator and compatibility check used by a focused
  record; do not retain `SnapshotContainer` or another live storage address.
- [x] Refactor the existing property snapshot codec just enough to expose one
  retention-neutral payload that can be wrapped by both the retaining legacy
  snapshot and the collector-enumerated transaction snapshot.
- [x] Preserve `FPropertyValueSnapshot` copy/move/equality and strong-retention
  behavior with regression coverage before building new record behavior on the
  payload.

Stage 0 is complete when the public contracts compile, the codec has one source
of truth, and existing property snapshot tests pass without changing active
editor history behavior.

### Stage 1: Persistent Identity And Collector Traversal

- [x] Implement exact handle capture and resolution, including null,
  generation mismatch, physical removal, and marked-garbage rejection.
- [x] Implement `AddReferencedObjects(...)` for the persistent reference,
  snapshot reference set, and non-reflected object record.
- [x] Add a test-only reachable object owner whose override enumerates a vector
  of non-reflected records.
- [x] Prove that a referenced participant survives collection; removing the
  record permits collection; a weak-only participant is not retained; and
  marked garbage is not rescued.
- [x] Prove stale handles do not resolve after slot reuse.
- [x] Prove rename and reparent preserve exact identity, direct subobject
  retention keeps the Outer chain, and Outer retention alone does not keep an
  unreferenced child or sibling.

Stage 1 is complete when all identity and hierarchy cases pass through ordinary
`CollectGarbage()` rather than root-count inspection or native-stack lifetime.

### Stage 2: Focused Record Capture And Detached Restore

- [x] Capture a supported top-level reflected member, including nested struct,
  array, map, hard, weak, and soft reference values, into an owned focused
  record payload.
- [x] Enumerate the record target and every hard reference exactly once for GC;
  keep weak and soft references outside collector traversal.
- [x] Resolve the target and member at restore time, reject missing targets and
  incompatible members deterministically, and decode into detached storage.
- [x] Cover record copy/move/destruction and reference release so no owned edge
  survives record eviction.
- [x] Demonstrate before/after focused payloads without adding the record to
  `FTransactionManager` or exposing application Undo/Redo.

Stage 2 is complete when focused values round-trip through detached storage,
hard/weak/soft lifetime behavior is proven, and no new manual root exists in
the transaction foundation.

### Stage 3: Qualification And Lasting Contracts

- [x] Update [Garbage Collection](../Runtime/Core/GarbageCollection.md) with the
  exact collector-enumerated transaction-reference boundary and update or add
  the owning editor architecture contract for the landed foundation.
- [x] Run `CoreObjectTests`, `CorePropertyValueSnapshotTests`, and the focused
  `EditorOperationTests` cases that own the new record contract.
- [x] Run `EditorPropertyTests` to prove the legacy property transaction path
  still retains its target and hard snapshot references until P2.
- [x] Build the complete registered profile because this changes public
  `CoreDObject`/`DurinEd` contracts consumed across editor modules.
- [x] Run changed-document, all-plan, and all-roadmap validation; update this
  plan and the parent roadmap with exact evidence.

Stage 3 is complete when focused tests and the full build pass, lasting
contracts describe the implemented behavior, and the parent roadmap records
P0 completion and the P1 entry gate.

## Acceptance Gates

- [x] The entry inventory remains complete or is updated for any newly
  discovered production transaction before implementation continues.
- [x] `FPersistentObjectRef` never roots, path-resolves, resurrects, or resolves
  a reused object-array slot.
- [x] A reachable non-reflected record retains its exact target and hard value
  references through collector traversal; weak-only and evicted references are
  collectible.
- [x] Marked-garbage objects are collected even when a record still contains
  their old identity.
- [x] Rename, reparent, direct-subobject, Outer-chain, and sibling behavior
  matches the documented CoreDObject reachability contract.
- [x] Focused capture and detached restore cover supported scalar, struct,
  container, hard, weak, and soft reference values with deterministic errors
  for stale targets and incompatible members.
- [x] The active `FTransactionManager` remains the only user-visible history,
  and its current root-based property behavior is unchanged.
- [x] No production transactor, history buffer, scoped transaction, custom
  change, creation/deletion record, or consumer migration lands in P0.

## Validation

Select and run native tests through the
[Agent Testing Workflow](../Agents/Testing.md), and configure/build through the
[Agent Build And Run Workflow](../Agents/BuildAndRun.md). The required P0 lane
is:

1. `CoreObjectTests` for handle generations, GC, and hierarchy reachability;
2. `CorePropertyValueSnapshotTests` for the shared payload and legacy root
   adapter;
3. focused `EditorOperationTests` cases for persistent references and
   non-reflected records;
4. `EditorPropertyTests` for unchanged legacy transaction retention; and
5. a complete registered-profile build after focused tests pass.

## Related Code

- [`ObjectHandle.h`](../../Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectHandle.h)
- [`ObjectLifecycle.h`](../../Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectLifecycle.h)
- [`Archive.h`](../../Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h)
- [`Archive.cpp`](../../Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp)
- [`Transaction.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/Transaction.h)
- [`PropertyEditing.h`](../../Engine/Source/Editor/DurinEd/Public/Editor/PropertyEditing.h)
- [`Transactor.cpp`](../../Engine/Source/Editor/DurinEd/Private/Editor/Transactor.cpp)
- [`PropertyEditing.cpp`](../../Engine/Source/Editor/DurinEd/Private/Editor/PropertyEditing.cpp)
