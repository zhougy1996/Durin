# Asset Mutation Transaction Simplification Plan

Summary: Replace global compensating asset transactions with bounded artifact publication, forward-recoverable authored jobs, irreversible version-control-backed deletion, and rebuildable Registry reconciliation.

Last reviewed: 2026-09-01

Status: Active
Completed:

## Current Status

Stage 0 captured the pre-cutover surface and froze the shared structured result
contract. Production source contains 24 `FAssetMutationTransaction` references,
25 `FAssetDeletionTransaction` references, 39 mutation transaction-state
references, 60 compensation references, and nine whole-Registry prepared-state
references. Deletion staging is rooted at `Saved/ContentBrowserUndo`. These are
the Stage 7 comparison baseline rather than accepted lasting architecture.

The cutover now changes editor behavior: authored relocation, Fix Up, and deletion
no longer enter the global object-edit Undo/Redo history. Relocation is a
forward-recoverable command, Fix Up is a resumable canonicalization job, and
deletion is permanent and restored only through version control. Crash safety remains mandatory
for authoritative package artifacts. Ordinary object and property edits remain
owned by the editor transactor.

Stages 1-6 are implemented. Production source now contains zero references to
`FAssetMutationTransaction`, `FAssetDeletionTransaction`,
`EAssetMutationTransactionState`, `FAssetDeletionPhysicalTransition`,
whole-Registry `PreAssets`/`PostAssets`, or `Saved/ContentBrowserUndo`.
Relocation and Fix Up use forward-only jobs, deletion has no reverse callback or
local recovery copy, and Registry-only lag fences affected paths until a
successful delta reconciliation or complete refresh. A post-cutover cleanup
removed the remaining unused editor-transactor request injection, Undo/Redo
notification phases, copyable deletion-operation aliases, generic job-summary
duplication, and string-prefix result classification. `FAssetResult` now carries
forward-pending, projection-pending, and recovery-required disposition plus
available durable recovery metadata directly. Result headers retain only their
shared data contracts and trivial state checks; diagnostic code mapping and
construction are module-owned implementations rather than public inline helpers.
The redundant public `Durin::Asset` namespace is also removed: AssetRegistry and
Engine asset contracts now follow the repository convention of domain-prefixed
symbols directly under `Durin`, while shared internals use `Durin::AssetPrivate`.
Stage 7 retains two
explicit qualification items: reconstructing jobs from durable records after
process restart at every transition, and a complete before/after project Cook.

## Goal

Make asset-operation correctness follow the authority of each state domain
instead of coordinating files, Catalog, references, residency, GC, editor
history, and Cook output through one reversible state machine.

The completed architecture must provide:

- one explicit authored package-artifact closure for `.dasset`, its optional
  `.dbulk`, and registered owned payloads;
- bounded atomic publication for a package closure or Save bundle;
- forward-only, idempotent relocation with destination-first and redirector-last
  ordering;
- resumable Fix Up that retains redirectors until every required rewrite is
  verified;
- history-independent destructive deletion whose recovery belongs to version control;
- Catalog and reference projections that reconcile from committed authored
  content without forcing that content to roll back;
- residency invalidation or finalization after authored commit rather than GC
  participation in file compensation; and
- operation results that distinguish rejection, no-change failure, committed
  content awaiting projection, and authoritative recovery requirements.

## Scope

This plan changes Engine authored package publication, relocation, redirector
Fix Up, deletion, AssetRegistry projection publication, AssetTools orchestration,
Content Browser deletion and history integration, and the focused native tests
for those paths.

It also consolidates single-package and bundle Save behind the same package
artifact publisher. Load remains a read-side residency session, and Cook remains
an independent build-output transaction; both boundaries are verified so the
new authored mutation abstractions do not absorb them.

## Non-Goals

- Do not change DAST v9 encoding, the stable `.dasset`/`.dbulk` sibling layout,
  package identity, mount syntax, or current content corpus.
- Do not introduce content-addressed authored bulk generations in this plan.
- Do not redesign Cook output layout, Cook manifest grammar, incremental Cook
  identity, or its manifest-last rollback protocol.
- Do not persist ordinary editor object history or add cross-session replay.
- Do not retain a local deletion quarantine or provide deletion Restore/Undo;
  recovery of committed authored content belongs to version control.
- Do not weaken deletion blockers for dirty/loading packages, external hard
  referencers, incomplete alias closure, ambiguous companion ownership,
  read-only mounts, or reparse points.
