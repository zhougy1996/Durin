# Package Async Save Contracts Plan

Summary: Add UE-style SAVE_Async admission-only background writes and replace explicit asset-save completion with a task-returning DPackage::SaveAsync while preserving transactional saves.

Last reviewed: 2026-09-17

Status: Completed
Completed: 2026-09-17

## Current Status

Stages 0-4 are complete. `SAVE_Async` owns bounded direct writes internally;
explicit-context `DPackage::SaveAsync` returns the final protected save Task.
AssetTools and all discovered Engine/Sandbox/RoadWeaver callers are migrated.
Physical reader/writer admission, lazy-resource retirement with authored-byte
retention, partial-write fencing/retry, terminal delivery and shutdown draining
are integrated. Authoritative contracts now live in the four documents linked
by Stage 4; the design record below preserves the implementation decision.

Validation on `Win64-Debug-DurinEditor` (2026-09-17):

- `test affected --report`: all 67 registered affected targets passed, including
  package/bulk/transaction/import/task-lifecycle coverage and SandboxGameplayTests.
  Receipt: `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.
- `test PackagePersistenceTests`: all 24 cases passed. Five new host completion,
  bounded admission, missing executor, rejected publication and canceled
  continuation cases also passed with `--isolate --test-jobs 4`.
- `test TextureImportWorkflowTests`: all 21 cases passed after the final shutdown
  change, including the real asset-manager shutdown draining accepted direct I/O.
  All three `FTextureImportQueueTests.DirectSave*` cases also passed with
  `--isolate --test-jobs 3`.
- `test RoadSceneIntegrationTests`: all five cases passed.
- `AssetPackageTests` includes the new lazy bulk retirement/retry case, also
  validated independently during implementation.
- Final `build --target all`: passed after all source changes.
- Workspace symbol search found no remaining declaration or consumer of the
  removed `FAsyncPackageSave` protocol across all three projects.

The broad run found and resolved a stale material diagnostic accessor in
`AssetPackageReloadTests` and shutdown before GameThread initialization in
configuration-only cook tools. No required validation gate remains outstanding.
Document and all-plan validation receipts are recorded by the completion commit.

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

- [x] Inventory declarations and consumers of `Save`, `SavePackage`, `SaveAsync`,
  `FPackageSaveOperation`, `FAsyncPackageSave`, and editor completion wrappers in
  source/test roots of every project in `Durin.dworkspace` (Engine, Sandbox,
  RoadWeaver). Record affected modules and targets here.
- [x] Specify the UE-style `SavePackage` surface, save-flag type and options,
  typed task result, admission-error representation, and exact global wait/query
  signatures. Resolve the existing CoreDObject `SaveAsync` signature collision.
- [x] Select a lower-layer extension/context mechanism for Engine asset policy
  and completion, including registration lifetime and missing-provider behavior.
  Give explicit file-only and asset-aware call examples and map every existing
  caller. Avoid implicit routing that can silently omit Registry publication.
- [x] Define same-path write admission and reader coordination, release of pins,
  partial-write reporting, error sink, retries and shutdown sequencing.
- [x] Define headless completion and wait behavior against the existing Task
  contract: no implicit GameThread pumping inside ordinary Task waits. Select an
  explicit host drain/pump for final publication where needed, and distinguish it
  from waiting only for file I/O. Reject unsupported waits without hanging.
- [x] Record the resolved design in this plan before changing shared APIs.

Acceptance: signatures, ownership diagram and failure/result mapping cover both
CoreDObject-only tools and Engine/editor callers without a dependency inversion
or GameThread self-wait.

### Stage 1: Add Internally Owned SAVE_Async Writes

Dependencies: Stage 0. Outcome: admission-only saves run without a caller handle.

- [x] Add flags and direct-file output using shared detached preparation; retain
  the existing protected writer for sync, task and explicit transaction paths.
- [x] Add bounded internal lifetime tracking, global query/wait, startup admission
  and shutdown drain, including GameThread result delivery.
- [x] Implement same-path coordination and lazy-resource safety, Dirty/revision
  handling, Registry publication/fencing, diagnostics and retry behavior.
- [x] Test admission rejection, delayed I/O after return, absence of backup files,
  success, partial main/bulk failure, changed revision, competing write modes,
  global drains and shutdown with queued/running writes.

Acceptance: success is never reported at admission as persisted content; direct
I/O failure is observable and cannot publish an inconsistent asset as valid.

### Stage 2: Return a Complete Save Task from DPackage::SaveAsync

Dependencies: Stages 0 and 1. Outcome: one task represents protected completion.

- [x] Implement the selected CoreDObject/Engine integration and owned preparation,
  staging, GameThread validation/publication and terminal result continuation.
- [x] Migrate both the existing package-only SaveAsync and Engine asynchronous
  owner; keep explicit staged coordinator internals only where still needed.
- [x] Test no publication before staging, automatic completion without an old
  `Complete` call, final-result timing, stale inputs, rollback and partial recovery,
  cancellation, dropped handles, reentrancy and exactly-once callbacks.
- [x] Test normal host ticking, headless completion, unsupported GameThread waits,
  deferred-queue admission failure and shutdown before object/module teardown.

Acceptance: callers can observe a final save result through a Task; no live-object
mutation occurs on an I/O worker, and no completion path can deadlock its owner.

### Stage 3: Migrate Consumers and Retire the Old Public Protocol

Dependencies: Stage 2. Outcome: all workspace callers use the selected contracts.

- [x] Migrate AssetTools, import queues, tests and other discovered callers to the
  protected task result where they need persistence confirmation or notifications.
  Do not switch these callers to weak SAVE_Async merely to simplify migration.
- [x] Remove public `FAsyncPackageSave` and obsolete caller-driven completion
  adapters after migration; preserve necessary internal bundle coordination.
- [x] Verify sync saves, serialization modes, bulk removal, prepared graph
  publication, atomic bundles and Registry-failure rollback retain their contracts.
- [x] Search all workspace project source/test roots again for removed APIs and
  update affected project targets as well as owning modules.

Acceptance: no stale public API consumers; editor callbacks fire once only after
the final save result, and explicit transaction guarantees remain intact.

### Stage 4: Validate and Publish the Implemented Contracts

Dependencies: Stage 3. Outcome: validated migration and authoritative documentation.

- [x] Follow [Testing](../Agents/Testing.md) to select registered targets. Include
  package persistence, asset package, import/async save, task lifecycle and the
  affected explicit transaction tests; add fault injection at destructive writes
  and completion boundaries. Verify independent case setup where relevant.
- [x] Complete the required `all` build for this shared Engine API migration and
  affected project validation under [Build And Run](../Agents/BuildAndRun.md).
- [x] Update [Package Persistence](../Runtime/Core/PackagePersistence.md),
  [Asset Packages](../Runtime/Assets/AssetPackages.md),
  [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md), and
  [Async Asset Operations](../Editor/Architecture/AsyncAssetOperations.md).
  Update [Task System](../Runtime/Core/TaskSystem.md) only if its contract changes.
- [x] Record exact passed validation and any outstanding gates, validate the plan
  and changed documents, and mark completion only after every required gate passes.

## Stage 0 Design Record

### Workspace Inventory

The inventory covers `Engine/Source`, `Engine/Tests/Native`, `Sandbox/Source`,
`Sandbox/Tests/Native`, `RoadWeaver/Source`, and `RoadWeaver/Tests/Native`, as
declared by all three projects in `Durin.dworkspace`.

- Core owns `IPackageWriter`, detached buffers, file replacement and Tasks.
  CoreDObject owns `DPackage::Save`, the existing operation-returning
  `SaveAsync`, and `FPackageSaveOperation`. Package-only async callers are in
  `PackagePersistenceTests.cpp`; staged-operation tests also exercise rollback.
- Engine owns `SavePackage`, `SavePackagesAtomically`, `FAsyncPackageSave`,
  package resources and asset-runtime shutdown. Async consumers are
  `PackageTests.cpp`, `TextureFileImportTests.cpp`, and AssetTools'
  `AssetOperations.cpp`. No Sandbox or RoadWeaver async consumer was found.
- AssetTools' `FAssetSaveOperation` is used by TextureEditor's import queue.
  It must consume the complete task before producing its once-only editor result.
- Existing synchronous calls in DurinEd, LevelEditor, MaterialEditor,
  AssetForgeBuiltins, AssetTools, DurinAssetTool and Engine/native tests retain
  asset-aware `SavePackage`. `StudioCubeGenerate` retains file-only persistence.
  RoadWeaverEditor's `RoadNetCreateDialog.cpp` and `RoadSceneIntegrationTests.cpp`
  retain synchronous asset saves. Sandbox has no package-save call to migrate.

Registered validation targets include `PackagePersistenceTests`,
`PackageWriterContractTests`, `AssetPackageTests`, `AssetBulkContainerTests`,
`AssetSaveReadinessTests`, `EditorAssetWorkflowTests`,
`TextureImportWorkflowTests`, `EditorOperationTests`,
`AsyncTaskPilotLifecycleTests`, `RoadSceneIntegrationTests`, and
`SandboxGameplayTests`, and `CoreConcurrencyTests`. Build `all` after the shared API migration; validate RoadWeaver and
Sandbox through their registered targets rather than guessed executable names.

### Explicit Context and Public Signatures

Use an explicit typed context instead of a process-global Engine callback.
CoreDObject must not depend on `FAssetResult` or Engine option types. The package
member accepts a context whose `SaveAsync(DPackage*)` returns the existing typed
Task. Contexts copy submission options into the internally owned operation.
Engine implements `FAssetPackageSaveContext`; CoreDObject implements the file-only
`FSavePackageContext`. There is no default context and no fallback from asset
policy to file-only persistence. No provider registration/unregistration is
needed; owned Engine work must drain before Engine or its consumers unload.

Implemented signatures:

```cpp
enum EPackageSaveFlags : uint32 { SAVE_None = 0, SAVE_Async = 1 };

