# Editor Workspace Framework

Summary: Define editor workspaces, documents, panels, commands, layout, and host integration.

Modules: AssetMaintenance, DurinEd, LevelEditor, MainFrame

This document defines editor-host ownership, persistent settings, application
commands, workspace registration, and document lifecycle contracts.

## C++ Ownership Boundary

Shared workspace, document, transaction, property, asset, preview, and
interaction contracts live directly in `Durin::Editor`. Concrete editor modules
own their ordinary C++ APIs under `Durin::Editor::MainFrame`, `LevelEditor`,
`MaterialEditor`, `TextureEditor`, `StaticMeshEditor`, and
`SkeletalMeshEditor`. Runtime and reflected object types remain in `Durin`; in
particular, `DEditorEngine` keeps its stable reflection identity. Module lookup
strings and persisted workspace/document
identities are independent of these C++ namespaces.

## Host Ownership

`DurinEd` owns the implementation-neutral host lifecycle and bootstrap
contracts under `Durin::Editor::Host`, exposed to `DEditorEngine` through
`Durin::IEditorHost`. `MainFrame` depends on that contract and owns the editor
root window plus its concrete implementation under
`Durin::Editor::MainFrame`. `FHostSettings` stores the native window size and
maximized state together with the global UI scale and color theme in the stable
`EditorHostSettings.yaml` file. When that file does not exist, the host uses
monitor-derived defaults and creates it when host display state is first
persisted. The reflected `DEditorEngine` remains in `Durin` and does not depend
on `MainFrame` headers.

`Editor::Level::FLevelEditorSessionSettings` persists only Level workspace state
such as viewport cameras, gizmo preferences, and Details layout.
`ContentBrowserSettings.yaml` independently persists browser presentation.
When the new file is absent it starts from defaults and does not read retired
Level Editor browser keys.

## Startup Bootstrap

`MainFrame` constructs and owns a lightweight native shell before loading
concrete editor modules. Persisted maximized state is applied while the window
is hidden and before native viewport creation. `DEditorEngine::Init()` then
advances the `IEditorHost` game-thread bootstrap to completion through a narrow
Launch-owned startup pump. Widget drawing observes state but never admits
startup work, and ordinary `DEditorEngine::Tick()` performs no bootstrap
transition.

Project startup follows the forward-only sequence
`ConstructingShell -> WaitingForFirstPresent -> LoadingWorkspace ->
WorkspaceReady -> LoadingDefaultDocument -> Ready`. Workspace registration or
opening failure enters terminal `Failed`. Project Browser uses the shell-only
`WaitingForFirstPresent -> Ready` path and does not register project workspaces
or load a default document.

For a visible host, `WaitingForFirstPresent` advances only after the production
RHI presentation path publishes a real FirstPresent milestone. Before each
blocking workspace or default-document operation, the pump processes events and
submits the current loading phase through the existing Mona/ImGui root window.
Hidden hosts bypass the presentation gate and run the same semantic phases
without requiring an active native window. A close request is initialization
cancellation and never enters the normal main loop.

Workspace readiness and default-document readiness are separate contracts.
After Level, Material, Texture, and StaticMesh registration and default
workspace opening succeed, MainFrame publishes `WorkspaceReady` for one frame
before asking LevelEditor to load the configured default level. The document
state advances independently through `Pending -> Loading -> Ready|Failed`.
Missing, incompatible, corrupt, or unactivatable default levels publish document
`Failed`, force the overall bootstrap to terminal `Failed`, preserve one
actionable owning diagnostic, and fail engine initialization. Successful
`FEngineLoop::Init()` is therefore authoritative evidence that a project editor
has an active default Level and is ready for ordinary ticking.

Loading progress is phase-based rather than time- or byte-based. The compact
themed view reports shell presentation, workspace activation, and default-Level
opening with a stable phase index and progress bar. Because the underlying
loads remain synchronous, it does not promise animation or fabricate
intra-operation percentages while one phase blocks the game thread.

Destroying the editor host removes the bootstrap context, so no later startup
callback can admit work. MainFrame then stops request admission and unwinds
workspace integrations in the reverse order described below. Partially loaded
default documents use the normal request-scoped package snapshot and release
only ownership introduced by that request.

## Application Commands

`MainFrame` owns the stable application menu structure. The shell keeps File,
Edit, Window, and Help as the compact top-level surface, including
application-owned commands such as About.

Registered workspaces may contribute File, Edit, and Window subcommands but
cannot add or replace top-level menus. Activating a document changes only the
target of Save, Undo, and Redo. Workspace-local actions belong in that editor's
toolbar or panels rather than replacing the application menu bar.

## Editor Host Presentation

The Level Editor internal layout is viewport-first: Scene Viewport occupies the
left side, while World Outliner and Details form the right column. It contains
no Content Browser, Console, Activity History, status bar, or bottom drawer.
Reset Active Editor Layout affects only that workspace's internal panels.

MainFrame owns the singleton Content Browser and Console surfaces, Activity
History, notification toasts, the global status bar, and the selected bottom
Drawer tool. The drawer is anchored below the host dock space, retains the
existing focus-loss and drag-leave dismissal behavior, and submits each tool
body at most once per frame. Dock in Layout opens the selected tool as an
ordinary host-dockable window. Selecting a docked tool focuses it; selecting
the active drawer tool closes the drawer.

