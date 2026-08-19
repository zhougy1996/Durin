# Editor Infrastructure Simplification Plan

Summary: Replace manual editor registration and mounted-source lifecycles with scoped ownership, then consolidate repeated import-form and editable-asset workspace glue.

Last reviewed: 2026-08-20

Status: Completed
Completed: 2026-08-20

## Current Status

All stages are complete. Import registrations now use exact move-only handles;
prepared mounted sources carry rollback ownership until commit; the three
single-source import dialogs share mounted-source form state and the two mesh
dialogs share one coordinate model; Material and Texture compose one editable
document model in `DurinEd`; and the bounded MainFrame and Content Browser glue
has been removed. Lasting contracts are published in the related architecture
and workflow documents.

Validation completed on 2026-08-20: `AssetImportCoreTests` (31),
`AssetMountedSourceTests` (3), `EditorAssetWorkflowTests` (84 passed, one
skipped), `EditorShellTests` (42), `AssetImportTests` (17), `SceneImportTests`
(15), and `TextureTests` (66 passed, two skipped) passed. The `fast-all`
aggregate, full `all` build, hidden Project Browser startup/shutdown, and hidden
Engine-project workspace startup/shutdown also passed.

## Goal

Make editor infrastructure use scoped ownership for registrations and staged
mounted-source files, and represent shared import-form and editable-document
behavior once. Preserve provider admission and drain ordering, source rollback,
asset/package identity, document close semantics, transaction behavior, UI
labels, defaults, validation messages, and editor startup behavior while
removing duplicated cleanup paths and pass-through glue.

## Scope

- Importer descriptor registration ownership in `AssetImportCore` and
  `StandardAssetImport`, including collision-safe rollback and shutdown drain.
- Scoped ownership for prepared `FMountedSourceFile` values used by editor
  import and source-translation workflows.
- Shared mounted-source form state for Texture2D, StaticMesh, and Scene import
  dialogs, with a shared mesh-coordinate editing model for StaticMesh and
  Scene.
- Shared editable-asset document composition for Material and Texture
  workspaces where their package and transaction semantics are identical.
- Removal of the redundant MainFrame workspace-activation flag, aggregation of
  MainFrame transient view state, and removal of pure Content Browser forwarding
  methods.
- Focused native tests, module builds, editor smoke validation, and publication
  of any lasting ownership contract changes.

## Non-Goals

- Splitting large files solely because of line count, including
  `SceneImport.cpp`, `ViewportPresentation.cpp`, or Content Browser view code.
- Changing source capture, import planning, candidate construction, package
  publication, DDC keys, format support, Scene output identity, or reimport
  policy.
- Unifying TextureCube's six-face/panorama form with the single-source form
  when doing so would add branching rather than remove duplicated behavior.
- Introducing a concrete-editor dependency into `DurinEd`, or teaching shared
  infrastructure about Texture, Material, StaticMesh, or Scene policy.
- Replacing immediate-mode UI, changing editor layout, labels, shortcuts,
  default paths, confirmation behavior, or user-visible validation ordering.
- Generalizing every AssetCore prepare/commit/rollback type. Mounted-source
  replacement and relocation remain explicit unless Stage 2 evidence shows the
  exact same scoped contract and tests can preserve their distinct semantics.
- Changing asynchronous import threading, cancellation, or mailbox ownership.

## Design Decisions and Invariants

### Registration ownership

- A successful importer registration returns one move-only scoped handle tied
  to the exact acquired registration, not merely a provider ID string.
- Resetting a handle closes provider admission, cancels and drains admitted
  work, retires registered handlers, and removes only that handle's registration.
  A stale handle cannot unregister a later registration that reused the same ID.
- `StandardAssetImport` retains its three handles in declaration order and
  resets them in strict reverse order. Partial startup failure unwinds only
  handles acquired by that attempt.
- Module callback gates and retained resource leases continue to bound code
  lifetime. Scoped registration does not replace or weaken those gates.
- Property-editing and source-relocation policies keep their existing ownership
  unless they can return an equally precise scoped handle without widening this
  plan.

### Mounted-source transaction ownership

- A successfully prepared mounted-source value owns rollback until explicitly
  committed. Destruction of an uncommitted owner performs the existing
  best-effort rollback and never throws.
- Commit releases rollback ownership but preserves the published source
  identity needed by the caller. Move transfers ownership exactly once; copying
  an owning prepared value is forbidden.
