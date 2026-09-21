# Asset Save Flow Simplification Plan

Summary: Introduce IPackageWriter and FSavePackageContext, move AssetsSaved into AssetRegistry, and simplify ordinary saves while preserving explicit transaction guarantees.

Last reviewed: 2026-09-17

Status: Archived
Completed: 2026-09-17

## Current Status

All stages completed on 2026-09-17. AssetRegistry owns `AssetsSaved`; the
stateless Engine publication coordinator and prepared ordinary-save handoff
are removed. Core owns the shared file writer, CoreDObject owns the save context,
and ordinary saves complete directly while explicit bundles retain rollback.

Validation on `windows-msvc-x64` / `Win64-Debug-DurinEditor`:

- `build --target all` passed, including Engine, Sandbox and RoadWeaver
  (final receipt `20260917-153057-855159-33552-cmake.log`).
- Focused targets passed: PackagePersistenceTests (13),
  PackageRegistryContractTests (8), AssetPackageTests (161), EditorOperationTests
  (52), TextureTests (113), AssetSaveReadinessTests (3), CookFunctionalTests (2),
  EditorAssetWorkflowTests (37), and ContentBrowserWorkflowTests (117 passed,
  2 skipped): 506 passing cases total. AssetPackageTests includes relocation,
  redirector repair, repeated recovery, explicit rollback and pending projection.
- The three new state-sensitive cases passed individually with `--isolate
  --test-jobs 1`: shared-writer isolation/occupied stages, committed finalization
  failure/recovery paths, and saved-metadata admission/reference preservation.
- Changed-document and all-plan validators passed. No coordinator symbols remain
  in active workspace source/tests. Public API consumers were checked across all
  three projects declared in `Durin.dworkspace`.
- The two ContentBrowser skips require unavailable Windows file/directory symlink
  privileges and do not exercise changed save behavior. GPU qualification and
  application-hosted tests were not selected: this change does not alter GPU or
  application-host behavior. `test affected --explain` also selected unrelated
  GPU integrations through shared Core/Engine dependencies; focused save, editor,
  cook and recovery targets above provide the task-specific runtime coverage.

The baseline was the completed [Package Persistence Layering](PackagePersistenceLayering.md)
work plus `51c0e690e`, `dde344e8b`, and `e4c546cb9`. At that baseline, ordinary
saves entered `SavePackagesAtomically` and a stateless Engine singleton forwarded
registry publication, metadata updates and recovery snapshots.

Lasting contracts are published in [Package Persistence](../../../Runtime/Core/PackagePersistence.md),
[Asset Catalog and Mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md), and
[Code Modules](../../../Workspace/CodeModules.md). DAST v10 and existing transaction,
revision, dependency identity, dirty-state and projection-pending boundaries are
preserved. File switching remains in-process rollback, not crash atomicity.

## Goal

Make the ordinary save flow directly readable as asset preparation, detached
capture, file staging/commit, `AssetsSaved`, and completion. Registry updates
should belong to AssetRegistry; Engine should own asset policy and recovery.
Remove redundant service objects and forwarding calls without creating a second
writer or weakening explicit transaction guarantees.

## Selected Architecture

| Owner | Responsibility |
| --- | --- |
| Core | `IPackageWriter`, its file-backed implementation, verified staging, replacement, backups and rollback primitives |
| CoreDObject | `FSavePackageContext`, generic capture, package format, detached persistence and package-only save APIs |
| AssetRegistry | `AssetsSaved`, catalog/reference updates, revision checks and registry-native results |
| Engine | Asset readiness, snapshot metadata construction, ordinary save orchestration, explicit transactions and disk recovery policy |
| AssetTools | Save selection, independent batch execution, aggregate results and editor notifications |

The ordinary asset path will be:

```text
Engine: prepare asset
  -> CoreDObject: capture using FSavePackageContext
  -> IPackageWriter: stage and commit detached package/bulk bytes
  -> AssetRegistry: AssetsSaved
  -> Save owner: finalize saved revision and result
```

### Saved Metadata and Registry Ownership

- Move `AssetsSaved` from the Engine coordinator to the existing AssetRegistry
  public API style. Do not introduce an `IAssetRegistry` facade solely to mirror UE.
- Accept owned, detached saved metadata and return `FAssetRegistryResult`.
  Keep Engine error conversion at Engine call boundaries.
- Retain the name `AssetsSaved`. Add a single-item `AssetSaved` convenience only
  if a migrated caller needs it; do not add unused symmetry.
