# Package Async Save Contracts Plan

Summary: Add UE-style SAVE_Async admission-only background writes and replace explicit asset-save completion with a task-returning DPackage::SaveAsync while preserving transactional saves.

Last reviewed: 2026-09-17

Status: Active
Completed:

## Current Status

Planning only; implementation has not started. The selected public direction is
`SavePackage(..., SAVE_Async)` without a per-save task handle, and
`DPackage::SaveAsync(...)` returning a task for the complete protected save.
Implementation begins with Stage 0; unresolved API integration details below are
not implemented contracts.

The completed [Asset Save Flow Simplification](AssetSaveFlowSimplification.md)
is the baseline. Follow-up commits `9a0d0b1c7` and `691e07327` unified Engine save
preparation and gave each writer unique staging/backup siblings. Save preparation
no longer recovers or deletes abandoned backups. Ordinary sync and async asset
saves share `FinishOrdinarySave`; explicit bundles retain coordinated rollback.

Current interfaces differ from the requested endpoint:

- CoreDObject `DPackage::Save` and `SaveAsync(Admission, Options)` persist files
  without Engine. The latter returns `unique_ptr<FPackageSaveOperation>` and
  requires explicit completion.
- Engine `FAsyncPackageSave::Begin` returns an operation with `IsReady` and
  GameThread `Complete`, including asset admission and Registry publication.
- AssetTools `FAssetSaveOperation` wraps the Engine operation and publishes
  editor results once. It must consume the final task result after migration.
- Task waits do not pump GameThread work. A task that depends on deferred
  GameThread completion cannot be synchronously waited on by that thread.

## Goal

Expose two intentionally different asynchronous contracts with UE-style naming.
Preserve synchronous saving and explicit atomic bundle guarantees. Reuse shared
preparation without wrapping ordinary saves in a bundle transaction or adding a
second task framework.

## Selected Contracts

| Entry point | Return meaning | Background I/O | Completion and recovery |
| --- | --- | --- | --- |
| `SavePackage` without `SAVE_Async` | Final synchronous result | Existing policy | Preserve current publication and rollback behavior |
| `SavePackage(..., SAVE_Async)` | Request accepted or rejected; no task handle | Write detached bytes directly to final files | No old-file rollback; internal owner reports completion and failures |
| `DPackage::SaveAsync(...)` | Task handle with a typed final save result | Stage detached bytes | Task finishes after validation, commit, Registry publication when applicable, and finalization or rollback |
| `SavePackagesAtomically(...)` | Existing transactional result | Existing policy | Preserve multi-package ordering and explicit Registry-failure rollback |

`SAVE_Async` is a scheduling flag, separate from Delta/Complete serialization
mode. Do not expose `EnqueuePackageSave` as an additional public alternative.
`SaveAsync` always selects protected saving; reject incompatible direct-write
flags rather than silently weakening that API. Supply completion options at
submission time, since there will be no public `Complete(Options)` step.

### Admission-Only Background Writes

- Capture and validate live objects on GameThread before accepting detached work.
  Copy options and retain buffers and required package/module ownership internally.
- Do not expose a per-save Task or require callers to retain an operation object.
  Internal task tracking is still required for bounded work, diagnostics and drain.
- Use no transaction backup or old-file restoration for this path. A write may
  truncate an old file or leave only part of the main/bulk set published.
- Reuse destination/version admission and coordinate all writes to the same
  physical closure across sync, direct-write, protected-task and bundle paths.
  A stamp check alone is not a lock across an asynchronous write interval.
- Protect Engine reads/resource admission from accepting a closure while direct
  writes are in progress. Define how existing lazy bulk readers are handled before
  allowing the first destructive write; do not weaken their generation checks.
- Admission success must not clear Dirty, mark a new package published, report
  Persisted, or publish Registry metadata. Do these on GameThread after successful
  I/O, preserving Dirty if the package was edited after capture.
- On partial write failure retain Dirty, report the affected paths and partial-write
  disposition, and fence the affected asset projection as required. Do not publish
  success or present this as an untouched old package. Version control is an
  external recovery option, not an automatic recovery mechanism.
