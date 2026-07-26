# Editor Workspace Refactor Plan

Summary: Editor workspace, document, panel, command, and lifecycle ownership boundaries.

Last reviewed: 2026-07-26

## Current Status

The editor already has a useful workspace and document foundation in `DurinEd`:

- `IEditorWorkspace` defines workspace behavior.
- `FEditorWorkspaceManager` owns registered workspaces, documents, and asset
  editor mappings.
- `EditorWorkspaceUI` supplies stable ImGui window, dock class, and dock-space
  identifiers.

The runtime workspace boundary and module boundary now match for materials.
`MMaterialEditor`, its asset routes, API annotations, and lifetime are owned by
the independent `MaterialEditor` module. Atomic scoped registrations, workspace
descriptors, generated host layout and Window menus, reusable root-window
lifecycle, and the shared asset picker are implemented and covered by native
tests.

`MainFrame` now owns `FEditorHostSettings`, including window size, maximized
state, global UI scale, and color theme in `EditorHostSettings.yaml`. A missing
host file uses monitor-derived defaults; legacy Level Editor display values are
intentionally not imported. The remaining `FLevelEditorSessionSettings` owns
only Level workspace state.
`MainFrame` still loads the concrete Level Editor and Material Editor module
interfaces as a temporary feature-discovery boundary.

Material parent and parameter edits enter the shared reflected-property
transaction path, including coalescing continuous scalar and color controls.
`MMaterialEditor`, `MTextureEditor`, and `MLevelEditor` each expose undo and
redo through the workspace interface by delegating to the global
`FEditorTransactionManager`. Ctrl+Z and Ctrl+Y route through the active
workspace in `DrawWorkspaceHost`. Save/Discard/Cancel confirmation dialogs
handle dirty-document close for all three workspaces.
Singleton Level replacements now use an explicit deferred-open result. The
workspace manager preserves the current document metadata while the unsaved
Level dialog is open and commits the replacement only after the Level switch
succeeds.

The shared editor asset picker now supports a width-reserving trailing action,
a path-prefix filter for project-scoped asset enumeration, and persistent
enabled/disabled presentation. It also accepts a soft current-selection path so
settings UI can display and highlight an asset without retaining a loaded object.
Static-mesh material slots use the trailing action for reset-to-default behavior.
Reflected Object property editing and the
project-settings default-level selector both use the shared picker.

Dirty-document close coordination is now owned by `DurinEd`.
`EEditorDocumentCloseResult` distinguishes closed, pending, rejected, and
cancelled outcomes; `FEditorWorkspaceManager` owns the pending document and
resolves Save/Discard/Cancel through workspace callbacks; and `MainFrame` draws
one shared confirmation modal. A composition-oriented document host now owns
the repeated per-resource root-window iteration for Material and Texture while
those workspaces retain resource lookup, preview, save, discard, release, and
body drawing.

The plan completed on 2026-07-26. Automated acceptance comprises all 205
`EngineTests`, a successful full `all` build, and hidden-window editor startup
from the same build. Interactive acceptance confirmed Level, Material, and
Material Instance opening and routing; project-browser transitions; layout
behavior at the tested UI scales; editing, saving, docking, project switching,
and shutdown. Rendering dependencies for a future material preview viewport
remain explicitly deferred until that feature is implemented.

## Goals

- [x] Make `MaterialEditor` an independently owned editor module.
- [x] Keep `DurinEd` as the shared editor framework rather than introducing a
  monolithic base editor class.
- [x] Make workspace discovery, host layout, and Window menu construction data
  driven.
- [x] Make workspace and asset editor registration atomic or safely reversible.
- [x] Share asset picker, root-window lifecycle, dirty-document close, error
  presentation, and transaction behavior where the semantics are genuinely
  common.
- [x] Separate editor-host settings from Level Editor session and viewport
  settings.
- [x] Preserve current Level, Material, and Material Instance opening behavior
  throughout the migration.

## Non-Goals

- Replacing ImGui docking or the existing workspace/document model.
- Moving Level-specific panels, viewport behavior, or component customizations
  into `DurinEd`.
- Building a material node graph as part of the module split.
- Making all editor panels dynamically loadable plugins in the first pass.
- Adding rendering dependencies to `MaterialEditor` before a material preview
  viewport actually requires them.

## Target Ownership

### DurinEd

`DurinEd` owns editor-wide contracts and reusable implementation:

