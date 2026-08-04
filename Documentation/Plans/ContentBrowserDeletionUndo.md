# Content Browser Recursive Deletion and Undo Plan

Summary: Add reference-aware recursive Content Browser deletion with same-volume staged Undo/Redo and no visible recycle-bin surface.

Last reviewed: 2026-08-04

Status: Active
Completed:

## Current Status

- `FContentBrowserPanel::DrawDialogs` already owns a delete confirmation modal,
  but it reports only the selected item count and asset-level blockers.
- `FContentBrowserOperations::AnalyzeDeletion` analyzes selected assets only.
- `FContentBrowserOperations::DeleteEmptyFolder` rejects every non-empty folder,
  which forces users to delete descendants manually before deleting the folder.
- `FAssetManager::DeleteAsset` stages and rolls back one asset operation
  internally, but removes its recovery copies before returning; it does not
  expose an editor-level undo boundary.
- `FContentBrowserModel::ItemsSnapshot` contains recursive content while
  `GetItems()` is the immediate, filtered presentation. Folder deletion must
  use physical filesystem state as its discovery source and join the registry
  and unfiltered snapshot as metadata rather than trusting the visible
  projection.
- The model omits unregistered `.dasset` files: filesystem enumeration skips
  packages and then adds back only registered assets. Recursive analysis needs
  an explicit unknown-package blocker rather than silently omitting those
  bytes.
- `FEditorTransactionManager` already provides bounded global Undo/Redo,
  descriptions, failure events, and `Execute`. The editor notification
  overlay already turns transaction events into transient notifications with
  Undo/Redo actions. Content Browser deletion is not yet registered as a
  transaction.
- This plan selects an immutable deletion plan, a transaction executed through
  `FEditorTransactionManager::Execute`, and same-volume rename staging of
  deduplicated maximal roots. AssetCore contributes batch safety, companion,
  unload, and registry state; it does not own a second file-staging layer.

## Goal

- Let users delete a non-empty Content Browser folder in one confirmed action.
- Preflight the complete subtree and explain counts and blockers before any
  mutation occurs.
- Make a successful folder or multi-item deletion one reversible editor
  transaction, available through `Ctrl+Z` / `Ctrl+Y`, the global Edit commands,
  and the existing notification action.
- Keep the Content Browser free of a visible Recycle Bin concept while retaining
  safe, session-scoped recovery internally.

## Scope

- Recursive physical target discovery for folders, registered assets,
  ordinary files, asset-managed companion files, unknown package files, and
  nested folders.
- One immutable deletion plan shared by the confirmation modal and executor,
  with stale-plan and Undo/Redo conflict detection.
- Reference-aware deletion analysis that distinguishes references from assets
  inside the same deletion set from references outside it.
- Same-volume rename staging of deduplicated maximal directory/file roots, plus
  compensating rollback and retained recovery data when rollback cannot finish.
- An AssetCore batch token for companion ownership, loaded-package safety,
  registry removal, and registry restoration without duplicating physical I/O.
- Integration with `FEditorTransactionManager` and the existing editor
  notification overlay.
- A lifetime-safe content-mutation notification that refreshes the Content
  Browser after Delete, Undo, and Redo.
- Delete dialog copy, wording, summary counts, blocker presentation, and
  post-operation refresh/selection behavior.
- Focused model/operation/transaction tests, editor interaction validation, and
  lasting Content Browser architecture documentation.

## Non-Goals

- Adding a Recycle Bin item, folder browser, or new permanent-delete command to
  the Content Browser UI.
- Force-deleting externally referenced assets or adding Replace References as
  part of this feature; existing safety checks remain authoritative.
- Copying deletion payloads across volumes. The first implementation rejects a
  selected root that cannot be renamed into its configured staging root.
- Restoring loaded-package residency, in-memory object identity, open asset
  editor tabs, or unsaved package state during Undo. Dirty documents are
  resolved through existing document workflows before deletion becomes
  available.