// Engine; preserve the existing two-argument serialization-mode overload.
auto SavePackage(DPackage*, EPackageSaveFlags,
                 EAssetPackageSaveMode = EAssetPackageSaveMode::Delta) -> FAssetResult;

// CoreDObject; the context is required, and supplies the typed result.
template<class TResult, class TContext>
auto DPackage::SaveAsync(TResult& Admission, const TContext& Context)
    -> decltype(Context.SaveAsync(this, Admission));

auto FSavePackageContext::SaveAsync(DPackage*, FPackageSaveResult& Admission) const
    -> Tasks::TTask<FPackageSaveResult>;
auto FAssetPackageSaveContext::SaveAsync(DPackage*, FAssetResult& Admission) const
    -> Tasks::TTask<FAssetResult>;

static auto DPackage::HasAsyncFileWrites() -> bool;
static auto DPackage::WaitForAsyncFileWrites() -> FTaskWaitResult;
static auto DPackage::DrainAsyncSaves() -> FPackageSaveResult;
```

File-only example: `Package->SaveAsync(Admission, FSavePackageContext{Options})`.
Asset-aware example: `Package->SaveAsync(Admission, FAssetPackageSaveContext{Options})`.
The asset context copies `FAssetBundleSaveOptions` at submission, rejects private
prepared-publication contexts, and supports Complete/Delta without a later
`Complete(Options)` call. Both contexts reject `SAVE_Async`: protected saves
cannot silently switch to destructive output. Replace the old admission-reference
overload; retain staged coordinator primitives only for explicit transactions.

Synchronous admission rejection writes the domain error to `Admission` and
returns an invalid task, including when the scheduler or completion executor is
unavailable. Successful admission returns a valid task and an acceptance-only
`Admission` value. This preserves checked admission without invoking Task
construction outside scheduler lifetime. The task's value reports the final
save-domain result; no caller completion call is required.

### Ownership, Coordination and Results

```text
GameThread submission -> bounded internal save owner -> detached I/O task
                              |                           |
                              |<---- staging / write -----|
                              v
                    GameThread terminal continuation
                    validate / commit / publish / finalize
                              |
                    typed result or direct-write sink
                              |
                    release pins and closure reservation