- Reuse committed-content/projection-pending reporting when bytes succeeded but
  Registry publication failed. Define retry/reconciliation admission explicitly so
  a failure fence does not permanently prevent recovery.
- Provide UE-style `HasAsyncFileWrites()` and `WaitForAsyncFileWrites()` on the
  agreed package API surface. Their global scope, drain cutoff and distinction
  between disk completion and GameThread publication must be explicit.
- Match UE's interface direction, not its Fatal-on-write-error implementation:
  surface asynchronous diagnostics through an internal completion/error sink.

### Task-Returning Protected Saves

- Replace the public Engine `FAsyncPackageSave::Begin` / `Complete` protocol with
  `DPackage::SaveAsync`. Reuse the existing typed Task system; the handle must
  represent the full save, not merely staging readiness.
- Retain current stale-object, participant, destination and staged-file checks,
  transaction-owned backup paths, file rollback and Registry failure policies.
  No persistent multi-file crash-recovery guarantee is introduced.
- No caller polling or completion call may be necessary to trigger publication.
  A normal host's GameThread executor must advance the internal continuation.
- Distinguish task execution state from save-domain result: task execution success
  must not hide a failed save, partial recovery or projection-pending disposition.
- Explicitly define cancellation, dropping the handle and once-only result delivery.
  Do not inherit the old operation-destructor cancellation behavior accidentally;
  background state must outlive handle release safely and release object pins on
  GameThread. A commit already in progress must reach a defined terminal state.
- Preserve CoreDObject's Engine-free file persistence capability. Moving the public
  asset entry point to DPackage must not add an Engine/AssetRegistry dependency to
  CoreDObject or silently bypass asset readiness/publication for editor callers.

## Implementation Stages

### Stage 0: Resolve Public Surface and Completion Ownership

Dependencies: none. Outcome: reviewed, concrete signatures and ownership design.

- [ ] Inventory declarations and consumers of `Save`, `SavePackage`, `SaveAsync`,
  `FPackageSaveOperation`, `FAsyncPackageSave`, and editor completion wrappers in
  source/test roots of every project in `Durin.dworkspace` (Engine, Sandbox,
  RoadWeaver). Record affected modules and targets here.
- [ ] Specify the UE-style `SavePackage` surface, save-flag type and options,
  typed task result, admission-error representation, and exact global wait/query
  signatures. Resolve the existing CoreDObject `SaveAsync` signature collision.
- [ ] Select a lower-layer extension/context mechanism for Engine asset policy
  and completion, including registration lifetime and missing-provider behavior.
  Give explicit file-only and asset-aware call examples and map every existing
  caller. Avoid implicit routing that can silently omit Registry publication.
- [ ] Define same-path write admission and reader coordination, release of pins,
  partial-write reporting, error sink, retries and shutdown sequencing.
- [ ] Define headless completion and wait behavior against the existing Task
  contract: no implicit GameThread pumping inside ordinary Task waits. Select an
  explicit host drain/pump for final publication where needed, and distinguish it
  from waiting only for file I/O. Reject unsupported waits without hanging.
- [ ] Record the resolved design in this plan before changing shared APIs.

Acceptance: signatures, ownership diagram and failure/result mapping cover both
CoreDObject-only tools and Engine/editor callers without a dependency inversion
or GameThread self-wait.

### Stage 1: Add Internally Owned SAVE_Async Writes

Dependencies: Stage 0. Outcome: admission-only saves run without a caller handle.

- [ ] Add flags and direct-file output using shared detached preparation; retain
  the existing protected writer for sync, task and explicit transaction paths.
- [ ] Add bounded internal lifetime tracking, global query/wait, startup admission
  and shutdown drain, including GameThread result delivery.
- [ ] Implement same-path coordination and lazy-resource safety, Dirty/revision
  handling, Registry publication/fencing, diagnostics and retry behavior.