- Recovering deletions across editor restarts or after an unclean process exit
  in the first implementation.
- Introducing new source-control provider UI or bypassing source-control
  ownership rules.
- Changing asset package formats, mount ownership, import-record semantics, or
  unrelated Content Browser operations.

## Design Decisions and Invariants

### User-visible behavior

- `Delete` on a folder means “delete this folder and all of its contents,”
  matching the documented Unreal Content Browser behavior.
- The existing delete modal remains the confirmation surface. For a folder it
  shows the folder name, asset/file/subfolder counts, and the first useful
  blocker details before enabling the destructive button.
- The destructive button remains `Delete` or `Delete Folder`; the UI never
  exposes the name “Recycle Bin.”
- A successful operation is committed through the global editor transaction
  manager. The existing notification overlay supplies a concise success
  message and an Undo action; global Edit > Undo/Redo and keyboard shortcuts
  remain the durable access path.

### Deletion set and safety

- The deletion set is computed from normalized absolute physical roots. The
  filesystem is authoritative for descendant existence; the registry and model
  supply asset identity and presentation metadata.
- The plan includes every descendant regardless of the current search, type
  filter, or hidden-content display setting. A `.dasset` file that cannot be
  reconciled with the registry is an unknown-package blocker, not an ordinary
  file and not an omitted entry.
- If both a parent and one of its descendants are selected, the parent owns the
  descendant. Physical staging uses the minimal set of maximal directory/file
  roots and moves each root only once.
- A reference from one asset in the deletion set to another asset in the same
  set is not a blocker. A reference from outside the set remains a blocker.
- AssetCore determines companion ownership. A companion contributed by an
  asset in the deletion set is included once even when it lies outside a
  selected directory. An uninspectable companion set, conflicting ownership,
  or a companion inside a selected root whose owner is outside the deletion set
  is a blocker.
- A loading asset is a blocker. Loaded packages are batch-unloaded only after
  persistent and loaded-package references outside the deletion set have been
  ruled out. Dirty packages or unresolved open documents are blockers until
  the existing document workflow resolves them.
- Mount roots, read-only/non-authoring mounts, paths outside their resolved
  mount, cross-volume staging, and directory symlinks, junctions, or other
  reparse-point traversal are rejected by the first implementation.
- Analysis is side-effect free and produces one immutable
  `FContentDeletionPlan`. The modal retains that exact plan. Execute validates
  its registry revision and physical fingerprints immediately before mutation;
  a stale plan is rejected and reanalyzed rather than silently changed.
- One operation is failure-atomic through an explicit compensation journal, not
  an operating-system or database transaction. On ordinary failure it reverses
  completed steps. If compensation itself fails, the transaction reports the
  original and staged paths, retains staging data, and does not report a
  completed deletion.
- Redo consumes the same manifest rather than re-enumerating the UI, but first
  revalidates fingerprints and external references. Undo requires every
  original destination to be free. A conflict causes no mutation and leaves the
  transaction at the current history head.

### Physical staging and AssetCore ownership

- Each operation receives a collision-safe identifier and an owned staging root
  at `FPaths::ProjectDir()/Saved/ContentBrowserUndo/<operation-id>`. The first
  implementation accepts only roots on the same volume as that staging root.
- Selected folders are renamed as whole maximal roots. Selected standalone
  assets/files and companion files outside those roots are renamed as
  individually numbered entries. This preserves hidden files, empty folders,
  unknown ordinary files, and directory structure without per-file copying.
- `FContentBrowserOperations` builds the plan and creates the reversible
  transaction. The transaction owns the physical move journal and manifest.
- AssetCore returns one batch state/token that captures asset identities,
  companion contributions, registry entries, and unload preconditions. It
  applies or restores the registry projection and package cache changes, but it
  does not stage the same files independently.
