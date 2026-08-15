# Asset Redirector Mutation Boundary Plan

Summary: Consolidate asset authoring mutations behind one transaction-owning service while preserving the completed redirector relocation and Fix Up contracts.

Last reviewed: 2026-08-15

Status: Archived
Completed: 2026-08-15

## Current Status

This is the completed M1 child plan of the
[Asset Architecture Simplification Roadmap](../../../Roadmaps/Archive/2026-08/AssetArchitectureSimplification.md).
Its M0 dependency and Stage 0 are complete. The production mutation caller
inventory, transaction ownership, callback contracts, and relocation,
deletion, Fix Up, and Undo/Redo failure matrix are frozen. Stages 1 and 2 are
complete: the UE-style resident-package model and opaque relocation transaction
are in production use, every characterization caller uses the same transaction,
and the legacy relocation token and phase functions have been removed. Stage 3
is also complete: strict Fix Up exposes an immutable summary and the same opaque
commit boundary, with all phase APIs removed. Stage 4 is complete: deletion,
contribution registration, create, and save now share the consolidated
authoring boundary. Stage 5 qualification and lasting-document publication are
complete; the roadmap has activated the dependency-ready M2 child plan.

This plan deliberately revises one M0 implementation choice after design
review. M0 separated unpublished drafts and loaded persistent packages into two
stores. M1 keeps the catalog/persistence boundary but replaces those stores
with one resident-package store whose entries carry explicit publication state.
The completed M0 plan remains historical evidence for its checkpoint; this plan
owns the superseding design and migration.

The selected direction keeps authored redirectors and all behavior established
by the completed catalog/load boundary. AssetCore owns package/catalog mutation
and its reversible journal. Editor integrations may contribute durable external
state or presentation-only observation through contracts with explicit failure
semantics, but callers cannot manually sequence AssetCore analyze, revalidate,
apply, projection, restore, or compensation phases.

## Outcome

- One resident-package store owns both newly created and published in-memory
  packages, with orthogonal publication and dirty state.
- One focused authoring surface owns create, save, move, deletion, and Fix Up.
- Each previewable mutation exposes an immutable summary and one executable
  transaction value; the transaction owns final revalidation, commit,
  compensation, undo, and redo.
- Relocation remains independent of referencer availability and publishes a
  direct redirector at every old authored path.
- Strict Fix Up remains the only bulk reference rewrite and the only operation
  allowed to delete a redirector after complete zero-incoming proof.
- Package rewrites, owned payload files, external reference stores, deletion
  staging, and transient observers have distinct contribution contracts.
- Internal transaction phases and journals are private; deterministic failure
  controls remain in `AssetTestSupport.h` only.
- Default unload preserves unsaved work; explicit discard policy is required to
  unload a newly created or dirty package.

## Scope

- AssetCore create, save, redirector creation, relocation, deletion, and Fix Up
  public APIs and their private runtime-state implementation.
- Resident package lookup, publication state, unload policy, create/save
  promotion, load rollback, shutdown, and test snapshot semantics.
- Relocation and deletion transaction integration in LevelEditor, including
  Content Browser folders and editor Undo/Redo.
- AssetImportCore reference-store participation and import-record rewrites.
- Engine-owned payload relocation and deletion companion contributions.
- Move observers, reference stores, contribution registration lifetime, and
  failure semantics.
- Focused AssetCore, editor workflow, import-record, Cook, and restart coverage.

## Non-Goals

- Removing persistent authored redirectors or eagerly rewriting referencers
  during relocation.
- Redesigning the DAST v4 codec, compatibility policy, import service, build
  service, or authored/cooked execution domain; later milestones own those.
- Adding source-control automation, remote transactions, consolidation, or a
  general editor job framework.
- Moving ordinary unmanaged-file and empty-folder operations into AssetCore.
- Preserving source compatibility for public phase functions, tokens, or
  callback forms that have no production owner after migration.

## Selected Architecture

### One public authoring calling style

Public callers use focused free functions and opaque value types. Create and
save remain direct operations with explicit results. Previewable operations
return immutable analysis data plus an opaque transaction that is either
committed once or retained for Undo/Redo. Public callers do not invoke separate
revalidate, unload, apply, registry-publication, restore, or rollback steps.