- [ ] Test admission rejection, delayed I/O after return, absence of backup files,
  success, partial main/bulk failure, changed revision, competing write modes,
  global drains and shutdown with queued/running writes.

Acceptance: success is never reported at admission as persisted content; direct
I/O failure is observable and cannot publish an inconsistent asset as valid.

### Stage 2: Return a Complete Save Task from DPackage::SaveAsync

Dependencies: Stages 0 and 1. Outcome: one task represents protected completion.

- [ ] Implement the selected CoreDObject/Engine integration and owned preparation,
  staging, GameThread validation/publication and terminal result continuation.
- [ ] Migrate both the existing package-only SaveAsync and Engine asynchronous
  owner; keep explicit staged coordinator internals only where still needed.
- [ ] Test no publication before staging, automatic completion without an old
  `Complete` call, final-result timing, stale inputs, rollback and partial recovery,
  cancellation, dropped handles, reentrancy and exactly-once callbacks.
- [ ] Test normal host ticking, headless completion, unsupported GameThread waits,
  deferred-queue admission failure and shutdown before object/module teardown.

Acceptance: callers can observe a final save result through a Task; no live-object
mutation occurs on an I/O worker, and no completion path can deadlock its owner.

### Stage 3: Migrate Consumers and Retire the Old Public Protocol

Dependencies: Stage 2. Outcome: all workspace callers use the selected contracts.

- [ ] Migrate AssetTools, import queues, tests and other discovered callers to the
  protected task result where they need persistence confirmation or notifications.
  Do not switch these callers to weak SAVE_Async merely to simplify migration.
- [ ] Remove public `FAsyncPackageSave` and obsolete caller-driven completion
  adapters after migration; preserve necessary internal bundle coordination.
- [ ] Verify sync saves, serialization modes, bulk removal, prepared graph
  publication, atomic bundles and Registry-failure rollback retain their contracts.
- [ ] Search all workspace project source/test roots again for removed APIs and
  update affected project targets as well as owning modules.

Acceptance: no stale public API consumers; editor callbacks fire once only after
the final save result, and explicit transaction guarantees remain intact.

### Stage 4: Validate and Publish the Implemented Contracts

Dependencies: Stage 3. Outcome: validated migration and authoritative documentation.

- [ ] Follow [Testing](../Agents/Testing.md) to select registered targets. Include
  package persistence, asset package, import/async save, task lifecycle and the
  affected explicit transaction tests; add fault injection at destructive writes
  and completion boundaries. Verify independent case setup where relevant.
- [ ] Complete the required `all` build for this shared Engine API migration and
  affected project validation under [Build And Run](../Agents/BuildAndRun.md).
- [ ] Update [Package Persistence](../Runtime/Core/PackagePersistence.md),
  [Asset Packages](../Runtime/Assets/AssetPackages.md),
  [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md), and
  [Async Asset Operations](../Editor/Architecture/AsyncAssetOperations.md).
  Update [Task System](../Runtime/Core/TaskSystem.md) only if its contract changes.
- [ ] Record exact passed validation and any outstanding gates, validate the plan
  and changed documents, and mark completion only after every required gate passes.

## Local UE Reference

The reference examined is the installed UE 5.8 source at
`E:/Programs/Epic Games/UE_5.8/Engine/Source/Runtime/CoreUObject/Private/UObject/`.
`SavePackage/SaveContext.h` selects memory saving for `SAVE_Async` or a custom
writer. Without a custom writer, `SavePackage2.cpp::FinalizeFile` sends detached
output buffers to `SavePackageUtilities.cpp::AsyncWriteFile`, which returns no
Task and writes final paths directly. Its global outstanding-write counter backs
`UPackage::HasAsyncFileWrites` and `WaitForAsyncFileWrites`. Its synchronous
`FinalizeTempOutputFiles` retains old files through `FPackageBackupUtility` and
restores them on publication failure. Custom/Cook package writers have separate
policies; these findings do not describe every UE save path.

This reference supports the selected naming and distinction between admission
and completion. Asynchronous execution itself does not require weaker rollback.