- The initial deletion is the transaction's first `Redo()` and is submitted
  through `FEditorTransactionManager::Execute`. `CommitApplied` is not used.
- Loaded-package residency is not part of the recoverable state. Undo restores
  bytes and registry visibility; packages load normally on future demand.

### Undo storage and ownership

- The reversible transaction owns a manifest containing normalized original
  and staged paths, root kinds, asset identities, companion ownership, registry
  revision, physical fingerprints, current applied/restored state, and
  compensation progress.
- The operation staging root contains an ownership marker and is never exposed
  as mounted Content Browser content. Cleanup accepts only the exact normalized
  operation root and never follows caller-provided descendants or links.
- The staging lifetime is tied to the editor transaction entry. It is retained
  while the entry is reachable from Undo or Redo and cleaned when the entry is
  evicted or the transaction manager is cleared. Cleanup failure is logged with
  the owned path and leaves that root in place. A transaction destroyed in the
  `RecoveryRequired` state after failed compensation deliberately skips cleanup
  so its reported staging path remains recoverable. Crash discovery and
  recovery are explicitly deferred.
- The generic transaction manager owns history only; it does not know Content
  Browser path rules, staging paths, or registry semantics.
- `MLevelEditor` injects the transaction execution boundary into the panel and
  owns a lifetime-safe content-mutation revision/event. Successful Delete,
  Undo, and Redo publish that event; panels refresh and repair selection without
  a transaction retaining a raw panel pointer.

## Current Foundations and Gaps

| Area | Existing foundation | Gap this plan closes |
| --- | --- | --- |
| Content model | Recursive `ItemsSnapshot`, mount containment, asset/file/companion classification | No physical-source deletion model, unknown-package blocker, or immutable stale-plan token |
| Asset safety | `AnalyzeAssetDeletion`, direct referencer checks, loaded/loading state, companion contributors | No batch analysis that excludes internal references, validates ownership, or captures reversible registry state |
| File mutation | Per-asset staging/rollback and ordinary filesystem operations | No maximal-root rename transaction or explicit compensation journal spanning assets, companions, files, and folders |
| Editor history | `IEditorTransaction`, `Execute`, bounded Undo/Redo, global descriptions | Content Browser deletion is not represented in history |
| User feedback | Delete modal and notification actions | No recursive summary, stale-plan retry, blocker list, post-delete Undo action, or Undo/Redo content refresh event |

## Implementation Stages

### Stage 0: Freeze the deletion and recovery contract

Dependencies: Existing Content Browser and editor transaction architecture.

- [ ] Confirm the same-volume, writable-mount boundary, hidden staging location,
  mount-root rejection, and no-reparse-traversal policy described above.
- [ ] Define `FContentDeletionPlan`, the AssetCore batch token, the transaction
  state machine, the compensation journal, and their ownership interfaces.
- [ ] Define the physical fingerprint and registry-revision checks used to
  reject stale Execute, conflicting Undo destinations, and modified Redo input.
- [ ] Define rollback-failure reporting, retained-staging behavior, cleanup on
  transaction eviction, and teardown ordering between history and editor
  services.
- [ ] Define the exact blocker categories and the modal summary wording for
  folders, mixed/nested selections, filtered views, unknown packages, dirty or
  loading packages, companion ambiguity, stale plans, and unsupported mounts.
- [ ] Record how source-controlled and read-only mounts are rejected without
  bypassing existing mount policy.

#### Acceptance Gate

- [ ] The supported roots, deletion set, reference/companion policy, staging
  lifetime, transaction owner, conflict checks, and compensation-failure state
  are unambiguous and reflected in interfaces before mutation code is changed.

### Stage 1: Build recursive deletion analysis

Dependencies: Stage 0; `FContentBrowserModel` snapshot and mount contracts.

- [ ] Add an operation-layer builder that normalizes selected roots, rejects
  unsupported roots, expands folders from physical filesystem state, and
  deduplicates nested selections into maximal staging roots.