- Do not make AssetRegistry, resident objects, DDC values, or Cook outputs an
  authored source of truth.

## Selected Architecture

### State-domain authority

| Domain | Classification | Required guarantee |
| --- | --- | --- |
| `.dasset`, optional `.dbulk`, owned authored payloads | Authoritative package artifacts | Atomic closure publication or durable forward recovery |
| Persistent external reference stores | Independent authored authority | Idempotent participant rewrite or explicit blocker |
| Catalog and package-reference index | Rebuildable projection | Revisioned delta plus bounded reconcile |
| Resident `DPackage`, load state, soft caches, GC reachability | Runtime projection/cache | Scope fence, invalidate, relocate, tombstone, or reload |
| Dirty and NewlyCreated | Editor package state | Clear only after authored commit |
| Editor Undo/Redo | Transient object-edit history | No authored file-operation records after cutover |
| Deleted authored content | Version-control responsibility | Permanent local removal after confirmation; restore through source control |
| Cook and DDC outputs | Reproducible build products | Existing domain-owned publication contracts |

### Guarantee classes

- **Read session:** Load and inspection use unpublished candidates plus RAII
  cleanup; they create no mutation journal.
- **Artifact publication:** Save and Duplicate publish one exact artifact
  closure and update package/editor state only after success.
- **Forward-recoverable job:** Relocation and Fix Up persist enough
  data to retry or resume toward one selected outcome; they do not restore every
  prior runtime projection on ordinary failure.
- **Destructive command:** Delete revalidates the complete selected closure and
  then permanently removes it. Partial I/O failure remains forward-only and
  fences stale Registry paths; no local pre-image or Restore protocol exists.
- **Compensating transaction:** Retained only for a future participant that has
  no redirect, versioning, quarantine, or idempotent forward protocol and whose
  product contract requires all-or-nothing authored visibility.

### Commit and recovery rules

1. Materialize and validate every new authoritative byte image before changing
   a mounted authored path.
2. Persist the operation identity, participants, expected fingerprints, and
   desired forward outcome before the first authoritative publication.
3. Publish in an order whose prefixes remain safe and make every step
   idempotent.
4. After the first committed authoritative step, recovery continues forward;
   it does not choose an unrecorded rollback direction.
5. Publish or reconcile Catalog/reference deltas after authored content. A
   projection failure produces `ProjectionPending`, keeps affected paths fenced,
   and never changes the durable job into an authored rollback.
6. Apply resident rename, tombstone, invalidation, Dirty/NewlyCreated clearing,
   notifications, and GC finalization only at their documented post-content
   boundary.
7. `RecoveryRequired` is reserved for uncertain authoritative artifacts or an
   independent persistent store. It carries an operation id, desired direction,
   failed participant, and recovery location; projection lag has a separate
   terminal state.

## Implementation Stages

### Stage 0: Freeze the semantic cutover and baseline failure contracts

- [x] Inventory every current operation state, failure-injection seam, staged
  path, Registry snapshot, resident mutation, editor custom change, and recovery
  root used by Save, relocation, Fix Up, and deletion.
- [x] Add a checked-in test matrix that maps each current failure seam to the
  target guarantee: no change, forward-resumable, projection pending, or
  authoritative recovery required.
- [x] Record the UI cutover in focused tests: relocation and Fix Up do not add
  editor history; Delete is explicitly permanent and adds no history; ordinary
  reflected/object transactions remain unchanged.
- [x] Define stable structured operation and recovery results before replacing
  existing enums. Include operation id, affected paths, content state,
  projection state, failed participant, and recovery location.
- [x] Capture a baseline of operation-specific transaction code and failure
  tests so the final stage can demonstrate removed protocol surface rather than
  only renamed abstractions.

Stage 0 is complete when the target behavior is executable as failing tests and
no later stage needs to decide whether a domain is authoritative, projected, or
transient.

#### Checked-in failure matrix