- Referenced existing files and reused identical files remain non-owning and
  are never removed by destruction or rollback.
- Arrays and bundles commit only after every dependent build/save/publication
  step succeeds. Destruction rolls back still-owned staged files in reverse
  acquisition order where ordering is observable.
- Source ingestion remains separate from package publication where the current
  architecture explicitly commits it before asynchronous planning.

### Shared import-form composition

- Reuse is composition, not a base-class hierarchy. One private LevelEditor
  form model owns source mode, editable buffers, automatic-suggestion identity,
  common browse rules, mounted-source inspection, and common presentation.
- The concrete dialog supplies format filters, fallback names, category/path
  policy, destination accessor, format-specific validation, settings UI, and
  the final import action.
- Automatic suggestions continue updating only values that are empty or still
  equal to the last automatic suggestion. Manual edits remain authoritative.
- One mesh-coordinate model owns the Durin, Y-up/-Z-forward, and Custom presets
  plus custom axis editing. StaticMesh and Scene consume the same values without
  changing the serialized `FStaticMeshImportSettings` contract.
- Validation precedence and user-facing diagnostic text remain stable unless a
  test proves that the existing order is inconsistent.

### Editable asset document composition

- Shared code is class-neutral and lives in `DurinEd`; concrete modules still
  load exact asset types, own previews/build cancellation, draw their UI, and
  report type-specific errors.
- The shared composition may own resource-to-object association, active
  document identity, package dirty/save/discard queries, document-host focus,
  and forwarding to the global transaction manager.
- Concrete hooks run before close, after close, and before document switch for
  property-edit finalization, preview release, and Texture build cancellation.
  A rejected hook leaves all document state unchanged.
- Read-only inspectors and the specialized Level workspace do not adopt the
  editable helper unless their semantics become identical; this plan does not
  force them through an editable abstraction.
- Existing incompatible-package rejection and request-scoped package ownership
  remain in the owning workspace path.

### Incidental cleanup boundary

- `bWorkspaceActivationStarted` is removed only after tests prove that
  `LoadingWorkspace` always transitions synchronously to `WorkspaceReady` or
  `Failed` in the same bootstrap advancement.
- MainFrame transient dialog/menu state becomes one private value passed by
  reference. It does not enter persisted host settings or bootstrap state.
- Content Browser forwarding methods are removed only when the view can call
  the already-owned model, operation, or item-view authority directly without
  moving policy into presentation code.

## Current Foundations and Gaps

| Area | Existing foundation | Selected gap |
| --- | --- | --- |
| Import registration | `FImportService` already registers one descriptor atomically and unregisters with admission drain. | Callers own registration by provider ID and StandardAssetImport rollback unconditionally unregisters IDs it may not own. |
| Mounted source | AssetCore already records whether preparation created a file; commit clears ownership and rollback removes only a created file. | Callers manually cover every early return, and multi-source paths repeat rollback loops. |
| Import dialogs | Destination and modal models already centralize asset-path editing and popup admission. | Source mode, source destination, browsing, suggestion, mount preview, and mesh-coordinate state remain repeated. |
| Workspace framework | `FWorkspaceDocumentHost`, `FWorkspaceManager`, property editing, and global transactions already own shared contracts. | Material and Texture still duplicate the editable package/document adapter around those contracts. |
| MainFrame and Content Browser | Bootstrap transitions, host drawing, model, operations, and item-view authorities are explicit. | One bootstrap guard is redundant; transient UI state and several presentation calls are manually forwarded. |

## Implementation Stages

### Stage 0: Freeze lifecycle and presentation contracts

- [x] Add an importer collision test proving a failed registration attempt
  cannot remove an existing provider, its single-asset handlers, or its record
  handler.
- [x] Add importer teardown tests for exact-owner reset, reverse batch unwind,
  stale-handle reset after ID reuse, outstanding lease drain, and partial
  StandardAssetImport startup failure.
- [x] Extend mounted-source tests to cover automatic rollback after every
  post-prepare failure boundary, explicit commit, move transfer, referenced and
  reused inputs, arrays with partial preparation, and idempotent empty cleanup.
- [x] Freeze Texture, StaticMesh, and Scene dialog state tests for source-mode
  switching, suggested destination updates, manual override preservation,
  mount/extension validation, coordinate presets, and validation precedence.
- [x] Freeze Material and Texture workspace tests for open/activate/switch,
  dirty close, Save/Discard/Cancel, property-edit deactivation, Undo/Redo, close
  cleanup, and failed hook behavior.