- [ ] Join registry/model metadata by normalized physical path and classify
  registered assets, managed companions, ordinary files, directories, and
  unregistered/invalid package files without omitting hidden descendants.
- [ ] Add AssetCore batch analysis that filters internal referencers, retains
  external persistent or loaded-package blockers, verifies loading/dirty state,
  and contributes companions with unambiguous ownership.
- [ ] Capture the registry revision and deterministic physical fingerprints
  needed for stale-plan, Undo-destination, and Redo-modification checks.
- [ ] Keep analysis side-effect free and return one immutable plan retained by
  both the delete modal and transaction constructor.
- [ ] Add native tests for nested roots, hidden/filter-independent discovery,
  unknown packages, internal versus external references, companion ambiguity,
  unsupported mounts/reparse points, and stale fingerprints.

#### Acceptance Gate

- [ ] A non-empty folder produces a complete deterministic plan without
  changing the filesystem or asset registry.
- [ ] Every physical descendant is represented exactly once or produces a
  specific blocker; no package, companion, or external reference is silently
  ignored.
- [ ] The modal and executor can share the same immutable plan and can detect
  when it is no longer current.
- [ ] Focused Content Browser tests pass through the DurinDevTool test entry
  point described by the repository build guide.

### Stage 2: Implement a compensating reversible deletion transaction

Dependencies: Stage 1; AssetCore package/registry ownership; file I/O contract.

- [ ] Add the AssetCore batch token that verifies unload preconditions, captures
  registry entries, batch-unloads packages, removes registry projection, and
  restores registry projection without staging physical bytes.
- [ ] Add `FContentDeletionTransaction` with a collision-safe owned staging
  directory, immutable plan, explicit applied/restored state, and compensation
  journal.
- [ ] Implement first `Redo` through `FEditorTransactionManager::Execute`:
  revalidate the plan, batch-unload packages, rename maximal directory roots and
  numbered standalone entries into staging, and apply registry removal.
- [ ] Implement `Undo` by validating every original destination, reversing the
  root moves, and restoring registry entries without reloading packages.
- [ ] Implement later `Redo` from the same manifest after rechecking physical
  fingerprints, current external references, and AssetCore preconditions.
- [ ] Reverse completed steps on ordinary failure. If compensation fails, keep
  the staging root and report both paths and the incomplete step without
  advancing transaction history.
- [ ] Tie cleanup to transaction lifetime and validate the ownership marker and
  exact operation root before removing staged data.
- [ ] Add transaction tests for initial Execute, Undo/Redo, destination and
  modification conflicts, external-reference changes, mid-operation failure,
  compensation failure, multi-root ordering, eviction, and cleanup.

#### Acceptance Gate

- [ ] Deleting a folder, undoing it, and redoing it produces the same visible
  persisted content and registry state each time without restoring package
  residency.
- [ ] An ordinary failed batch restores all changed targets. A deliberately
  failed compensation retains recoverable staging data and reports its exact
  state instead of claiming success.
- [ ] No staged data appears in any mounted Content Browser view, including
  when hidden files are enabled.
- [ ] No per-asset delete routine independently stages bytes already owned by
  the Content Browser transaction.

### Stage 3: Integrate the modal, global Undo, and notifications

Dependencies: Stage 2; existing `DrawDialogs`, `FEditorTransactionManager`,
and notification overlay.

- [ ] Retain one immutable plan in the pending dialog state and replace the
  current empty-folder-only error path with its recursive summary and blocker
  presentation.
- [ ] Keep the destructive action disabled while blockers or analysis errors
  remain; show enough detail to identify the external referencer or invalid
  file without flooding the modal.
- [ ] Revalidate on confirmation. If the plan is stale, keep the dialog open,
  rebuild its analysis, and require confirmation of the updated scope.
- [ ] Execute the transaction through the injected `Execute` callback so the
  global Edit menu and keyboard commands expose `Delete Folder "..."` as the
  Undo description only after the first `Redo` succeeds.