- Workspace and document interfaces and manager.
- Workspace and asset editor registration handles or batches.
- Workspace descriptors used by the host layout and Window menu.
- Common workspace root-window lifecycle helpers.
- Generic class-filtered asset picker UI.
- Dirty-document close coordination.
- Editor transactions, notifications, and common error presentation.

It must not depend on `LevelEditor` or `MaterialEditor`.

### MainFrame

`MainFrame` owns the native editor host and project-browser transition:

- Root native window creation.
- Global UI scale and editor-host display settings.
- Workspace host menu and top-level dock space.
- Loading or receiving editor feature registrations.
- Applying the registered default workspace layout.

It should not construct editor-specific workspace classes or hard-code material
asset classes. A minimal temporary dependency on concrete module interfaces is
acceptable during migration, but the final host behavior should consume shared
registration descriptors.

### LevelEditor

`LevelEditor` owns only level-editing behavior:

- `MLevelEditor` and the Level workspace descriptor.
- Level document controller and session state.
- Scene viewport, Outliner, Details, Content Browser, Console, and activity UI.
- Import dialogs and asset move coordination used by the Level workspace.
- Level-specific details customizations and component visualizers.
- Registration of `DLevel` with the Level workspace.

### MaterialEditor

`Engine/Source/Editor/MaterialEditor` has its own `.dmodule`, CMake target, API
header, module interface, workspace type, and implementation:

- `MMaterialEditor` and the Material workspace descriptor.
- Registration of `DMaterial` and `DMaterialInstance`.
- Material parameter and inheritance editing.
- Future material preview, compilation diagnostics, and graph-editing UI.

The initial module should depend only on the facilities it uses. Expected
dependencies are `Core`, `Engine`, `AssetCore`, `MonaImGui`, and `DurinEd`.
Rendering dependencies should be added later with the preview implementation.

## Shared Editor Primitives

### Workspace Registration

The current registration path mutates `FEditorWorkspaceManager` one entry at a
time. Failure after an earlier successful insertion leaves a partially
initialized manager and makes retries unreliable.

- [x] Add unregister support or, preferably, scoped registration handles.
- [x] Support validating and committing a batch of workspace and asset editor
  registrations atomically.
- [x] Reject duplicate workspace types and asset class mappings before applying
  any entry in a batch.
- [x] Ensure unloading an editor module removes its registrations and closes or
  rejects documents whose workspace would become unavailable.
- [x] Add tests for duplicate, partial-failure, rollback, and retry behavior.

### Workspace Descriptor

Introduce a shared descriptor that contains the host-facing metadata instead of
requiring `MainFrame` to know literal workspace names and keys. It should cover:

- Stable workspace type.
- Display name and root key.
- Whether the workspace appears in the Window menu.
- Whether it opens by default.
- Singleton document metadata where applicable.
- Workspace factory or registration callback.
- Optional default host docking preference.

- [x] Generate the Window menu from registered descriptors.
- [x] Generate the default host dock layout from registered descriptors.
- [x] Remove the literal `LevelEditor` and `MaterialEditor` root-window names
  from `MainFrameModule.cpp`.
- [x] Keep stable ImGui IDs so existing user layouts are preserved where
  possible; bump the host layout version only when migration requires it.

### Workspace Root Window

`MLevelEditor` and `MMaterialEditor` both implement focus requests, dirty-title
flags, root window class selection, dock-tab activation edge detection, and
close forwarding to `FEditorWorkspaceManager`.

- [x] Extract a small composition-oriented root-window helper into `DurinEd`.
- [x] Return explicit visible, focused, activated, and close-requested state.
- [x] Support both a simple single-window workspace and a workspace containing
  an internal dock space.
- [x] Keep editor-specific shortcut and content drawing in each workspace.
- [x] Do not introduce a base class containing Level-specific panels, settings,
  or play-session behavior.
- [x] Add a composition-oriented multi-document host for the common
  per-resource root-window iteration, activation, and close forwarding used by
  Material and Texture workspaces.
- [x] Keep resource lookup, preview visibility, and document body drawing in
  the owning workspace through callbacks.

### Asset Picker

Material parent selection, texture selection, static-mesh material selection,
reflected object properties, and default-level selection all repeat variants of
asset registry iteration, class resolution, inheritance filtering, search,
loading, and error reporting.

- [x] Add a generic editor asset picker to `DurinEd`.
- [x] Filter by required asset class, with an explicit exact/derived-class
  policy.
- [x] Support None/Clear, current selection, caller-owned search state, and
  caller-provided assignment validation.
