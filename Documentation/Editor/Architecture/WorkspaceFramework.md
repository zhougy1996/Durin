# Editor Workspace Framework

This document defines editor-host ownership, persistent settings, application
commands, workspace registration, and document lifecycle contracts.

## Host Ownership

`MainFrame` owns the editor root window. `FEditorHostSettings` stores the native
window size and maximized state together with the global UI scale and color
theme in `EditorHostSettings.yaml`. When that file does not exist, the host uses
monitor-derived defaults and creates it when host display state is first
persisted.

`FLevelEditorSessionSettings` remains owned by `LevelEditor` and persists only
Level workspace state such as viewport cameras, gizmo preferences, Content
Browser state, and Details layout. The two settings files have no compatibility
or migration coupling.

## Application Commands

`MainFrame` owns the stable application menu structure. The shell keeps File,
Edit, Window, and Help as the compact top-level surface, including
application-owned commands such as About.

Registered workspaces may contribute File, Edit, and Window subcommands but
cannot add or replace top-level menus. Activating a document changes only the
target of Save, Undo, and Redo. Workspace-local actions belong in that editor's
toolbar or panels rather than replacing the application menu bar.

## Workspace Registration

`DurinEd` owns the reusable editor workspace and document framework. Workspace
modules register their workspace descriptor, factory, and asset-class routes as
one scoped batch. The manager validates the complete batch before mutation, and
destroying its registration handle removes the routes and documents owned by
that module.

`MainFrame` derives the default host layout and singleton reopen commands from
registered descriptors. The Window menu lists every open document, marks the
active document, and exposes layout reset and workspace-specific panel commands
for the active editor; it does not name concrete editor root windows.

`LevelEditor`, `MaterialEditor`, and `TextureEditor` own their editor-specific
panels and resource behavior. `DurinEd` must not depend on those concrete
modules. Rendering dependencies belong in an editor module only when its preview
implementation actually uses them.

## Document Ownership

`FEditorDocumentTab` is document identity and presentation metadata, not a
resource controller. Each workspace remains responsible for loading, saving,
discarding, releasing, and drawing its resource.

`FEditorWorkspaceRootWindow` provides the shared root-window state transition,
while `FEditorWorkspaceDocumentHost` composes that transition across the
per-resource documents used by Material and Texture editors. The singleton
Level workspace keeps its specialized internal dock-space lifecycle.

Document open and close operations return explicit results. A deferred singleton
open preserves the current document until its replacement succeeds. For dirty
closes, `FEditorWorkspaceManager` owns the single pending document and routes
Save, Discard, or Cancel back to resource-specific workspace callbacks.
`MainFrame` renders the one shared confirmation modal. Repeated close requests
cannot create duplicate confirmations.

Class-filtered asset selection uses the `DurinEd` asset picker. Its optional path
prefix limits candidate enumeration without retaining a loaded current
selection.

## Related Documentation

- `Documentation/Editor/Architecture/PlayInEditorArchitecture.md`
- `Documentation/Editor/Architecture/ReflectedPropertyEditing.md`
- `Documentation/Editor/Design/UIStyle.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