### Residency follows the UE package mental model

`FAssetRuntimeState` owns one path-keyed resident-package store. Each entry
contains the package and an explicit publication state:

- `NewlyCreated`: the package exists in memory but has no persistent catalog
  entry yet;
- `Published`: the package corresponds to persistent catalog truth, whether it
  was loaded from disk or promoted by save.

`DPackage::IsDirty()` remains independent: a newly created package is normally
dirty, while a published package may be clean or modified. Authoring code uses
resident lookup for either state. Exact catalog lookup and redirect resolution
continue to describe persistent content only. Load and soft resolution may
return an exact resident real package, including `NewlyCreated`, without file
I/O; absent residency, physical load remains catalog-authoritative and cannot
turn an unindexed file into persistent truth.

`UnloadPackage` rejects dirty or newly created state by default and accepts an
explicit discard-unsaved policy when the caller intentionally abandons work.
The separate draft lookup/store and `DiscardUnpublishedPackage` API are removed.
This matches UE's state-oriented package lifecycle without importing UE's
in-memory Asset Registry semantics into Durin's persistent catalog.

### Transactions own state advancement

An AssetCore transaction records the catalog revision, fingerprints, ordered
journal entries, reversible bytes/files, package residency changes, and
contributions needed by its operation. `Commit`, `Undo`, and `Redo` own state
validation and compensation. A failure reports whether state is restored or
recovery is required; it never leaves callers guessing which private phase ran.

### Contribution contracts match failure ownership

- Owned-payload relocators may contribute files and reversible live state owned
  exclusively by the moving asset.
- Persistent external reference stores capture deterministic snapshots and
  contribute fingerprint-bound reversible writes for strict Fix Up.
- Deletion contributors identify owned companion files but do not orchestrate
  registry or filesystem phases.
- Move observers receive committed direction changes only and cannot reject or
  roll back authored publication.

Registration uses lifetime-safe module ownership and rejects duplicate or stale
providers deterministically. Test-only substitution and failure injection do
not appear in production headers.

## Stage 0 Inventory

| Current surface | Production owner/callers | Destination |
| --- | --- | --- |
| Create/save | AssetCore; import providers, asset editors, level authoring, bake program | Focused direct authoring entries backed by the private authoring owner |
| Redirector creation | AssetCore relocation; direct construction otherwise appears only in characterization tests | Private relocation primitive plus focused test support |
| Relocation analyze/revalidate/apply/restore | `FEditorAssetMoveCoordinator`, `FAssetRelocationTransaction`, `GrayboxSceneAuthoring` | One opaque relocation transaction with commit/undo/redo |
| Fix Up analyze/revalidate/apply | AssetCore facade; direct phase callers are tests | One opaque Fix Up preview/transaction; tests use the public transaction behavior or test support |
| Fix Up facade | Content Browser and `DurinEd` console command | Retained as one-shot authoring entry backed by the transaction owner |
| Deletion analyze/revalidate/unload/projection/restore | Content Browser plan/transaction | One AssetCore deletion contribution retained by the editor filesystem transaction |
| Direct single-asset deletion | Graybox cleanup and tests | Focused one-shot deletion entry; cleanup-only draft removal stays in package authoring support |
| Reference stores | AssetImportCore import records and LevelEditor project defaults | Lifetime-gated persistent Fix Up contributor |
| Owned payload relocation/deletion companions | Engine asset services | Class-owned reversible payload and companion-description contracts |
| Move observers | LevelEditor viewport/session state | Committed-only transient observer with no rollback authority |

| Durable state | Current write owner | Required transaction owner |
| --- | --- | --- |
| Real package and redirector bytes | AssetCore relocation/Fix Up journals | AssetCore opaque transaction |
| Catalog revision and reference index | AssetCore catalog store | AssetCore opaque transaction, published atomically |
| Loaded package identity/residency | AssetCore package store | AssetCore opaque transaction |
| Owned payload files/live state | Class relocator invoked by AssetCore | AssetCore journaled contribution |
| External import/project records | Registered reference store | Fingerprint-bound Fix Up contribution invoked by AssetCore |
| Content folders/unmanaged files | LevelEditor content deletion/rename | Editor filesystem transaction with one AssetCore contribution |
| Viewport/session/cache observation | Move observers | Post-commit notification only |