- [x] Return load failures without assuming a Level Editor context.
- [x] Migrate Material parent and texture pickers first.
- [x] Migrate the static-mesh material picker while retaining its reflected
  transaction and validation behavior.
- [x] Migrate remaining reflected Details object pickers after matching their
  transaction and validation behavior.
- [x] Evaluate default-level and other class-filtered selectors after the core
  picker API has stabilized.
- [x] Leave room for thumbnails, drag and drop, favorites, and recently used
  assets without requiring them in the first version.

### Dirty Document Close Flow

When a close is requested on a dirty document, the workspace manager should own
the pending document and the editor host should show one shared
Save/Discard/Cancel modal. Workspaces retain only the resource-specific
operations needed to prepare, save, discard, and release a document.

- [x] Define an explicit close result such as closed, pending confirmation, or
  rejected.
- [x] Provide Save, Discard, and Cancel coordination at the document framework
  level.
- [x] Let each workspace implement document-specific save and discard behavior.
- [x] Prevent repeated close requests from opening duplicate confirmation
  dialogs.
- [x] Cover closing active and inactive Material documents and the singleton
  Level document.

### Transactions

Material setters mark packages and render data dirty, and Material Editor value
changes enter the shared `FEditorTransactionManager` through reflected property
editing. All three workspace types (`MLevelEditor`, `MMaterialEditor`,
`MTextureEditor`) expose Undo/Redo through the `IEditorWorkspace` interface,
delegating to the global transaction manager.

- [x] Add transactions for parent changes and scalar, vector, and texture
  parameter overrides.
- [x] Coalesce continuous controls so one drag or color edit produces one
  transaction rather than one entry per frame.
- [x] Route Ctrl+Z/Y through the active workspace and retain a coherent
  editor-wide history policy.
- [x] Verify undo and redo update material render data and dirty state.
  `FReflectedPropertyTransaction::Undo`/`Redo` both call
  `Target.Object->MarkPackageDirty()`, so undo/redo correctly marks packages
  and render data dirty.
- [x] Decide and document whether switching projects or closing the final
  document clears material transactions.
  **Decision:** The global transaction manager is cleared on PIE
  start/stop and on applying play changes. It is intentionally not cleared
  when switching projects or closing documents — undo history persists
  across project transitions while the editor session is alive. This is
  consistent with the fact that the transaction manager is owned by the
  editor engine, not by individual workspaces or documents.

## Settings Split

The former `FEditorSessionSettings` ownership has been split. `MainFrame` owns
editor-host display persistence through `FEditorHostSettings`, while the renamed
`FLevelEditorSessionSettings` contains only viewport, gizmo, Content Browser,
Details, and other Level workspace state.

- [x] Move window size, maximized state, and global UI scale into an editor-host
  settings owner used by `MainFrame`.
- [x] Rename the remaining settings type to make its Level Editor ownership
  explicit.
- [x] Start `EditorHostSettings.yaml` with monitor-derived defaults and do not
  couple it to the legacy `LevelEditorSession.yaml` display map.
- [x] Give Material Editor separate settings only when it gains persistent
  layout or preview state.
- [x] Remove the `MainFrame` dependency on `FLevelEditorModule` for native
  window configuration.

## Recommended Migration Order

1. Add atomic/scoped registrations and tests in `DurinEd`.
2. Add workspace descriptors and make `MainFrame` consume them while retaining
   the current concrete modules.
3. Extract the generic asset picker and root-window helper, then migrate the
   existing Material Editor to use them in place.
4. Create the `MaterialEditor` module and move its workspace, registration, and
   API ownership out of `LevelEditor`.
5. Split editor-host settings from Level Editor session settings.
6. Route undo/redo through active workspaces, add dirty-document close
   confirmation, and document transaction lifecycle.
7. Roll out the shared asset picker to reflected object properties and
   the project-settings default-level selector.
8. Add a material preview module dependency only when preview rendering is
   implemented and validated.

Each step should leave the editor buildable and runnable. Avoid combining the
module move, registration redesign, settings migration, and transaction work in
one change.

### Stage 5: Split editor-host settings from Level Editor session settings

- [x] Move host display persistence and preferences UI into `MainFrame`.
- [x] Rename and narrow the remaining Level Editor session settings owner.
- [x] Leave the legacy Level workspace file independent and start host display
  persistence from monitor-derived defaults.

#### Acceptance Gate

- `MainFrame` no longer obtains native window configuration from
  `FLevelEditorModule`; the full build, native tests, and hidden-window startup
  validation pass with the separated settings boundary.

### Stage 6: Workspace command routing and close coordination