| Operation boundary | Current seam | Target guarantee |
| --- | --- | --- |
| Save materialization/directory/staging | `CreateDirectories`, `StagePackage`, companion preparation | No authored or projection change; package remains Dirty |
| Save payload/package publication | `PublishCompanion`, `PublishPackage`, `PublishRootPackage` | Bounded artifact publication; durable forward recovery only if visibility is uncertain |
| Save Registry publication | `PublishRegistry` | Content committed, projection pending, path fenced |
| Relocation preparation | `PrepareOutput`, `StageOriginal` | No change rejection |
| Relocation destination/payload publication | `PublishRealAsset`, `PublishOwnedPayload` | Forward-resumable; old path remains valid until redirector publication |
| Relocation redirector publication | `PublishRedirector` | Forward-resumable; destination already valid |
| Relocation Registry/residency | `PublishRegistry`, `UpdateLoadedPackage` | Projection pending; never authoritative recovery |
| Fix Up preparation | `PreparePackage`, `PrepareStore`, `StageOriginal` | No change rejection |
| Fix Up participant rewrite | `PublishPackage`, `ApplyStore`, `Verify` | Prior verified participants remain committed; redirector retained |
| Fix Up redirector deletion | `DeleteRedirector` | Forward-resumable; deletion only after zero exact incoming occurrences |
| Fix Up Registry publication | `PublishRegistry` | Projection pending; redirector compatibility retained until reconcile |
| Destructive Delete | Content Browser remove hook | Permanent forward progress; partial failure is retryable and never compensated |
| Delete Registry/residency/GC | Engine deletion transition | Projection pending for Registry; residency and GC are post-content finalizers |

### Stage 1: Separate Registry delta publication from authored commit

Depends on Stage 0.

- [x] Add an `FAssetRegistryDelta` value for deterministic Add, Replace, Remove,
  and reference-invalidation sets against an expected revision.
- [x] Move final Catalog/reference derivation and validation into the Engine
  publication boundary; operation states stop editing package-reference edges
  directly.
- [x] Add path-scoped projection fences and a reconciler that can rebuild the
  affected entries from committed package artifacts.
- [x] Extend AssetTools results with distinct `ContentCommittedProjectionPending`
  and `RecoveryRequired` outcomes; presentation must not report either as a
  no-change rejection.
- [x] Prove that an injected Registry failure after content commit leaves valid
  authoritative bytes, blocks stale resolution for the affected paths, and
  converges through one retryable reconcile.
- [x] Preserve expected-revision rejection before authored publication; do not
  silently rebase a changed read set in this stage.

Stage 1 is complete when no successful authored commit must reverse package
bytes solely because the rebuildable Registry projection rejected publication.

### Stage 2: Establish one package-artifact closure publisher

Depends on Stage 1.

- [x] Introduce an internal package-artifact-set value that owns the main
  package, optional validated raw bulk segment, and contributed owned payload
  participants without suffix guessing.
- [x] Extract staging, fingerprinting, ordered replace/delete, verification,
  durable progress, and exact cleanup from the relocation-named journal helpers
  into a bounded artifact publisher.
- [x] Make the main package and every referenced authored payload one publication
  closure; publish payloads before the package image that binds them.
- [x] Route single-package Save and `SavePackagesAtomically` through the same
  publisher. Single Save becomes a one-element bundle rather than a separate
  rollback implementation.
- [x] Clear Dirty and NewlyCreated and publish saved metadata only after closure
  commit; an earlier failure must leave the resident package retryably Dirty.
- [x] Keep Cook's `ICookOutputStore` independent. Sharing byte validation or a
  low-level atomic-file helper must not make Cook an authored mutation client.

Stage 2 is complete when Save has one closure publication protocol, authored
companions cannot tear from their binding package, and Cook retains its existing
manifest-owned transaction.

### Stage 3: Convert redirector Fix Up into a resumable job

Depends on Stages 1 and 2.

- [x] Replace the opaque Fix Up transaction with an immutable plan plus durable
  per-participant progress: pending, rewritten, verified, failed, and alias
  pending deletion.
- [x] Rewrite and save each referring package independently through the Stage 2
  publisher. A committed canonical rewrite is never compensated because a later
  participant failed.
- [x] Require persistent external reference stores to provide idempotent rewrite
  and verification. Reject Fix Up before mutation when a required provider
  cannot satisfy the resumable contract.
- [x] Keep live soft-reference changes as post-content cache updates; invalidate
  them when direct rewrite cannot be made safely.
- [x] Reinspect package and external-store occurrences after each pass. Delete
  redirectors only when the complete selected scope has zero remaining exact
  incoming occurrences and the reference projection is complete.
- [x] Return partial progress and failed paths without `RecoveryRequired` while
  every redirector required for compatibility remains valid.
- [x] Remove Fix Up Commit/Undo/Redo integration from AssetTools and the editor
  transactor.