- [x] Add or identify bootstrap coverage proving the synchronous
  `LoadingWorkspace` transition and capture current MainFrame/Content Browser
  visible behavior for the incidental cleanup.

#### Acceptance Gate

- Focused tests fail for the foreign-provider rollback defect, pass for all
  currently intended lifecycle and UI behavior, and establish exact contracts
  for subsequent ownership and deduplication stages.

### Stage 1: Introduce exact scoped importer registrations

- [x] Add a move-only importer registration handle whose reset operation is
  validated against an exact service-owned registration identity.
- [x] Change `FImportService` registration state to mint and validate that
  identity while preserving atomic descriptor installation, revision changes,
  provider admission, cancellation, draining, handler teardown, and leases.
- [x] Migrate StandardAssetImport's Scene, Assimp, and DurinImage descriptors to
  retained handles and remove unconditional ID-based rollback and `GRegistered`.
- [x] Make partial registration failure unwind only successfully acquired
  handles in reverse order, then retain the same reverse order at shutdown.
- [x] Keep an ID-based query API for lookup/diagnostics, but remove production
  unregister calls that do not carry ownership.

#### Acceptance Gate

- A colliding registration leaves the prior provider fully usable; partial
  startup and normal shutdown drain only owned registrations; importer revision,
  capability lookup, async cancellation, and module unload tests pass.

### Stage 2: Make prepared mounted-source files scoped

- [x] Introduce move-only rollback ownership around `FMountedSourceFile` with a
  no-throw destructor and explicit commit/release operation.
- [x] Migrate Texture2D, TextureCube, Terrain, Scene, and Standard provider
  source-translation paths; remove local rollback lambdas and repeated
  per-return cleanup that the scoped owner now guarantees.
- [x] Preserve explicit package unload and build cancellation where those are
  separate resources; do not hide them inside the source owner.
- [x] For multi-file inputs, use one bounded owner collection whose commit and
  rollback order is deterministic and covered by failure injection.
- [x] Remove the old free commit/rollback surface after all production callers
  use scoped ownership, unless a non-owning compatibility call remains required
  and is documented.

#### Acceptance Gate

- Every created source is removed on pre-commit failure and retained after
  commit; existing/reused sources are never removed; all typed import,
  reimport, repair, source-change, Scene bundle, and failure-injection tests
  pass without duplicate cleanup paths.

### Stage 3: Consolidate mounted-source import form state

- [x] Add one private LevelEditor mounted-source form model and migrate
  Texture2D and StaticMesh first, proving the single-asset destination path.
- [x] Migrate Scene using destination-directory callbacks while retaining its
  asynchronous preview/import state outside the shared form.
- [x] Add the shared mesh-coordinate model and remove the duplicate preset enum,
  labels, axis-combo helper, and settings synchronization from StaticMesh and
  Scene.
- [x] Keep concrete format admission, settings, preview contents, import button,
  and final service calls in each concrete dialog.
- [x] Compare visible labels, default paths, validation messages, enablement,
  manual override behavior, and cancellation against Stage 0 fixtures.

#### Acceptance Gate

- The three dialogs retain their existing workflows and diagnostics; one
  implementation owns common source state and one owns mesh-coordinate state;
  no concrete dialog duplicates common browse/suggestion/mount-preview logic.

### Stage 4: Consolidate editable asset document glue

- [x] Add a class-neutral editable-asset document composition beside the
  existing workspace document host, with explicit concrete hooks and no
  dependency on MaterialEditor or TextureEditor.
- [x] Migrate MaterialEditor first and validate load, dirty, save/discard,
  transaction, property-edit, preview, and close behavior.
- [x] Migrate TextureEditor, retaining Texture build cancellation, preview
  state, mounted-source operations, and type-specific errors in TextureEditor.
- [x] Remove duplicated active-resource, package-state, global transaction, and
  document-host forwarding only after both modules pass the same shared tests.
- [x] Keep LevelEditor and read-only inspectors on their specialized paths and
  document why their semantics do not use the editable composition.

#### Acceptance Gate

- Material and Texture use one tested editable-document lifecycle; concrete
  cleanup hooks remain exact; Save/Discard/Cancel, Undo/Redo, focus, dirty
  decoration, document switching, and module shutdown match Stage 0 behavior.

### Stage 5: Remove bounded incidental editor glue and qualify

