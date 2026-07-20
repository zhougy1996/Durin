# Editor Workspace Refactor TODO

Last reviewed: 2026-07-20

## Current Status

The editor already has a useful workspace and document foundation in `DurinEd`:

- `IEditorWorkspace` defines workspace behavior.
- `FEditorWorkspaceManager` owns registered workspaces, documents, and asset
  editor mappings.
- `EditorWorkspaceUI` supplies stable ImGui window, dock class, and dock-space
  identifiers.

The runtime workspace boundary is currently ahead of the module boundary.
`MMaterialEditor` is a distinct workspace, but its implementation, asset editor
registrations, API annotations, and lifetime are all owned by the `LevelEditor`
module. `MainFrame` also hard-codes knowledge of Level Editor and Material Editor
root windows and obtains host-window settings through `FLevelEditorModule`.

This arrangement is workable while only two workspaces exist, but it makes new
asset editors harder to add, leaves reusable asset-selection and document-host
behavior duplicated, and will cause `LevelEditor` to absorb unrelated material
preview, compilation, and graph-editing responsibilities.

## Goals

- [ ] Make `MaterialEditor` an independently owned editor module.
- [ ] Keep `DurinEd` as the shared editor framework rather than introducing a
  monolithic base editor class.
- [ ] Make workspace discovery, host layout, and Window menu construction data
  driven.
- [ ] Make workspace and asset editor registration atomic or safely reversible.
- [ ] Share asset picker, root-window lifecycle, dirty-document close, error
  presentation, and transaction behavior where the semantics are genuinely
  common.
- [ ] Separate editor-host settings from Level Editor session and viewport
  settings.
- [ ] Preserve current Level, Material, and Material Instance opening behavior
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

Create `Engine/Source/Editor/MaterialEditor` with its own `.dmodule`, CMake
target, API header, module interface, workspace type, and implementation:

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
- [ ] Migrate Details object and static-mesh material pickers after matching
  their transaction and validation behavior.
- [ ] Evaluate default-level and other class-filtered selectors after the core
  picker API has stabilized.
- [ ] Leave room for thumbnails, drag and drop, favorites, and recently used
  assets without requiring them in the first version.

### Dirty Document Close Flow

Both current workspaces reject a close request when a package is dirty, but the
workspace manager has no pending confirmation protocol. A close action can
therefore appear to do nothing.

- [ ] Define an explicit close result such as closed, pending confirmation, or
  rejected.
- [ ] Provide Save, Discard, and Cancel coordination at the document framework
  level.
- [ ] Let each workspace implement document-specific save and discard behavior.
- [ ] Prevent repeated close requests from opening duplicate confirmation
  dialogs.
- [ ] Cover closing active and inactive Material documents and the singleton
  Level document.

### Transactions

Material setters correctly mark packages and render data dirty, but Material
Editor edits currently bypass the shared `FEditorTransactionManager`.

- [ ] Add transactions for parent changes and scalar, vector, and texture
  parameter overrides.
- [ ] Coalesce continuous controls so one drag or color edit produces one
  transaction rather than one entry per frame.
- [ ] Make Ctrl+Z and Ctrl+Y operate on the active workspace while retaining a
  coherent editor-wide history policy.
- [ ] Verify undo and redo update material render data and dirty state.
- [ ] Decide and document whether switching projects or closing the final
  document clears material transactions.

## Settings Split

`FEditorSessionSettings` currently mixes host-window state with Level-specific
viewport, gizmo, Content Browser, and Details state.

- [ ] Move window size, maximized state, and global UI scale into an editor-host
  settings owner used by `MainFrame`.
- [ ] Rename the remaining settings type to make its Level Editor ownership
  explicit.
- [ ] Preserve existing persisted values or provide a one-time migration from
  `LevelEditorSession.yaml`.
- [ ] Give Material Editor separate settings only when it gains persistent
  layout or preview state.
- [ ] Remove the `MainFrame` dependency on `FLevelEditorModule` for native
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
6. Add dirty-document confirmation and Material Editor transactions.
7. Migrate remaining Details and project-setting asset selectors after the
   shared picker has proven stable.
8. Add a material preview module dependency only when preview rendering is
   implemented and validated.

Each step should leave the editor buildable and runnable. Avoid combining the
module move, registration redesign, settings migration, and transaction work in
one change.

## Validation

- [x] Add native tests for registration commit, rollback, unloading, and retry.
- [x] Test opening multiple Material and Material Instance documents and
  switching the active document.
- [ ] Test save, close, cancel, and discard behavior for dirty documents.
- [ ] Test Material Editor undo and redo for every parameter kind and parent
  changes.
- [ ] Verify Level assets still open in the singleton Level workspace.
- [ ] Verify Content Browser double-click routing selects the independently
  registered Material Editor.
- [ ] Verify project-browser startup and opening a project after startup.
- [x] Verify the Window menu and default host layout are generated from
  registrations without concrete editor-name literals.
- [ ] Verify existing and reset layouts at multiple UI scales.
- [x] Build the full `all` target through `BuildTool` using the active Agent
  profile.
- [ ] Run `DurinEditor` from the same full build and smoke-test Level and
  Material editing, saving, docking, project switching, and shutdown.

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Editor/EditorWorkspace.h`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorWorkspaceUI.h`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorTransaction.h`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/LevelEditorModule.cpp`
- `Engine/Source/Editor/LevelEditor/Public/Widgets/MLevelEditor.h`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MMaterialEditor.h`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MMaterialEditor.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/DetailsPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/EditorSessionSettings.h`