The frozen relocation matrix covers single/folder/batch moves, loaded and
unloaded packages, stale and read-only unrelated referencers, real and alias
collisions, move-back, repeated direct-alias compression, owned payloads, one
catalog revision, publication failure at every retained seam, compensation,
restart, and Cook. The Fix Up matrix covers rewrite-only and delete modes,
package hard/soft fields, import/project external stores, incomplete indexes,
unavailable providers, stale fingerprints, verification/publication failure,
and restoration of stores, packages, and aliases. The deletion matrix covers
recursive/filter-independent selection, read-only mounts, asset/alias closure,
companion ambiguity, persistent references, loaded eviction, byte-identity and
destination conflicts, staging failure compensation, Undo/Redo, and retained
recovery roots.

## Stage 0: Freeze Mutation Inventory And Failure Contracts

Dependencies: M0 complete.

- [x] Inventory every production and test caller of create, save, redirector
  creation, relocation phases, Fix Up phases, deletion phases, contribution
  registration, and move observation; assign a destination or deletion.
- [x] Record which layer owns every physical write, catalog publication,
  residency change, external-store rewrite, notification, and Undo/Redo record.
- [x] Characterize single, folder, and batch relocation, move-back, repeated
  move compression, collision, stale revision, read-only source, and every
  existing failure-injection boundary.
- [x] Characterize Fix Up rewrite-only and rewrite-and-delete across complete
  and incomplete reference indexes, package and external occurrences,
  read-only/dirty/stale inputs, provider failure, and zero-incoming proof.
- [x] Characterize asset/folder deletion preflight, physical staging,
  compensation, resident publication/dirty state, target-plus-alias closure, companion
  ownership, Undo/Redo, and recovery-required behavior.
- [x] Record create/save/import publication behavior so consolidation does not
  blur resident publication state, persistent catalog truth, or atomic bundle
  publication.

### Acceptance Gate

- Every manual phase caller has one named transaction-owned replacement.
- The baseline matrix proves relocation remains independent of arbitrary
  referencers and strict Fix Up remains fail-closed.
- No operation begins implementation with unresolved ownership of a durable
  write or its compensation.

Stage 0 baseline validation used the `windows-msvc-x64` profile and
`Win64-Debug-DurinEditor` preset. `AssetPackageTests` passed 97/97,
`AssetImportCoreTests` passed 27/27, and `EditorAssetWorkflowTests` passed 80
cases with its existing Windows directory-symlink privilege skip. The baseline
test names and the ownership tables above bind every manual production phase
caller to its Stage 1-4 destination.

## Stage 1: Unify Residency And Introduce The Transaction Boundary

Dependencies: Stage 0 complete.

- [x] Replace `LoadedPackages` plus `DraftPackages` with one resident entry map
  carrying `NewlyCreated`/`Published` state and package identity.
- [x] Replace loaded/draft lookup pairs with one resident lookup; keep ordinary
  load catalog-authoritative and make save promote state in place.
- [x] Make `UnloadPackage` reject newly created or dirty packages by default and
  accept an explicit discard-unsaved policy; remove
  `DiscardUnpublishedPackage` and the private draft-discard path.
- [x] Migrate create/save/import rollback, load rollback, snapshots, release,
  relocation, deletion, shutdown, and tests to the unified store.
- [x] Add characterization for newly-created clean/dirty, published
  clean/dirty, default unload rejection, explicit discard, save promotion, and
  catalog-miss behavior while a newly created package is resident.
- [x] Add opaque mutation summary and transaction values with explicit state,
  operation kind, immutable scope, result details, `Commit`, `Undo`, and `Redo`.
- [x] Move revision/fingerprint revalidation, journal advancement,
  compensation, and recovery-required classification behind the transaction.
- [x] Keep create/save direct but route their resident-state promotion and catalog
  publication through the same private authoring owner.