- [x] Undo, redo, save, and dirty-state flags route through the active
  workspace in `DrawWorkspaceHost`.
- [x] `MMaterialEditor`, `MTextureEditor`, and `MLevelEditor` each delegate
  Undo/Redo to the global `FEditorTransactionManager`.
- [x] Reflected property transactions call `MarkPackageDirty()` on both
  commit/apply and undo/redo, so package dirty state and render data stay in
  sync with transaction history.
- [x] The global transaction manager clears on PIE start/stop and play-changes
  apply, but persists across project open/close and document transitions.
- [x] The editor host shows one shared Save/Discard/Cancel modal when a
  workspace defers a dirty-document close.
- [x] Material and Texture use the shared per-resource document host for root
  windows, activation, and close forwarding.
- [x] Deferred singleton Level opens retain the current tab identity until the
  unsaved-change decision and Level activation both succeed.

### Stage 7: Shared asset picker rollout

- [x] Replace the ad-hoc asset combo in `FReflectedPropertyView::EditPropertyWidget`
  (Object branch) with `EditorAssetPicker::Draw`. This unifies every reflected
  object property in the Details panel under the shared picker.
- [x] Migrate the project-settings default-level combo to the shared picker
  and add `PathPrefixFilter` to support project-scoped asset enumeration.
- [x] Preserve the project-settings default-level path as the picker's visible
  and highlighted current selection without forcing the Level asset to stay loaded.
- [x] Remove the unused local `IsClassChildOf` helper that duplicated
  `EditorAssetPicker::MatchesClass`.
- [x] Add `PathPrefixFilter` unit coverage.

### Stage 8: Material preview module dependency

Deferred until preview rendering is implemented and validated in the Material
Editor. The `MaterialEditor` module should add rendering dependencies only when
a material preview viewport actually requires them.

## Validation

- [x] Add native tests for registration commit, rollback, unloading, and retry.
- [x] Test opening multiple Material and Material Instance documents and
  switching the active document.
- [x] Test save, close, cancel, and discard behavior for dirty documents.
- [x] Test that deferred singleton replacements preserve current document
  metadata on cancel and commit it only on successful completion.
- [x] Test Material Editor undo and redo for every parameter kind and parent
  changes.
- [x] Verify Level assets still open in the singleton Level workspace.
- [x] Verify Content Browser double-click routing selects the independently
  registered Material Editor.
- [x] Verify project-browser startup and opening a project after startup.
- [x] Verify the Window menu and default host layout are generated from
  registrations without concrete editor-name literals.
- [x] Verify existing and reset layouts at multiple UI scales.
- [x] Build the full `all` target through `BuildTool` using the active Agent
  profile.
- [x] Run `DurinEditor` from the same full build and smoke-test Level and
  Material editing, saving, docking, project switching, and shutdown.

The 2026-07-25 changes (material editor undo/redo, dirty-document close
confirmation, reflected object picker, default-level picker) were validated
with a successful full `all` build and all 196 `EngineTests` passing. The
broader interactive workflow smoke test remains open.

Deferred Level document replacement was validated with all 197 `EngineTests`,
a successful full `all` build, and hidden-window editor initialization from the
same test preset.

Shared document close coordination and the Material/Texture document host were
validated on 2026-07-25 with all 11 `FEditorWorkspaceManagerTests`, a successful
full `all` build, and hidden-window editor initialization from the same test
preset. The initialization smoke run was stopped after the editor reported
successful startup; interactive Save/Discard/Cancel UI coverage remains part of
the broader open workflow smoke test.

The default-Level picker current-selection fix was validated with both
`FEditorAssetPickerTests`, a successful full `all` build, and hidden-window
editor initialization from the same test preset.

`PathPrefixFilter` candidate matching was isolated as a shared pure predicate
and validated on 2026-07-25 with all 3 `FEditorAssetPickerTests`, all 205
`EngineTests`, a successful full `all` build, and an 8-second hidden-window
editor startup smoke. Coverage includes empty, nested project, and root-mismatch
cases.

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Editor/EditorWorkspace.h`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorWorkspaceUI.h`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorTransaction.h`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/LevelEditorModule.cpp`
- `Engine/Source/Editor/LevelEditor/Public/Widgets/MLevelEditor.h`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Editor/MaterialEditor/Public/MaterialEditorModule.h`
- `Engine/Source/Editor/MaterialEditor/Private/Workspace/MaterialEditorWorkspace.h`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.h`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Settings/LevelEditorSessionSettings.h`
