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

`LevelEditor`, `MaterialEditor`, `TextureEditor`, and `StaticMeshEditor` own
their editor-specific panels and resource behavior. `DurinEd` must not depend on
those concrete modules. Rendering dependencies belong in an editor module only
when its preview implementation actually uses them. MainFrame registers these
modules in that order and rolls them back in reverse order.

`MaterialEditor`, `TextureEditor`, and `StaticMeshEditor` each expose one
integration lifecycle for their workspace routes and thumbnail extensions.
MainFrame supplies the long-lived thumbnail service; a failed exact-class
provider registration removes every earlier contribution from that module, and
unregistration removes thumbnail admission in reverse order before closing its
documents.

MainFrame shutdown first stops Content Browser request admission. It then
unregisters StaticMesh, Texture, Material, and Level integrations in reverse
composition order, so each concrete thumbnail handle drains its queued and
in-flight leases before its workspace documents close. MainFrame next drains
and destroys the provider-neutral thumbnail caches and service. Concrete editor
modules may unload only after those steps, so no route, document, provider,
session, preview object, or queued upload can retain module code.

## Document Ownership

`FEditorDocumentTab` is document identity and presentation metadata, not a
resource controller. Each workspace remains responsible for loading, saving,
discarding, releasing, and drawing its resource.

`FEditorWorkspaceRootWindow` provides the shared root-window state transition,
while `FEditorWorkspaceDocumentHost` composes that transition across the
per-resource documents used by Material, Texture, and StaticMesh editors. The
singleton Level workspace keeps its specialized internal dock-space lifecycle.

Document open and close operations return explicit results. A deferred singleton
open preserves the current document until its replacement succeeds. For dirty
closes, `FEditorWorkspaceManager` owns the single pending document and routes
Save, Discard, or Cancel back to resource-specific workspace callbacks.
`MainFrame` renders the one shared confirmation modal. Repeated close requests
cannot create duplicate confirmations.

Level, Material, Texture, and StaticMesh workspaces apply the same compatibility
policy after loading and before document activation. A load report containing any
compatibility issue rejects the requested document, leaves the previous active
document or world unchanged, and reports one error directing the user to Asset
Compatibility Audit for full details. Each request captures package ownership
before loading so rejection or activation failure releases only packages
introduced by that request; packages that were already loaded remain owned by
their existing users.

Workspaces expose no save, discard, repair, open-without-saving, or data-loss
action for an incompatible package. AssetCore's ordinary-save guard remains the
final persistence boundary if another caller bypasses the workspace policy.

The `StaticMeshEditor` workspace is presented as **StaticMesh Inspector** and is
strictly read-only. Exact `DStaticMesh` routes open closable per-resource
documents. Each document owns an isolated preview world and viewport, supports
orbit, pan, zoom, framing, and solid/wireframe presentation, and releases its
mesh component before destroying the scene. Its active document never becomes
dirty and never enables global Save.

Project-wide compatibility review is an explicit application tool rather than
a workspace document. `MainFrame` owns the non-modal `Tools > Asset
Compatibility Audit` window and its presentation actions, while `DurinEd` owns
the request-scoped worker, cancellation, request-serial mailbox, and path-keyed
result index. Opening or drawing the window does not start work; only `Run
Audit` captures the registry and reflection snapshots. The window can reveal a
selected package in the Level Editor Content Browser, but neither `MainFrame`
nor a concrete asset workspace owns compatibility classification or a write
action.

The Level workspace composition root constructs panels and document services in
dependency order, while panel and dialog presenters remain module-private
implementation details.

Class-filtered asset selection uses the `DurinEd` asset picker. Its optional path
prefix limits candidate enumeration without retaining a loaded current
selection.

## Related Documentation

- `Documentation/Editor/Architecture/PlayInEditorArchitecture.md`
- `Documentation/Editor/Architecture/ReflectedPropertyEditing.md`
- `Documentation/Editor/Design/UIStyle.md`
- `Documentation/Editor/Guides/StaticMeshInspector.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