- Use the metadata captured for the bytes that were committed, including the
  observed output file stamps. Do not inspect live objects to reconstruct the
  saved version, and do not rescan or reserialize the package on ordinary success.
- Preserve existing optimistic concurrency semantics during extraction. Stage 0
  must define the expected-revision/participant argument and Add/Replace rules.
  Moving the method must not silently replace the admission snapshot with current
  registry state or allow a stale writer to overwrite newer metadata.
- Ordinary updates must not require a complete global reference index or erase
  unrelated reference errors. AssetRegistry owns atomic catalog/reference updates.

### Package Writer and Save Context

- Introduce `IPackageWriter` in Core as the shared storage boundary for package
  and bulk output. Its inputs and results use Core-owned paths, byte buffers,
  file stamps and I/O diagnostics, without DObject, AssetRegistry or Engine types.
- Provide one file-backed implementation using the existing staging and file
  replacement primitives. Define Begin/write/stage/Commit operations and explicit
  finalization/rollback semantics in Stage 0. Do not build a second file writer.
- Separate content commit from backup finalization so an Engine transaction can
  still roll back after a registry failure. Report uncommitted, committed and
  recovery-required outcomes without turning the writer into a registry or
  multi-package business coordinator.
- Define per-save operation ownership independently of reusable writer
  configuration. Buffers must outlive admitted I/O; cancellation and destruction
  drain work before release. Stage 0 must specify whether writer instances can
  be shared and how per-package state is isolated.
- Introduce `FSavePackageContext` in CoreDObject to carry the writer, generic
  archive target and effective save settings. Adapt Engine cook settings at the
  boundary; the context must not depend on Engine enums or concrete asset types.
- Keep context and writer lifetimes explicit for asynchronous operations; do not
  retain references to caller-stack settings. Preserve current save overloads
  through a default context backed by the file writer, with one authoritative
  representation of effective options rather than conflicting copies.
- Both CoreDObject package-only saves and Engine asset saves use this writer
  boundary. Engine-prepared snapshots enter the detached path without recapture.
  Engine continues to choose registry-failure and multi-package rollback policy.
- Use UE names and responsibility boundaries while retaining Durin-specific
  completion and recovery requirements. Additional storage backends and a full
  `IAssetRegistry` facade are outside this implementation scope.

### Coordinator Removal and Save Composition

- Remove `FAssetPublicationCoordinator` and its singleton after migrating all
  consumers, including load construction, cooking, relocation, redirector repair
  and recovery. Remove unused coordinator parameters and stored references.
- Replace `PublishDelta` forwarding with direct registry calls and explicit
  Engine result conversion. Consolidate metadata wrappers into `AssetsSaved`
  where their admission policy is preserved at the appropriate caller.
- Use the existing registry snapshot types when replacing `CapturePreparedState`;
  retain a separate Engine state only if a consumer needs a distinct contract.
- Move `ReconcileProjection` behavior into a clearly named internal operation
  such as `RefreshSavedPackages`. Keep Engine mount/recovery decisions in Engine;
  move only generic registry scan/update work into AssetRegistry.
- Give ordinary single-package saves a direct orchestration path. Explicit
  multi-package transactions compose the same detached staging and commit
  mechanisms. Avoid duplicated capture, payload buffers, package pins or file I/O.
- Do not mechanically wrap `DPackage::SaveAsync` if that would recapture an
  Engine-prepared graph. Identify the shared detached operation boundary first.
- Preserve the public `SavePackagesAtomically` contract and its specialized
  callers. Keep its internal prepared-save handoff only if it remains necessary
  after ordinary saves have their own completion path.

## Behavior Boundaries

This work preserves DAST v10, Delta/Complete semantics, bulk placement and hashes,
package-only persistence without Engine, and current save option defaults.

Preserve these protections and outcomes:

- Live capture and completion remain on GameThread; workers handle detached data.
- Package identity, edit revision, destination conflicts and staged-file checks
  remain effective. Allowing old snapshots after further edits is outside scope.
- Hard dependencies retain the current identity/type/path/redirect checks;
  content-only changes and soft-target metadata changes remain non-blocking.
- A package's main and bulk files retain their existing in-process rollback
  guarantees. Do not claim multi-file crash atomicity.
- Ordinary registry-update failure after file commit retains
  `ContentCommittedProjectionPending` and affected-path fencing.
- Explicit registry-failure rollback and prepared graph publication retain their
  existing guarantees. Recovery failures retain diagnostics and recovery files.