- [x] Quarantine legacy phase tokens for characterization and staged Stage 2-4
  migration; all production relocation consumers use the opaque transaction,
  and the later owning stages remove their respective legacy declarations.
- [x] Add deterministic state-machine tests for double commit, undo-before-
  commit, redo-before-undo, stale inputs, failed compensation, and retained
  recovery evidence.

### Acceptance Gate

- Production code cannot advance an AssetCore mutation through internal phases.
- Every successful mutation publishes one catalog revision or a documented
  no-op; every failed mutation reports restored versus recovery-required state.
- Newly created and published packages share one resident store, while the
  persistent catalog remains authoritative and independent.
- Default unload cannot lose unsaved work and explicit discard has focused
  coverage at every rollback caller.

The residency checkpoint passed `AssetPackageTests` (98/98), the affected
import, editor workflow, texture, static-mesh, sky-box, editor-rendering,
material Vulkan, and renderer-scene targets, followed by all 72 native test
targets through `DevTool.bat test all`. The checkpoint also searched production
and native-test sources for the retired draft store, split lookup, and
`DiscardUnpublishedPackage` surfaces with no remaining matches.

The transaction checkpoint passed the full repository build,
`AssetPackageTests` (99/99), `EditorAssetWorkflowTests` (80 passing cases plus
its existing Windows directory-symlink privilege skip), and all 72 native test
targets. A production-source search found no caller-managed relocation analyze,
revalidate, apply, or restore sequence outside AssetCore; the LevelEditor move
coordinator, editor history, and graybox publisher now retain the opaque
transaction and call only `Commit`, `Undo`, or `Redo`.

## Stage 2: Consolidate Relocation And Editor Undo/Redo

Dependencies: Stage 1 complete.

- [x] Replace analyze/revalidate/apply/restore relocation calls with one
  relocation preview/transaction entry and migrate all production callers.
- [x] Make the transaction own real-package publication, source redirector
  publication, upstream direct-alias compression, loaded identity changes,
  owned payloads, and compensation in one journal.
- [x] Adapt LevelEditor transactions to retain the opaque AssetCore transaction
  rather than sequencing AssetCore phases.
- [x] Preserve committed-only move observation and prove observer failure or
  module shutdown cannot reject durable publication.
- [x] Remove obsolete relocation tokens, public phases, helpers, and duplicate
  test wrappers after caller migration.

### Acceptance Gate

- Single, folder, and batch moves use one public relocation entry and one
  transaction state machine.
- Move, move-back, repeated move, collision, restart, Cook, payload, failure,
  compensation, Undo, and Redo behavior matches the Stage 0 matrix.
- Relocation neither scans nor rewrites arbitrary referencers.

Stage 2 passed `AssetPackageTests` (99/99) and all 72 native test targets. A
repository source/test search found no remaining
`FAssetRelocationBatchToken`, `AnalyzeAssetRelocationBatch`,
`RevalidateAssetRelocationBatch`, `ApplyAssetRelocationBatch`, or
`RestoreAssetRelocationBatch` symbol; only the completed historical redirector
plan records the superseded API. The retained private relocation state still
drives the existing byte journal, direct-alias compression, resident identity,
owned payload, one-revision publication, observer, compensation, and recovery
tests through the opaque transaction.

## Stage 3: Consolidate Strict Fix Up

Dependencies: Stage 1 complete; Stage 2 relocation behavior qualified.

- [x] Replace public analyze/revalidate/apply Fix Up phases with one immutable
  preview and transaction-owned commit entry.
- [x] Retain complete package-reference and external-store snapshots with
  fingerprint-bound contributions inside the transaction.
- [x] Make rewrite-only and rewrite-and-delete results expose rewritten,
  retained, deleted, skipped, and failed paths without mutable side channels.
- [x] Require final complete-index and zero-incoming proof immediately before
  redirector deletion; compensate every owned durable write on failure.
- [x] Migrate Content Browser and editor-startup callers, then remove obsolete
  Fix Up plans and phase functions.

### Acceptance Gate

- Fix Up is one public transaction entry and callers cannot delete aliases or
  invoke package/store rewrite phases separately.