- [ ] Publish an editor-owned content-mutation revision/event after successful
  Delete, Undo, and Redo. Refresh registry/model state, cancel obsolete
  thumbnails, and repair selection without storing a raw panel pointer in the
  transaction.
- [ ] Keep the current directory when it survives. If the directly deleted root
  contains it, navigate to the nearest surviving parent; global Undo/Redo
  refreshes without otherwise stealing Content Browser focus.
- [ ] Verify the notification overlay presents Undo after delete and Redo after
  Undo, with actions disabled when the transaction is no longer the history
  head.

#### Acceptance Gate

- [ ] The UI never asks the user to manually empty a folder.
- [ ] The normal flow contains no Recycle Bin control, but the same operation is
  reversible through the notification action and global Undo/Redo commands.
- [ ] External reference blockers remain visible and cannot be bypassed by the
  folder path.
- [ ] Stale confirmation state cannot execute a deletion set different from the
  one currently displayed to the user.
- [ ] Content Browser state refreshes after global or notification Undo/Redo
  without relying on panel lifetime assumptions.

### Stage 4: Validate the end-to-end workflow

Dependencies: Stages 1 through 3.

- [ ] Extend focused native coverage for operation analysis, asset/file
  transactions, registry refresh, conflict rejection, ordinary compensation,
  and retained staging after compensation failure.
- [ ] Run the editor interaction matrix in the validation table below,
  including Delete-key and context-menu entry points.
- [ ] Validate notification Undo, global Ctrl+Z/Ctrl+Y, and redo invalidation
  after a new edit, as well as refresh without unwanted navigation after global
  history commands.
- [ ] Run the repository's complete `all` build and required native test targets
  through `DevTool.bat`; record the results and verified editor executable in
  the implementation handoff.

#### Acceptance Gate

- [ ] All focused tests, end-to-end scenarios, and full-build checks pass with
  no unrelated working-tree changes.
- [ ] The operation remains safe when the folder contains ordinary files,
  hidden descendants, loaded assets, managed companions, nested folders, or an
  unsupported entry that must block the operation.

### Stage 5: Publish the lasting editor contract

Dependencies: Stage 4 evidence.

- [ ] Update `Documentation/Editor/Architecture/ContentBrowser.md` with the
  recursive delete, physical-source discovery, reference, companion, staging,
  conflict, compensation, and transaction invariants.
- [ ] Update `Documentation/Editor/Architecture/WorkspaceFramework.md` only if
  the transaction/notification ownership boundary changes the shared editor
  command contract.
- [ ] Record validation evidence in this plan, set `Status: Completed`, fill
  `Completed: YYYY-MM-DD`, and run the all-plan validator before archival.

#### Acceptance Gate

- [ ] The owning architecture document contains the implemented long-lived
  behavior, while this plan contains only execution history and evidence.
- [ ] The completed plan validates and is ready for the normal monthly archive
  workflow.

## Validation Matrix