`Ctrl+Space`, the status-bar actions, Content Browser reveal requests, and the
application Window menu all route through MainFrame host state regardless of
the active workspace. Content Browser mutations are disabled during Play while
read-only navigation, search, reveal, and asset opening remain usable. The same
host policy reaches feature-owned import presenters, so a dialog opened before
Play cannot submit new work after Play starts. Console and Activity History
remain usable. Console polling, unread counts,
transaction feedback, notification aggregation, and toasts update once per host
frame even when the Level workspace root is hidden. Reset Editor Host Layout
restores global tool defaults without resetting any asset-editor layout.

## Workspace Registration
`DurinEd` owns the reusable editor workspace and document framework under
`Durin::Editor`. `WorkspaceTypes.h` defines IDs, document metadata, descriptors,
and asset routes; `Workspace.h` defines the implementation-facing `IWorkspace`
contract; and `WorkspaceManager.h` owns registration leases and document
lifecycle. Workspace modules register their workspace descriptor, implementation,
and asset-class routes as one scoped batch. The manager validates the complete
batch before mutation, and destroying its registration handle removes the routes
and documents owned by that module.

`MainFrame` derives the default host layout and singleton reopen commands from
registered descriptors. The Window menu lists every open document, marks the
active document, and exposes layout reset and workspace-specific panel commands
for the active editor; it does not name concrete editor root windows.

`LevelEditor`, `MaterialEditor`, `TextureEditor`, `StaticMeshEditor`, and
`SkeletalMeshEditor` own
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
unregisters SkeletalMesh, StaticMesh, Texture, Material, and Level integrations
in reverse composition order. Scoped browser extensions and feature-owned
dialogs retire before their callback gates, and each concrete thumbnail handle
drains its queued and in-flight leases before its workspace documents close.
The host then destroys Content Browser, which drains an admitted import and
releases its provider-neutral and source-thumbnail caches, followed by shared
workspace and notification state. Concrete editor modules may unload only after
those steps, so no route, document, provider, session, preview object, import,
or queued upload can retain module code.

## Document Ownership

`Editor::FDocumentTab` is document identity and presentation metadata, not a
resource controller. Each workspace remains responsible for loading, saving,
discarding, releasing, and drawing its resource.

`Editor::FWorkspaceRootWindow` provides the shared root-window state transition,
while `Editor::FWorkspaceDocumentHost` composes that transition across the
per-resource documents used by Material, Texture, and StaticMesh editors. The
singleton Level workspace keeps its specialized internal dock-space lifecycle.

Material and Texture additionally compose
`Editor::FEditableAssetDocumentModel`. It owns active-resource identity,
document focus, package dirty/save/discard behavior, and forwarding to the
global transaction manager without depending on either concrete editor module.
Concrete workspaces still load exact asset types and run hooks before a switch,
close, discard, or save; Texture retains build cancellation and pending-build
rejection, while each editor retains its preview and type-specific diagnostics.
The read-only StaticMesh inspector and specialized Level workspace do not use
this editable composition because their document semantics differ.

Document open and close operations return explicit results. A deferred singleton
open preserves the current document until its replacement succeeds. For dirty
closes, `Editor::FWorkspaceManager` owns the single pending document and routes
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
action for an incompatible package. Engine's ordinary-save guard remains the
final persistence boundary if another caller bypasses the workspace policy.

The `StaticMeshEditor` workspace is presented as **StaticMesh Inspector** and is
strictly read-only. Exact `DStaticMesh` routes open closable per-resource
documents. Each document owns an isolated preview world and viewport, supports
orbit, pan, zoom, framing, and solid/wireframe presentation, and releases its
mesh component before destroying the scene. Its active document never becomes
dirty and never enables global Save.

Project-wide compatibility review is an explicit application tool rather than
a workspace document. `MainFrame` owns the non-modal `Tools > Asset
Compatibility Audit` window and its presentation actions. The Developer
`AssetMaintenance` module owns the UI-neutral deterministic audit batch;
`DurinEd` adapts it to the request-scoped worker, cancellation, request-serial
mailbox, and path-keyed result index. Opening or drawing the window does not start work; only `Run
Audit` captures the registry and reflection snapshots. The window can reveal a
selected package through the host-owned Content Browser, but neither `MainFrame`
nor a concrete asset workspace owns compatibility classification or a write
action. `DurinGame` does not select or link `AssetMaintenance`.

The Level workspace composition root constructs panels and document services in
dependency order, while panel and dialog presenters remain module-private
implementation details.

Class-filtered asset selection uses the `DurinEd` asset picker. The shared
editor asset payload carries canonical path and reflected class identity from
asset views to picker targets. Compatible drops use the picker's normal
assignment callback: hard references load and assign the object, while soft
references retain the canonical path without forcing a load. Incompatible,
malformed, disabled, or out-of-prefix drops do not mutate the property. Its
optional path prefix limits candidate enumeration without retaining a loaded
current selection.

## Related Documentation

- `Documentation/Editor/Architecture/PlayInEditorArchitecture.md`
- `Documentation/Editor/Architecture/ReflectedPropertyEditing.md`
- `Documentation/Editor/Design/UIStyle.md`
- `Documentation/Editor/Guides/StaticMeshInspector.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