- Incomplete indexes, unavailable stores, stale fingerprints, dirty/read-only
  packages, apply/verify failures, and new incoming references fail closed.
- Restart and Cook observe canonical real paths after success and intact
  redirectors after any restored failure.

Stage 3 passed the full repository build, `AssetPackageTests` (100/100),
`AssetImportCoreTests` (27/27), `EditorAssetWorkflowTests` (80 passing cases plus
its existing Windows directory-symlink privilege skip), and all 72 native test
targets. Source/test search found no public Fix Up plan or analyze/revalidate/
apply/one-shot AssetCore functions. Focused coverage verifies rewrite-only
retention, rewrite-and-delete removal, structured rewritten/retained/deleted/
failed path results, stale providers and inputs, complete-index proof, external
stores, reverse-order compensation, and recovery-required journals through the
opaque transaction.

## Stage 4: Consolidate Deletion, Contributions, Create, And Save

Dependencies: Stages 1-3 complete.

- [x] Replace AssetCore deletion phase calls with one immutable asset-deletion
  contribution retained by the owning editor content transaction.
- [x] Give the content transaction one AssetCore commit/undo/redo seam while it
  continues to own confirmed folder and unmanaged-file staging.
- [x] Preserve target-plus-alias closure, companion ownership, persistent
  reference blockers, loaded-package eviction, and recovery staging.
- [x] Normalize owned-payload, deletion-companion, external-reference-store,
  and move-observer registrations around lifetime-safe handles and remove
  unused callback forms.
- [x] Route all production create/save callers through the focused authoring
  surface and keep redirector creation private to relocation/test support.
- [x] Remove direct deletion, raw redirector creation, public orchestration
  hooks, and legacy phase declarations with no remaining production consumer.

### Acceptance Gate

- Content deletion owns one filesystem transaction and one opaque AssetCore
  contribution; neither layer sequences the other's private journal.
- Create/save/delete behavior preserves draft rollback, atomic publication,
  companion ownership, Undo/Redo, and catalog revision guarantees.
- Production headers expose no raw redirector constructor or manual mutation
  phase API.

The deletion-boundary checkpoint replaced the public batch token and its
revalidate/unload/projection/restore phases with one opaque deletion
transaction. AssetCore now owns final validation, resident eviction, catalog
publication, compensation ordering, and transaction state; the LevelEditor
content transaction supplies only reversible physical stage/restore actions and
continues to own folders and unmanaged files. `AssetPackageTests` passed
100/100 and `EditorAssetWorkflowTests` passed 80 cases with its existing
Windows directory-symlink privilege skip.

The contribution-registration checkpoint gives owned-payload relocators and
deletion contributors the same explicit handle, unregister, callback gate, and
retained module-resource lifetime model already used by reference stores and
move observers. Duplicate class providers are rejected instead of silently
replacing the active owner, and invocation fails closed after owner retirement.
The focused AssetCore and editor workflow suites remained green after migrating
Engine service and test registrations.

The authoring-surface checkpoint leaves create/save as the focused direct
publication entries backed by the resident-state/catalog owner, removes raw
redirector construction and immediate deletion from production headers, and
keeps both only as explicitly named test support. Graybox cleanup now prepares
the same deletion transaction as the Content Browser, provides an adjacent
reversible physical staging transition, commits catalog removal, and only then
disposes its staged bytes. Focused AssetCore and editor workflow suites remained
green after all production and test callers migrated.

Stage 4 completed with a full `Win64-Debug-DurinEditor` build after the focused
`AssetPackageTests` 100/100 and `EditorAssetWorkflowTests` run. Production and
native-test source searches found no legacy deletion token/phase symbol, no
production raw redirector constructor, and no production direct-deletion entry.

## Stage 5: Qualify And Publish The Mutation Boundary

Dependencies: Stages 0-4 complete.

- [x] Run focused AssetCore package, redirector, reference-store, deletion,
  import-record, Cook, and restart tests through the repository workflow.
- [x] Run affected LevelEditor Content Browser, folder move, deletion,
  transaction, project-default, and external-store tests.
- [x] Run all affected targets, complete native qualification, full build, and
  hidden-window editor smoke without concurrent build processes.
