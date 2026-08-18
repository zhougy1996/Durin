# Editor Workspace Framework

Summary: Define editor workspaces, documents, panels, commands, layout, and host integration.

Modules: DurinEd, LevelEditor, MainFrame

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

`MainFrame` owns the editor root window. Its ordinary C++ host contracts live in
`Durin::Editor::MainFrame`; `FHostSettings` stores the native window size and
maximized state together with the global UI scale and color theme in the stable
`EditorHostSettings.yaml` file. When that file does not exist, the host uses
monitor-derived defaults and creates it when host display state is first
persisted. The reflected `DEditorEngine` remains in `Durin` and talks to the
host through `Durin::IMainFrameModule`.

`Editor::Level::FLevelEditorSessionSettings` persists only
Level workspace state such as viewport cameras, gizmo preferences, Content
Browser state, and Details layout. The two settings files have no compatibility
or migration coupling.

## Startup Bootstrap

`MainFrame` constructs and owns a lightweight native shell before loading
concrete editor modules. Persisted maximized state is applied while the window
is hidden and before native viewport creation. `DEditorEngine::Init()` then
advances the game-thread bootstrap to completion through a narrow Launch-owned
startup pump. Widget drawing observes state but never admits startup work, and
ordinary `DEditorEngine::Tick()` performs no bootstrap transition.

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

Destroying the default frame removes the bootstrap context, so no later startup
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

## Level Editor Presentation

The Level Editor's version-6 default internal layout is viewport-first. Scene
Viewport occupies the left side, while one right column places World Outliner
above Details. Content Browser, Console, and Activity History have no default
dock assignment and start closed. Reset Layout reconstructs that default,
reopens its three persistent surfaces, closes the transient tools, and clears
bottom-drawer state.

LevelEditor owns the selected bottom-drawer tool and presents Content Browser
or Console through the reusable MonaImGui drawer. Drawer selection and open
state are session-transient and do not extend `LevelEditorSession.yaml` or
`imgui.ini`. Each hosted panel separates its ordinary window wrapper from one
state-preserving content body, and that body is submitted at most once per
frame. Content Browser reveal requests select its drawer unless a separate
Content Browser window is already visible.

The Level Editor status bar exposes Content Drawer and Console on the left,
with workspace status (`Ready` when idle), notification actions, and Activity
History aligned on the right. `Ctrl+Space` toggles Content Drawer. Console
continues bounded log polling while hidden and reports a bounded unread
warning/error count without opening or taking focus. Activity History opens as
a non-docked floating ImGui window; notification updates, status presentation,
and toasts do not depend on that window being visible. Window > Panels opens
Content Browser or Console as an ordinary workspace-class window and remains
the recovery path for every surface.

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

`Editor::FDocumentTab` is document identity and presentation metadata, not a
resource controller. Each workspace remains responsible for loading, saving,
discarding, releasing, and drawing its resource.

`Editor::FWorkspaceRootWindow` provides the shared root-window state transition,
while `Editor::FWorkspaceDocumentHost` composes that transition across the
per-resource documents used by Material, Texture, and StaticMesh editors. The
singleton Level workspace keeps its specialized internal dock-space lifecycle.

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
