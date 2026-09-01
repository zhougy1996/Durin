# Editor Transaction Global Cutover Plan

Summary: Make the editor-owned transactor the sole application history, move revision and event services into it, and remove the legacy manager.

Last reviewed: 2026-08-30

Status: Archived
Completed: 2026-08-30

## Current Status

P4 is complete. `DEditorEngine::Trans` is the sole application history and
owns ordering, executable records, package and mounted-content revisions,
events, deferred barriers, and collector-visible retention. The legacy manager,
ID bridges, and transaction-specific roots have been removed.

## Goal

Route every application Undo/Redo, workspace adapter, Activity History,
notification, save checkpoint, module drain, and test seam through the single
`DEditorEngine::Trans` service. Move the remaining session metadata into
`DTransBuffer`, then remove `FTransactionManager`, its bridge entries, and all
transaction-specific roots.

## Scope

DurinEd transactor/application contracts, editor engine ownership, MainFrame,
AssetTools, ContentBrowser, MaterialEditor, LevelEditor, TextureEditor, editor
tests, and authoritative transaction/GC/workspace documentation.

## Non-Goals

- Do not change domain command semantics established in P3.
- Do not persist the ordinary Undo buffer or add multi-user exchange.
- Do not add a second history or a compatibility manager alias.

## Implementation Stages

### Stage 0: Integrate Session Metadata

- [x] Store package before/after revision transitions in `FTransaction` and
  apply them only after successful Execute/Undo/Redo.
- [x] Move saved checkpoints, dirty synchronization, package forgetting, and
  mounted-content revision into `DTransBuffer` with collector-visible package
  references.
- [x] Unify committed/undone/redone/failed/discarded/evicted events on the
  application-facing transactor event contract.
- [x] Preserve deferred completion and failure invariants for every metadata
  transition.

### Stage 1: Global Application Cutover

- [x] Route editor engine, workspace Undo/Redo, Activity History,
  notifications, saving, asset mutation adapters, property views, and all
  module command contexts directly through `DEditorEngine::Trans`.
- [x] Replace standalone manager test seams with rooted `DTransBuffer`
  instances and preserve exact assertions.
- [x] Route module-retirement drains through the authoritative transactor.

### Stage 2: Legacy Removal

- [x] Remove `FTransactionManager`, ID bridges, manager-owned package roots,
  and its source implementation.
- [x] Remove all legacy symbols, accessors, includes, comments, and adapters.
- [x] Prove the editor owns exactly one global history and no retained-history
  manual root remains. The active property-preview session keeps its existing
  transient target root when no transactor is supplied; it is not committed
  history and releases the root when the interaction ends.

### Stage 3: Documentation And Qualification

- [x] Update transactor, reflected-property, workspace, GC, and affected domain
  contracts and mark P4 complete in the roadmap.
- [x] Run focused transaction, editor shell, property, material, Level, asset,
  texture, save/checkpoint, and module-retirement tests.
- [x] Run the complete registered-profile build and all documentation
  validators, recording exact evidence here.

## Acceptance Gates

- [x] `DEditorEngine::Trans` is the only editor-session history owner.
- [x] Package revisions, dirty/saved state, mounted-content revision, events,
  and deferred completion change only after successful transitions.
- [x] Every production and test caller uses `DTransactor`/`DTransBuffer`;
  `FTransactionManager` and legacy bridges are absent.
- [x] History/package collector edges release on forget, branch replacement,
  eviction, reset, module drain, and shutdown.
- [x] One global Undo/Redo order is visible in every workspace and Activity
  History surface.

## Validation

Use the repository agent build and native-test workflows. Required lanes are
`EditorOperationTests`, `EditorPropertyTests`, `EditorShellTests`,
`MaterialTests`, `SplineTests`, `WorldTests`, `StaticMeshTests`, and
`TextureTests`, followed by the complete registered-profile build and all
documentation validators.

Completed evidence on macOS arm64 Debug `DurinEditor`:

- full `all` build passed;
- `EditorOperationTests` 39/39, `EditorPropertyTests` 32/32,
  `EditorShellTests` 47/47, `MaterialTests` 108/108, `SplineTests` 40/40, `WorldTests` 106/106,
  `StaticMeshTests` 75/75, `TextureTests` 78/78, `ViewportTests` 104/104,
  `LevelMutationTests` 15/15, `SkyBoxTests` 11/11, and
  `ContentBrowserWorkflowTests` 65/65 passed;
- the complete `fast-all` contract, feature, and infrastructure profile passed;
- documentation validation passed for all 143 files, 5 active/5 completed/295
  archived plans, and 3 active/23 archived roadmaps.

## Related Code

- [`Transaction.h`](../../../../Engine/Source/Editor/DurinEd/Public/Editor/Transaction.h)
- [`Transactor.h`](../../../../Engine/Source/Editor/DurinEd/Public/Editor/Transactor.h)
- [`Transactor.cpp`](../../../../Engine/Source/Editor/DurinEd/Private/Editor/Transactor.cpp)
- [`EditorEngine.h`](../../../../Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h)
- [`WorkspaceRootWindow.cpp`](../../../../Engine/Source/Editor/DurinEd/Private/Editor/WorkspaceRootWindow.cpp)