- Dirty is cleared only for the saved revision; completion remains idempotent.
- Cancellation, abandonment, scheduler shutdown, pins and buffer ownership keep
  their current lifetime guarantees.
- Ordinary editor batches retain successful packages, continue after failures,
  and report partial persistence with correct once-only notifications and refresh.

No new save policies, broad API renaming, async edit semantics, checksum reduction,
file-format changes, or persistent crash-recovery system are included.

## Implementation Stages

### Stage 0: Freeze Contracts and Migration Boundaries

Dependencies: none. Outcome: a concrete API and consumer migration map.

- [x] Audit all coordinator methods, stored references and singleton uses across
  source/test roots of every project in `Durin.dworkspace`.
- [x] Define the AssetRegistry `AssetsSaved` signature, concurrency token,
  duplicate/path validation, empty-batch behavior and failure semantics.
- [x] Record the destination of each coordinator responsibility and identify
  admission checks that must remain with Engine callers.
- [x] Define `IPackageWriter` inputs/results, per-save state, Begin/write/stage/
  Commit/finalize/rollback transitions, and asynchronous ownership rules.
- [x] Define `FSavePackageContext`, default writer selection, generic target and
  settings adaptation, and compatibility with existing save entry points.
- [x] Map the shared writer/context boundary to package-only saves, ordinary
  asset saves and explicit transactions without repeated capture or payload copies.
- [x] Identify existing regression coverage and add only necessary missing cases.

Acceptance: every existing consumer and behavior has an assigned destination;
there are no unresolved ownership or concurrency decisions before code migration.

### Stage 1: Move AssetsSaved into AssetRegistry

Dependencies: Stage 0. Outcome: saved metadata updates have their natural owner.

- [x] Implement `AssetsSaved` using existing registry delta/state mechanisms.
- [x] Migrate saved metadata and admission callers, preserving their distinct
  policy checks and converting result types only at Engine boundaries.
- [x] Remove obsolete metadata forwarding methods once no consumers remain.
- [x] Validate saved additions/replacements, reference edges, duplicate/path
  rejection, stale updates, and unrelated registry-state preservation.

Acceptance: save notifications require no Engine types in AssetRegistry; normal
save, pending projection, and explicit rollback paths preserve their behavior.

### Stage 2: Remove the Stateless Publication Coordinator

Dependencies: Stage 1. Outcome: no coordinator object or injection remains.

- [x] Migrate snapshot capture and disk reconciliation consumers to their selected
  owners without changing recovery, removal or fencing semantics.
- [x] Remove `PublishDelta`, singleton accessors, unused constructor arguments,
  stored references, forward declarations and obsolete includes.
- [x] Consolidate redundant snapshot structures and remove obsolete source files
  only after checking other definitions and build ownership.
- [x] Validate relocation, redirector repair, disk reconciliation and cooking
  paths affected by the migration.

Acceptance: no references to `FAssetPublicationCoordinator` or its getter remain
in active source/tests; all migrated callers compile and their regression gates pass.

### Stage 3: Introduce Package Writer and Save Context

Dependencies: Stage 2. Outcome: all save paths have a shared detached storage boundary.

- [x] Implement Core `IPackageWriter` and the file-backed writer over existing
  verified staging/replacement primitives, including retained-backup rollback.
- [x] Implement CoreDObject `FSavePackageContext` and default-context compatibility
  for existing package save APIs; map effective options and targets once.
- [x] Migrate synchronous/asynchronous package-only persistence to the shared
  writer while preserving Engine-free operation and scheduler-independent sync saves.
- [x] Provide the detached entry path needed by Engine without recapturing graphs
  or adding full payload copies, and define operation/context lifetime ownership.
- [x] Validate storage failures, commit/finalize/rollback outcomes, cancellation,
  abandonment, draining and package-only round trips.

Acceptance: writer headers have no higher-layer dependencies, context has no
Engine dependencies, and existing package APIs use the default writer correctly.
Only one file staging/replacement implementation remains.

### Stage 4: Simplify Ordinary Save Orchestration

Dependencies: Stage 3. Outcome: ordinary saves have a direct, shared-mechanism path.

- [x] Route synchronous and asynchronous ordinary saves through preparation,
  context-based capture, writer staging/commit, `AssetsSaved` and finalization.
- [x] Keep explicit multi-package transactions over the same mechanisms;
  remove redundant ordinary-save handoff options and bookkeeping where possible.