- [x] Search for retired phase names, raw redirector construction, duplicate
  mutation entries, public journals/tokens, and production failure hooks.
- [x] Update Asset Packages, Asset Data Lifecycle, Content Browser, and import
  architecture documents with the implemented transaction ownership.
- [x] Run changed-document, all-plan, all-roadmap, and repository documentation
  validation and record evidence for the M1 exit gate.

### Acceptance Gate

- The Stage 0 relocation, Fix Up, deletion, publication, restart, Cook, and
  Undo/Redo matrix passes through the consolidated boundary.
- Repository production code has one public entry for each mutation and no
  caller-managed AssetCore transaction phase.
- Lasting documentation owns the final mutation and contribution contracts,
  and the M2/M3 dependency handoff is explicit.

Stage 5 passed `AssetPackageTests` (100/100), `AssetImportCoreTests` (27/27),
`AssetCookTests` (13/13), and `EditorAssetWorkflowTests` (80 passing cases plus
its existing Windows directory-symlink privilege skip). All 72 native targets,
the full `Win64-Debug-DurinEditor` build, and an 8-second hidden-window editor
smoke passed. Retired-symbol searches found no public/manual mutation phases,
raw production redirector constructor, direct production deletion entry, or
production failure hook. The four lasting architecture documents and roadmap
now own the implemented boundary, and all documentation validators passed.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Relocation | Single/folder/batch, move-back, repeated compression, collision, stale/read-only, loaded/unloaded, payload, restart, Cook |
| Relocation failure | Every retained injection boundary restores authored bytes, catalog revision, residency, payloads, and direct aliases or reports recovery required |
| Fix Up | Rewrite-only/delete, complete/incomplete index, package/external occurrences, stale/dirty/read-only, apply/verify failure, zero incoming |
| Deletion | Asset/folder closure, alias target safety, companion ownership, staging, failure compensation, Undo/Redo, recovery retention |
| Publication | Create, save, failed save, import bundle, draft promotion, atomic catalog revision |
| Callbacks | Duplicate registration, owner shutdown, in-flight invocation, committed-only observation, deterministic order |
| Integration | Content Browser, project default reference store, import records, Cook, restart, editor transactions |
| Qualification | Focused tests, affected targets, complete native tests, full build, hidden-window smoke, documentation validators |

## Completion Criteria

- All six stages and their acceptance gates pass with evidence recorded here.
- One resident-package store represents newly created and published in-memory
  packages without a parallel draft container.
- One focused authoring surface owns create, save, move, delete, and Fix Up.
- Public callers cannot sequence AssetCore transaction internals.
- Redirectors retain the roadmap's exact lookup, relocation, strict Fix Up, and
  Cook invariants.
- M1 is marked completed and the roadmap activates only dependency-ready M2
  and/or M3 work with a new child plan.

## Related Documentation

- [Asset Architecture Simplification Roadmap](../../../Roadmaps/Archive/2026-08/AssetArchitectureSimplification.md)
- [Asset Catalog and Load Boundary Plan](AssetCatalogAndLoadBoundary.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Content Browser](../../../Editor/Architecture/ContentBrowser.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- [`AssetMutation.h`](../../../../Engine/Source/Runtime/AssetCore/Public/AssetMutation.h)
- [`AssetSystem.cpp`](../../../../Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp)
- [`EditorAssetMoveCoordinator.cpp`](../../../../Engine/Source/Editor/LevelEditor/Private/Assets/EditorAssetMoveCoordinator.cpp)
- [`AssetRelocationTransaction.cpp`](../../../../Engine/Source/Editor/LevelEditor/Private/Assets/AssetRelocationTransaction.cpp)
- [`ContentBrowserOperations.cpp`](../../../../Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.cpp)
- [`ContentDeletionTransaction.cpp`](../../../../Engine/Source/Editor/LevelEditor/Private/Panels/ContentDeletionTransaction.cpp)
- [`AssetImportCoreModule.cpp`](../../../../Engine/Source/Editor/AssetImportCore/Private/AssetImportCoreModule.cpp)
- [`PackageTests.cpp`](../../../../Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp)
- [`ContentBrowserModelTests.cpp`](../../../../Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp)