- [x] Remove the redundant MainFrame workspace-activation field and branch.
- [x] Aggregate transient MainFrame menu/dialog state into one private view
  state and shorten title-bar/menu/workspace-host signatures without changing
  persistence or capture lifetime.
- [x] Remove pure Content Browser forwarding declarations/definitions and call
  the existing model, operations, and item-view authorities directly.
- [x] Remove obsolete helpers, includes, compatibility overloads, and comments
  made inaccurate by the completed ownership changes.
- [x] Run focused native tests, affected module builds, aggregate editor tests,
  and an editor startup/import/open/edit/save/close/shutdown smoke workflow
  following the repository testing and build/run guides.
- [x] Publish lasting registration, mounted-source ownership, and workspace
  composition rules in their authoritative documents, then validate changed
  documentation and all active plans.

#### Acceptance Gate

- Required tests, builds, editor smoke, and documentation validation pass; no
  provider or source owner can clean up another owner's state; visible editor
  behavior is unchanged; the selected duplicate lifecycle and forwarding paths
  have one production implementation.

## Validation Matrix

| Contract | Required evidence |
| --- | --- |
| Registration isolation | Collision, partial startup, stale handle, ID reuse, reverse reset, capability lookup, revision, admission close, async drain, lease, and module unload tests. |
| Mounted-source ownership | Created, referenced, reused, committed, moved, partially prepared bundle, build failure, save failure, publication failure, and destructor rollback tests. |
| Import forms | Texture2D, StaticMesh, and Scene defaults, browse results, source modes, automatic/manual suggestions, mount policy, extension policy, coordinate settings, preview, cancellation, and diagnostic precedence. |
| Editable documents | Material and Texture open/activate/switch, compatibility rejection, dirty state, Save/Discard/Cancel, Undo/Redo, property edits, build/preview cleanup, focus, close, and shutdown. |
| Bootstrap and presentation | Hidden/visible host bootstrap, first-present gate, workspace failure, default-document failure, menus/dialogs, Content Browser item formatting/copy/reveal, and startup/shutdown smoke. |
| Layering | AssetCore remains provider-neutral; DurinEd remains independent of concrete editor modules; LevelEditor-private form models do not leak into runtime or provider APIs. |
| Handoff | Focused and aggregate native tests, affected builds, editor smoke, changed-document validation, and all-plan validation follow repository guidance and pass. |

## Definition of Done

- Importer registrations are owned by exact move-only handles; collision or
  stale cleanup cannot remove another registration.
- Prepared mounted-source files automatically roll back until committed, and
  production editor import paths contain no repeated manual early-return
  rollback for that resource.
- Texture2D, StaticMesh, and Scene dialogs share mounted-source form state;
  StaticMesh and Scene share one mesh-coordinate editing model.
- Material and Texture workspaces share one editable-document lifecycle while
  retaining their concrete rendering, build, and authoring policy.
- The redundant bootstrap flag, repeated MainFrame transient-state parameter
  list, and pure Content Browser forwarding methods are removed.
- User-visible workflows, labels, defaults, errors, document behavior, import
  results, package/source identities, and shutdown ordering remain unchanged.
- Lasting ownership contracts are updated, required validation passes, and no
  temporary compatibility path or duplicated production implementation remains.

## Deferred Follow-ups

- Apply scoped ownership to mounted-source replacement and relocation only if
  separate evidence shows repeated cleanup risk and their publish/restore
  semantics can remain explicit.
- Consider a reusable registration batch utility for other modular-feature
  families after the importer handle establishes a second concrete consumer.
- Revisit TextureCube form reuse only if its six-face and panorama workflows
  acquire enough common single-source behavior to reduce, rather than increase,
  branching.
- Split large editor implementation files only when a later change identifies
  independently owned state or a testable semantic boundary; line count alone
  is not an activation condition.

## Related Documentation

- [Editor Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Mounted Source Workflows](../Editor/Guides/MountedSourceWorkflows.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Editor/AssetImportCore/Public/ImportService.h`
- `Engine/Source/Editor/AssetImportCore/Private/ImportService.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp`
- `Engine/Source/Runtime/AssetCore/Public/Asset/MountedSource.h`
- `Engine/Source/Runtime/AssetCore/Private/Asset/MountedSource.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/ImportDialogState.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/StaticMeshImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SceneImportDialog.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/WorkspaceRootWindow.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/WorkspaceRootWindow.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetImportCoreTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/MountedSourceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ImportDialogStateTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorWorkspaceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorBootstrapStateTests.cpp`