```

The internal owner survives task-handle release. It retains copied options,
package pins and writer state; workers use only detached bytes and paths.
Terminal delivery occurs once, after cleanup, on GameThread. Cancellation is
advisory before commit, with staged output discarded; an in-progress commit
finishes publication/finalization or rollback before releasing ownership.
Task execution failure/cancellation and a successful task containing a failed
save are distinct outcomes that every consumer must inspect.

Use a Core physical-closure reservation shared by both file writers and Engine.
Normalize absolute paths and filesystem aliases before reserving `.dasset` and
`.dbulk` together; reject hard-linked files rather than treating their path aliases
as independent destinations. Direct output reserves before scheduling its writes.
Protected output may stage concurrently, but acquires exclusive destination
ownership before commit and holds it until terminal finalization. This preserves
optimistic protected staging without letting it overlap a destructive writer. An
expected timestamp alone cannot authorize a second writer. Bundles acquire their
whole closure before their first publication and unwind admission atomically.

Implementation refinement: direct capture materializes successfully read authored
bulk sources into immutable live memory without changing their content identity
or edit revision. Otherwise retiring the old resource would make a later retry
of a partially failed save unable to recover its own source bytes. External
holders of old resource handles still observe retirement; the live authored
values retain their validated snapshot for continued editing and retries.

Direct writes also require read exclusion. Existing loose resources reopen the
bulk path in `FLoosePackageResource::ReadRangeImpl`; retirement already cancels
and drains admitted reads. Capture must finish before retiring the old generation,
and retirement must finish before the first destructive write. Resource
registration, detached preparation/revalidation and package load/inspection must
participate in physical read admission. Reads already in progress must drain;
new reads reject while the direct reservation is held. Old resource handles stay
retired and require explicit reload/rebinding; do not let them read the new
generation or weaken existing size/digest checks. Owned immutable resources
remain usable because they do not reopen the overwritten files.

Direct-write admission reports only acceptance: Dirty, published state, Registry
metadata and editor Persisted results remain unchanged. After successful bytes,
publish captured metadata on GameThread; clear Dirty only when identity and edit
revision still match. Partial output reports all affected physical paths with a
new explicit partial-write disposition, keeps Dirty, and fences the Registry
projection. Do not report backup recovery paths when no backup exists. Successful
bytes followed by failed Registry publication use
`ContentCommittedProjectionPending`. A fenced destination may be retried by the
same resident package using a full fresh capture, or reconciled by explicit
validated catalog admission; fence removal follows successful publication only.
Other asset operations remain fenced throughout recovery.

The process-wide direct-write completion sink receives success and failure once
on GameThread and has a default diagnostic logger. Retain diagnostics independent
of any caller handle. Bound accepted operation count and retained detached bytes;
reject excess admission before scheduling destructive work. Initially permit at
most 64 internally owned saves and 256 MiB of retained detached output across
both asynchronous contracts. Account bytes after capture but before accepting
the request, and keep the operation slot until GameThread terminal delivery.
Oversized requests may use synchronous saving; do not admit one unbounded async
exception. Tests use scoped limits/fault injection rather than large fixtures.

### Wait and Shutdown Boundaries

`HasAsyncFileWrites` covers outstanding disk work for all internally owned async
saves in this process, including protected staging. It does not imply Registry
publication has completed. `WaitForAsyncFileWrites` snapshots admitted disk tasks
at entry, waits only those tasks, and never pumps GameThread or waits for later
submissions. It returns unsupported-thread/dependency status without pretending
the cutoff drained. Ordinary typed Task waits keep their existing no-pump rule;
GameThread waits for unfinished protected save tasks must reject.

`DrainAsyncSaves` is a separate explicit GameThread/headless host operation. It
drains the submission cutoff through terminal publication, including I/O and
GameThread continuation, and rejects reentrant use from publication callbacks.
Normal hosts advance saves through their deferred executor. Submission requires
an initialized GameThread deferred executor; headless hosts initialize the same
executor and call the explicit drain;
deferred admission failure must release pins on GameThread and produce a domain
failure before starting destructive I/O.

Shutdown closes admission, drains owned saves and diagnostics on GameThread,
then flushes Registry state, retires package resources, releases object pins and
allows module/object/task teardown. Engine's current asset shutdown retires
resources and marks packages garbage, so the new drain must precede those steps.
CoreDObject-only tools need the same explicit save drain before object teardown.
The internal tracking owner must retain cleanup authority even if a queued
continuation is canceled; cancellation cannot release its last pin on a worker.
Test scheduler cancellation and owner shutdown separately; a canceled deferred
task must never be the last owner responsible for GameThread pin destruction.

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