Stage 3 is complete when an injected failure at every participant boundary
leaves prior successful rewrites committed, every unresolved old path usable
through a redirector, and a later invocation resumes only remaining work.

### Stage 4: Convert relocation into a forward-only authored job

Depends on Stages 1 and 2.

- [x] Narrow relocation planning to destination real artifacts, source
  redirectors, owned payload closure, relevant occupancy/fingerprint facts, and
  resident finalization. Upstream alias compression moves exclusively to Fix Up.
- [x] Publish every destination artifact first and its source redirector last.
  Persist progress before and after each authoritative step.
- [x] Make crash recovery and ordinary retry use one idempotent `ResumeForward`
  path. Remove reverse file compensation and Pre/Post whole-Registry snapshots.
- [x] Treat a clean resident package as a post-content projection: relocate it
  in place when safe, otherwise invalidate/unload it so the next access loads
  the committed destination. Dirty or loading participants remain blockers.
- [x] Publish move observers only after destination, redirector, and projection
  visibility are complete; a pending projection emits no committed observer.
- [x] Remove relocation Commit/Undo/Redo integration from the editor transactor.

Stage 4 is complete when every durable prefix of relocation is safe, recovery
has only the forward direction, the old path never resolves before its
destination exists, and unrelated aliases are untouched.

### Stage 5: Replace recursive deletion Undo with irreversible deletion

Depends on Stages 1 and 2. Stage 4 is not required.

- [x] Retain Content Browser's immutable recursive preflight, deterministic
  fingerprints, companion ownership, source-control
  policy, warnings, and confirmation revalidation.
- [x] Remove same-volume staging as a requirement because Delete no longer
  renames content into an Engine-owned recovery area.
- [x] Make Delete permanently remove maximal roots, remove/reconcile Registry
  entries, tombstone resident packages, and finalize GC after content commit.
- [x] Make the confirmation explicitly state that deletion is irreversible and
  recovery is provided by version control rather than the editor.
- [x] Remove `FAssetDeletionPhysicalTransition`, Engine deletion Undo/Redo, and
  the Content Browser custom transaction. A successful Delete publishes one
  mounted-content mutation revision.
- [x] Prove that partial physical failure never restores an already removed
  root, fences stale Registry paths, and can be retried toward deletion.

Stage 5 is complete when deletion is independent of editor history, no local
recovery artifacts or reverse callbacks exist, and GC/residency are absent from
physical rollback.

### Stage 6: Remove the superseded generic transaction surface

Depends on Stages 3 through 5.

- [x] Remove `FAssetMutationTransaction`, `FAssetDeletionTransaction`,
  `EAssetMutationTransactionState`, operation-specific Undo/Redo result phases,
  and editor mutation custom-change adapters after their last callers migrate.
- [x] Replace operation-specific compensation failure enums with artifact
  participant and job-progress failure seams that correspond to actual durable
  boundaries.
- [x] Keep one generic recovery descriptor only for authoritative artifacts;
  keep Fix Up progress domain-specific and keep deletion free of local recovery
  records.
- [x] Verify module retirement cannot leave an executable callback in a durable
  record. Persist data descriptors and reacquire owner-gated providers by stable
  registration identity when recovery needs them.
- [x] Confirm Load, Registry scanning, DDC, Cook, ordinary object transactions,
  and GC expose no dependency on the removed mutation transaction types.
- [x] Remove unused editor-transactor request plumbing, notification phases,
  dead shadow outcome types, and copyable destructive-operation handles.
- [x] Keep shared Asset result headers dependency-light by moving diagnostic
  mapping and construction out of public inline API.
- [x] Flatten the redundant public Asset namespace without retaining aliases;
  qualify broad entry points and keep shared internals in `AssetPrivate`.

Stage 6 is complete when repository searches find no superseded transaction
types or asset-file custom changes and the remaining abstractions correspond to
artifact publication, forward jobs, projection reconciliation, or editor object
history without overlapping ownership.

### Stage 7: Qualify the cutover and publish lasting contracts

Depends on all prior stages.

- [x] Run the complete failure matrix for Save, relocation, Fix Up, Delete,
  projection reconcile, startup recovery, and shutdown/module
  retirement.
- [ ] Add restart fixtures at every durable progress transition and prove that
  replay is idempotent across a second interruption.
- [ ] Verify a complete project Cook before and after the refactor produces the
  same manifest semantics and never enters authored recovery state.