| Scenario | Expected result | Validation layer |
| --- | --- | --- |
| Empty folder | Delete, Undo, and Redo preserve the folder state transitions | Native operation test + editor smoke |
| Folder with nested folders and assets | One confirmation, accurate counts, one move per maximal root | Native operation test + editor smoke |
| Search/filter/hidden-content view | Entire selected subtree is included, regardless of presentation filters | Native model/operation test |
| Unregistered or invalid `.dasset` | Analysis blocks and identifies the unknown package instead of omitting it | Native operation test + modal smoke |
| Asset referenced by another asset outside the folder | Delete is blocked with the external referencer listed | Asset/operation test + modal smoke |
| Assets reference each other inside the deleted subtree | Delete is allowed as one batch | Asset/operation test |
| Loading, dirty, or externally referenced loaded package | Delete remains blocked until existing package/document state is resolved | Asset transaction test + editor smoke |
| Loaded asset or managed companion | Batch unload, delete, and persisted-content Undo work without restoring residency | Asset transaction test + editor smoke |
| Companion inspection or ownership ambiguity | Delete is blocked; no main-package-only fallback is offered | Asset/operation test |
| Companion outside the selected folder | It is shown, staged once as a standalone entry, and restored with its owner | Operation transaction test |
| Ordinary file mixed with assets | File is included and restored without asset-registry corruption | Operation transaction test |
| Nested selected roots | Descendants owned by a selected ancestor are staged exactly once | Native operation test |
| Mount root, read-only root, cross-volume root, or directory reparse point | Preflight blocks before mutation with the unsupported reason | Filesystem safety test + modal smoke |
| Plan changes while modal is open | Confirmation reanalyzes and cannot execute the stale displayed plan | Operation/UI test |
| Stage, registry, or directory failure | Completed steps compensate in reverse order; original tree remains usable | Failure-injection test |
| Compensation failure | Transaction does not enter history and retains exact staged recovery paths | Failure-injection test |
| Undo destination conflict | Undo performs no mutation and remains the Undo head | Transaction conflict test |
| Content changed after Undo | Redo performs no mutation and remains the Redo head | Transaction conflict test |
| Undo followed by a new edit | Redo is invalidated by the shared transaction manager | Transaction manager test + editor smoke |
| Global or notification Undo/Redo | Content refreshes and repairs selection without unwanted navigation | Editor interaction smoke |
| Staging cleanup | Evicted/cleared history validates ownership and releases only its operation root | Filesystem safety test |

## Definition of Done

- A user can delete a non-empty Content Browser folder with one clear
  confirmation and never has to delete its children manually.
- The modal accurately reports the immutable recursive deletion scope and
  blocks external references, unknown packages, ambiguous companions, stale
  state, and unsupported roots before mutation.
- Delete, Undo, and Redo are one compensating transaction executed through the
  global transaction manager, with no visible Recycle Bin surface or silent
  partial success.
- Supported same-volume roots use maximal-root rename staging. AssetCore owns
  package/registry safety while the Content Browser transaction is the sole
  owner of physical staging.
- Ordinary failures restore prior state; compensation failures retain staged
  data and report actionable recovery paths.
- The existing global editor Undo/Redo commands and notification action work
  for folder, asset, file, and mixed deletions.
- Successful Delete, Undo, and Redo refresh Content Browser state through a
  lifetime-safe editor event without restoring loaded-package residency.
- Focused tests, end-to-end interaction checks, full build, and documentation
  validation are recorded before the plan is completed.

## Deferred Follow-ups

- Crash-safe recovery center for abandoned staging manifests after an editor
  restart.
- Verified copy-stage-delete support for writable mounted content on a different
  volume from the project `Saved` directory.
- Optional OS Recycle Bin integration for users who need recovery beyond the
  editor transaction history.
- Cancellable background preflight and progress UI for very large content
  trees.
- Restoring asset editor tabs or package residency after Undo if a future user
  workflow requires it.
- Explicit source-control revert/restore affordances beyond the existing
  provider integration.

## Related Documentation

- [Content Browser architecture](../Editor/Architecture/ContentBrowser.md)
- [Editor workspace framework](../Editor/Architecture/WorkspaceFramework.md)
- [File I/O contract](../Runtime/Core/FileIO.md)
- [Build and run](../Development/Build/BuildAndRun.md)
- [Unreal Engine Sources Panel Reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/sources-panel-reference-in-unreal-engine?lang=en-US)
- [Unreal Engine Working with Assets](https://dev.epicgames.com/documentation/en-us/unreal-engine/working-with-assets-in-unreal-engine)
- [Unreal Engine FAssetDeleteModel API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/UnrealEd/FAssetDeleteModel)

## Related Code

- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserOperations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorTransaction.h`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorNotification.h`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserModelTests.cpp`