- [x] Validate byte equivalence, revision/conflict rejection, paired-file rollback,
  pending projection, explicit registry rollback, cancellation and shutdown.
- [x] Validate editor partial batches, failed-save retry and completion notifications.

Acceptance: ordinary saves no longer depend on the multi-package coordinator as
their implementation entry point; there is one shared file mutation mechanism
and no duplicate capture or additional full payload copy from the refactor.

### Stage 5: Validate Integration and Publish Contracts

Dependencies: Stage 4. Outcome: validated migration with updated lasting contracts.

- [x] Run focused Core writer, CoreDObject persistence, registry, package,
  editor operation, texture save and recovery
  tests selected according to the [testing workflow](../../../Agents/Testing.md).
- [x] Run new state-sensitive cases in isolation and record results and omissions.
- [x] Complete an `all` build including Engine, Sandbox and RoadWeaver according
  to the [build workflow](../../../Agents/BuildAndRun.md).
- [x] Update module ownership and the package persistence / asset mutation
  contracts; remove obsolete descriptions and compatibility scaffolding.
- [x] Validate changed documents and all plans according to the
  [documentation workflow](../../../Agents/Documentation.md), then record completion evidence.

Acceptance: all required gates pass for the final code; remaining caveats are
explicit, and the plan is marked completed only after implementation and validation.

## Frozen Migration Contracts

- `AssetsSaved(std::vector<FAssetData>, const FAssetRegistryPublication&)`
  returns `FAssetRegistryResult`. The supplied revision is checked under the
  registry mutex; Add/Replace is selected from that same admission snapshot.
  Empty batches succeed without publication, invalid/duplicate logical paths
  fail before mutation, and delta validation remains authoritative for metadata.
  Unrelated reference errors and completeness are preserved. Async Engine saves
  retain participant validation before capturing the commit-time publication,
  allowing unrelated updates and dependency content-only changes as before.
- Coordinator audit covers Engine, Sandbox and RoadWeaver source/test roots.
  Save and catalog admission move to `AssetsSaved`; catalog admission retains
  its complete-index check. Removal calls `PublishAssetRegistryDelta` directly.
  Relocation and redirector fixup use `FAssetRegistryPublication` snapshots.
  Their reconciliation and mutation-journal recovery call internal Engine
  `RefreshSavedPackages`, retaining mount resolution and missing-file policy.
  Loader, mutation service, cooking and package inspection lose unused injection.
  Existing class-aware resolution and cook reachability code stays in Engine.
- Core `IPackageWriter` is reusable, stateless configuration; `Begin` creates an
  exclusively owned operation with detached file descriptors and moved buffers.
  Each descriptor specifies destination, expected stamp, staging/backup paths
  and replacement or removal. `Stage` writes and verifies bytes; `Commit` checks
  destination/stage integrity and switches files in descriptor order. Backups
  survive until `Finalize`; reverse-order `Rollback` restores previous files.
  Results distinguish uncommitted, committed and recovery-required states and
  retain recovery paths. Destruction abandons stages or rolls back unfinished
  commits. The enclosing save operation drains its worker before destruction;
  writer operations have no live objects, registry policy or scheduler dependency.
- `FSavePackageContext` owns effective `FPackageSaveOptions` and a shared writer.
  Existing overloads construct a default file-backed context. Capture consumes
  its generic target/options; Engine adapts cook settings once. Detached Engine
  buffers enter the writer without object recapture or full buffer copies.
  Package-only completion keeps its existing pin/revision/dirty-state policy.
  Ordinary Engine saves own their completion; explicit bundles retain ordered
  commits, graph publication and registry-failure rollback using writer operations.
- Existing PackagePersistenceTests, PackageRegistryContractTests, PackageTests,
  editor operation and texture save tests cover revision conflicts, failures,
  pending projection, rollback and shutdown. Add direct saved-metadata tests and
  writer state/ownership coverage where the new boundary is not exercised by
  those regressions; run state-sensitive additions alone as well as in target runs.

## UE References

- [IAssetRegistry](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/AssetRegistry/IAssetRegistry)
  owns `AssetsSaved` for updating saved asset metadata.
- [IPackageWriter](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/IPackageWriter)
  separates package storage and Begin/Commit operations.
- [FSavePackageContext](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FSavePackageContext)
  carries writer and save settings.

These references guide naming and responsibility boundaries. They do not establish
that UE has identical concurrency, rollback or recovery contracts to Durin.