- [x] Verify ordinary Load, unload, object/property Undo/Redo, Dirty package
  prompts, mounted-content revision acknowledgement, and manual Registry refresh
  retain their documented behavior unless explicitly changed above.
- [x] Compare the Stage 0 baseline and record removed state machines,
  compensation paths, durable record kinds, and operation-owned Registry copies.
- [x] Update the authoritative asset package, asset mutation, data lifecycle,
  Content Browser, and editor transaction documentation in the same final
  implementation stage; do not leave the plan as the lasting contract.

Stage 7 is complete when all acceptance gates pass, lasting documents own the
new behavior, and no required correctness property depends on the removed
global transaction semantics.

## Acceptance Gates

### Authority and publication

- [x] A stable authored state always contains a package image and exactly the
  bulk/payload closure declared by that image.
- [x] Save failure before authored commit leaves the prior disk/Catalog state
  and a retryably Dirty resident package.
- [x] Registry/reference projection can be rebuilt from committed authored
  artifacts and never forces an already committed valid closure to roll back.
- [x] `RecoveryRequired` is impossible for Registry-only lag, resident cache
  invalidation, GC finalization, DDC, or Cook output.

### Operation semantics

- [x] Relocation has one recovery direction and never publishes a redirector
  before its destination.
- [x] Fix Up partial success is durable, safe through retained redirectors, and
  resumable without rewriting completed participants.
- [x] Delete permanently removes only the revalidated maximal roots and retains
  no Engine-owned recovery copy; version control owns recovery.
- [x] Relocation, Fix Up, and Delete do not enter the global editor
  Undo/Redo history; ordinary editor object transactions remain one ordered
  history.

### Complexity removal

- [x] No authored operation stores complete before/after Catalog and reference
  snapshots solely for rollback.
- [x] No authored operation implements a private copy of generic file staging,
  fingerprint, ordered publication, or exact cleanup.
- [x] No relocation, Fix Up, or deletion implementation contains reverse-order
  compensation of already valid committed authored progress.
- [x] No durable recovery record depends on a process-local `std::function`, raw
  owner pointer, or editor history entry.

## Validation Strategy

Use the smallest focused native targets selected through the
[Agent Testing Workflow](../Agents/Testing.md), then apply the build coverage
required by the [Agent Build And Run Workflow](../Agents/BuildAndRun.md).
Expected focused coverage includes `AssetTests`, `AssetRegistryTests`, and
`EngineTests`; exact registered targets must be confirmed when each stage is
implemented.

Tests must cover deterministic failure before and after every authoritative
publication, durable progress write, Registry delta, reconcile attempt,
resident finalizer, and explicit purge boundary. Filesystem tests use isolated
writable mounts and exact marked roots. Restart tests must reconstruct jobs from
durable descriptors rather than retaining the original C++ state object.

## Related Code

- [Asset mutation public types](../../Engine/Source/Runtime/Engine/Public/Asset/MutationTypes.h)
- [Asset mutation job](../../Engine/Source/Runtime/Engine/Private/Asset/AssetMutationJob.cpp)
- [Asset mutation journal](../../Engine/Source/Runtime/Engine/Private/Asset/AssetMutationJournal.cpp)
- [Asset publication coordinator](../../Engine/Source/Runtime/Engine/Private/Asset/AssetPublicationCoordinator.cpp)
- [Package Save operations](../../Engine/Source/Runtime/Engine/Private/Asset/AssetPackageOperations.cpp)
- [Asset relocation](../../Engine/Source/Runtime/Engine/Private/Asset/AssetRelocation.cpp)
- [Redirector Fix Up](../../Engine/Source/Runtime/Engine/Private/Asset/AssetRedirectorFixup.cpp)
- [Asset deletion](../../Engine/Source/Runtime/Engine/Private/Asset/AssetDeletion.cpp)
- [Cook coordinator](../../Engine/Source/Runtime/Engine/Private/Asset/CookCoordinator.cpp)
- [AssetTools operations](../../Engine/Source/Editor/AssetTools/Private/AssetTools/AssetOperations.cpp)
- [Content Browser destructive deletion](../../Engine/Source/Editor/ContentBrowser/Private/Panels/ContentDeletionOperation.cpp)
- [Content Browser operations](../../Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserOperations.cpp)

## Related Documentation

- [Asset Catalog And Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Editor Transaction System Roadmap](../Roadmaps/EditorTransactionSystem.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
