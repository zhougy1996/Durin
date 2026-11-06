# Editor Framework, Toolkits and Documents

Summary: Define editor toolkits, document ownership, tab presentation, panels, commands, layout, and host integration.

Modules: AssetMaintenance, DurinEd, LevelEditor, MainFrame, MaterialEditor, StaticMeshEditor, TextureEditor

This document defines editor-host ownership, persistent settings, application
commands, session integration, tab presentation, and document lifecycle contracts.

## C++ Ownership Boundary

Shared toolkit, document and tab APIs live directly in `Durin`, including
`IAssetEditorToolkit`, `FAssetEditorToolkit`, `FEditorDocumentManager`,
`FEditorDocument`, `FDocumentId` and `FTabManager`. Responsibility-specific helpers
remain under `Durin::TabLayout` and `Durin::DocumentUI`. There are no compatibility
aliases in `Durin::Editor`; consumers and forward declarations use the owning
namespace directly. Document controller internals stay in `Durin::DocumentPrivate`.

Transaction, property, asset, preview and interaction services retain their
existing `Durin::Editor` namespaces. Concrete editor implementation namespaces
also remain unchanged in this migration. Runtime and reflected object types
retain their existing identities. Module lookup strings and persisted
window/document/layout identities are independent of these C++ namespaces.

## MainFrame Ownership

`DurinEd` owns the implementation-neutral `Durin::IMainFrameModule` lifecycle
contract and the bootstrap states under `Durin::Editor::Startup`. `DEditorEngine`
uses this interface to drive main-frame startup and shutdown. `MainFrame` depends on that contract and owns the editor
root window plus its concrete implementation under
`Durin::Editor::MainFrame`. `FMainFrameSettings` stores the native window size and
maximized state together with the global UI scale and color theme in the stable
`EditorHostSettings.yaml` file. The historical settings filename and dock/window
identities remain unchanged for compatibility; C++ APIs use MainFrame naming. When that file does not exist, the host uses
monitor-derived defaults and creates it when host display state is first
persisted. The reflected `DEditorEngine` remains in `Durin` and does not depend
on `MainFrame` headers.

`Editor::Level::FLevelEditorSessionSettings` persists only Level workspace state
such as viewport cameras, gizmo preferences, and Details layout.
`ContentBrowserSettings.yaml` independently persists browser presentation.
When the new file is absent it starts from defaults and does not read retired
Level Editor browser keys.

## Settings Window

`MainFrame::FEditorSettingsWindow` presents editor preferences and project
settings in one resizable window with a category sidebar and a content pane.
`Edit > Editor Preferences` selects Appearance; the Level Editor's
`Project Settings` command selects General and Startup. Search filters pages
by category, setting keywords, description, and scope, including comma-separated
alternatives and exclusions prefixed with `-`.

`DurinEd` owns the game-thread-only `FEditorSettingsRegistry` and scoped
`FEditorSettingsPageRegistration`. Feature widgets register stable page IDs,
labels, descriptions, drawing callbacks, and optional reset/open callbacks.
Registrations retire before their widgets' settings dependencies are destroyed;
MainFrame does not own feature configuration or persist feature state.

The built-in pages expose Appearance, Display (VSync), Viewport and Controls,
Content Browser, Material Editor, and project General and Startup. Preferences
apply and save immediately through their existing stores, with visible save
errors. The Content Browser save callback returns success so failed writes from
the preferences page do not change its active presentation. Project default-level
edits use Apply and Revert and are disabled during Play. Selecting or reopening
the project page initializes its draft from the saved default level.

Restore page defaults resets only the preferences on that page. It preserves
window geometry, per-level cameras, browser navigation and pane widths, material
graph viewports, and unrelated settings. Project settings do not expose an
automatic reset action.

Low-frequency preference editing is centralized in this window. The browser's
view-options popup links to Content Browser preferences; the material Window
menu links to Material Editor preferences; rendering diagnostics reports VSync
read-only and links to Display. Right-clicking the viewport snap button or using
its overflow menu opens Viewport and Controls instead of a separate snap editor.
High-frequency contextual controls remain local: view switching, panel visibility,
camera speed, grid/statistics visibility, snap enablement, and Ctrl-wheel thumbnail
resizing. These controls continue to share the same state as their settings pages.

## Startup Bootstrap

`MainFrame` constructs and owns a lightweight native shell before loading
concrete editor modules. Persisted maximized state is applied while the window
is hidden and before native viewport creation. `DEditorEngine::Init()` then
creates the shell through `IMainFrameModule::CreateDefaultMainFrame()`.
The Launch-owned startup pump calls `DEditorEngine::AdvanceStartup()` to advance
the game-thread bootstrap. `DEditorEngine` is the sole owner of startup phase,
terminal status, and diagnostic; MainFrame reads these values for presentation
and does not maintain a second state machine. Widget drawing observes state but never admits
startup work, and ordinary `DEditorEngine::Tick()` performs no bootstrap
transition.

`AdvanceStartup()` only advances a pending startup. Once ready, the ordinary
editor tick drains Toolset invocations, pumps Python/scripts, updates worlds, then
calls `IMainFrameModule::TickMainFrame(DeltaSeconds)` before rendering. MainFrame
updates native-window settings, project upgrade work, document state, Python
snapshots, Console receipts/logs, and transaction presentation feedback there.
After that optional UI update, the editor ticks its notification subsystem once
using the engine frame delta. Notification timing does not require MainFrame.
Python/MCP subsystem providers own service startup and shutdown; Python startup
script submission also runs without the panel. MainFrame only observes the job
and owns its presenter interactions.
These updates run without drawing, including minimized frames and while the
Project Browser or individual tools are hidden. Shutdown suppresses the normal
main-frame tick; modal render continuations do not run another editor tick.

Project startup follows the forward-only sequence
`ConstructingShell -> WaitingForFirstPresent -> InitializingProject ->
LoadingWorkspace -> LoadingDefaultDocument -> Ready`. Workspace registration or
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
`IMainFrameModule::InitializeWorkspaces(OutError)` synchronously registers and
opens workspaces, returning `bool` with a diagnostic on failure. Once workspaces
are available, DurinEd resolves the project's configured default level through
`Editor::Startup::ResolveStartupLevel()`, then `DEditorEngine` starts one ordinary
`OpenLevel()` request and owns its pending/completed/failed state. Resolution
preserves settings diagnostics, unique top-level Level validation and redirects,
and verifies that the resolved target belongs to the selected project mount.
An absent default level succeeds without starting a map change. MainFrame and
LevelEditor do not select or load the startup level; there is no LevelEditor
module interface in DurinEd.

LevelEditor supplies `FStartupLevelCallbacks` for pre-commit viewport preparation
and post-commit document association, transaction checkpoint handoff, viewport
state restoration and initial presentation preparation. These callbacks do not
own startup status or configuration. They carry a weak controller lifetime;
closing a document invalidates them, and workspace destruction unbinds them.
A missing or retired workspace cannot make startup ready. The engine invokes
post-commit preparation once, then waits for the existing scene-presentation
receipt without reopening or reassociating the level. Interactive opens retain
their document confirmations and use the same map-change and association paths.

MainFrame exposes the shell's first-presentation check. `DEditorEngine` retains
only a weak reference to the exact startup window supplied by Launch. While
waiting for scene presentation it gets the current renderer from MonaApplication
and calls `FMonaRenderer::IsOutputPresented(Window, Receipt, Generation)`.
The renderer checks its current window mapping and the receipt's nonzero output
generation and presenting viewport internally. Missing applications, renderers,
windows or mappings, auxiliary-window presents, stale generations and receipts
for replaced viewports do not make startup ready. Shutdown clears the window
reference before retiring the main frame. Window creation and produced scene
output alone do not satisfy the first-presentation gates. Queries do not prepare
draws or consume pending resize/presentation-policy changes.
The Project Browser receives the engine's startup diagnostic as display input,
keeping its own interactive project-opening errors independent.

After Level workspace registration, ordinary feature-module startup and default
workspace opening succeed, the engine enters `LoadingDefaultDocument` and yields
to the startup pump before resolving and loading the configured default level.
There is no idle workspace-ready phase. Project modules, initial content and
subsystem reconciliation run once in `InitializingProject`, after the shell
presentation gate and before workspace registration. Subsystem reconciliation
runs outside the project-admission operation scope so it can safely admit new
providers; completion is represented by the next phase rather than a separate
project-content readiness flag. Map-change failure
or cancellation enters terminal startup `Failed`; a successful commit still
requires document preparation and actual scene presentation.
Missing, incompatible, corrupt, or unactivatable default levels publish document
`Failed`, force the overall bootstrap to terminal `Failed`, preserve one
actionable owning diagnostic, and fail engine initialization. Successful
`FEngineLoop::Init()` is therefore authoritative evidence that a project editor
has activated its configured default Level, if any, and is ready for ordinary ticking.

Loading progress is phase-based rather than time- or byte-based. The compact
themed view reports shell presentation, workspace activation, and default-Level
opening using the existing asynchronous map-change phase and elapsed time.
Workspace registration can still block the game thread; the UI does not
fabricate intra-operation percentages.

Editor shutdown suppresses startup advancement, discards map-change callbacks,
and clears startup-level workspace hooks before destroying the MainFrame context. MainFrame then stops request admission and unwinds
workspace integrations in the reverse order described below. Partially loaded
default documents use the normal request-scoped package snapshot and release
only ownership introduced by that request.

## Application Commands

`MainFrame` owns the stable application menu structure. The shell keeps File,
Edit, Window, and Help as the compact top-level surface, including
application-owned commands such as About. The Help → About Durin window
shows the engine version and the active RHI backend reported by RHI, including
Vulkan after an automatic Metal startup fallback.

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
frame even when the Level workspace root is hidden. Reset Editor Layout
restores global tool defaults without resetting any asset-editor layout.

## Asset Editor Session Integration

[Asset definitions and editor sessions](AssetEditors.md) own ordinary asset-type
metadata, discovery, open execution, toolkit ownership, hosts and panels.
MaterialEditor, TextureEditor and StaticMeshEditor publish independent scoped
contributions during normal module startup; MainFrame no longer connects their
workspaces or import callbacks. They do not support physical dynamic reloading.

LevelEditor creates its specialized owned session through the asset-editor
subsystem and publishes Level metadata/opening through its definition. The Level
session owns its document manager; startup, scene activation, PIE and project
switching remain with the existing engine/Level controllers. MainFrame has no
document registry or routing fallback; it enumerates committed sessions for menus,
drawing, close confirmation and exit preparation. Toolkit registration batches,
route tables and per-document editor-instance adapters have been removed from
production and fixtures. Lifecycle and project-scoped restore contracts are
specified in [asset editors](AssetEditors.md#lifecycle-and-session-restoration).

MainFrame shutdown stops browser admission and releases its UI-held thumbnails,
retires owned asset sessions after exit/save policy settles, and releases the
specialized Level integration. Ordinary feature registrations live with their
modules. Browser invocation lifetimes prevent callbacks into a retired host;
feature controllers drain admitted work before their provider modules stop.

## Document Ownership

`FEditorDocument` is document identity and presentation metadata, not a
resource controller. Each toolkit remains responsible for loading, saving,
discarding, releasing, and drawing its resource.

`FEditorTabWindow` provides the shared root-window state transition,
while `FTabManager` composes that transition across the
internal document tabs within session roots used by Material, Texture and Mesh editors. The
singleton Level workspace keeps its specialized internal dock-space lifecycle.

Material and Texture additionally compose
`FEditableAssetDocumentModel`. It owns active-resource identity,
document focus, package dirty/save/discard behavior, and forwarding to the
bound requesting-editor transactor without depending on either concrete editor module.
Concrete workspaces still load exact asset types and run hooks before a switch,
close, discard, or save; Texture retains build cancellation and pending-build
rejection, while each editor retains its preview and type-specific diagnostics.
The read-only StaticMesh inspector and specialized Level workspace do not use
this editable composition because their document semantics differ.

Document open and close operations return explicit results. A deferred singleton
open preserves the current document until its replacement succeeds. For dirty
closes, each `FEditorDocumentManager` owns its single pending document and routes
Save, Discard, or Cancel back to resource-specific toolkit callbacks.
`MainFrame` renders the oldest outstanding close in one shared confirmation modal. Repeated close requests
cannot create duplicate confirmations.

The Level document controller separates interactive `OpenLevel` (including the
unsaved-change decision) from `LoadLevel` (a specified path without a save
prompt). Its immediate results distinguish `AwaitingConfirmation`, `Loading`,
`Opened`, and `Rejected`; the workspace adapter maps both pending states to a
deferred document open. Completion publishes success only after activation and
viewport/session restoration. Concurrent level or project opens are rejected
without replacing the pending request. Default-Level startup uses the same load
path with the host's existing startup presentation instead of a second modal.

Interactive Level loading uses the startup presentation style: a centered
360-unit content column, centered level name and preparation phase, and an
8-unit indeterminate progress bar. Elapsed time and Cancel share a subdued
footer; the full target path is available on hover over the level name. Resource
counts are omitted because they do not measure completion.

Startup and Level loading project their authoritative bootstrap/scene-load
state directly into `FSlowTaskSnapshot` and reuse the `DurinEd` progress widget.
The Level controller presents a modal snapshot for interactive opens; MainFrame
startup presents inline progress without a second feedback lifecycle. Elapsed
time comes from the scene operation when available. MainFrame also observes the
default thread-local feedback context for generic slow tasks. See
[slow task feedback](../../../Runtime/Core/SlowTasks.md) for projection,
nesting, cancellation, and execution boundaries.

Cancellation and failure preserve the previous document; the modal closes when
the pending operation ends. Save confirmation is a separate modal preceding
loading. Dialogs are drawn even when the Level workspace has no visible root.
Loading and confirmation block Level edits and play shortcuts. Only ordinary
host frames advance and commit scene operations; modal callbacks observe
progress or request cancellation, and never run a nested engine tick or wait
loop. This preserves the [runtime scene-load contract](../../../Runtime/Application/RuntimeLifecycle.md).

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
strictly read-only. Its definition creates owned sessions with closable internal
documents. Each document owns an isolated preview world and viewport, supports
orbit, pan, zoom, framing, and solid/wireframe presentation, and releases its
mesh component before destroying the scene. Its active document never becomes
dirty and never enables global Save.

Project asset upgrades are an on-demand application flow rather than a
workspace document. MainFrame owns **Tools > Project Asset Upgrade...**, with an
explicit read-only check, recommendation preview, confirmed application, and
retained outcomes. Neither startup nor opening/drawing the window launches an
audit. **Check Project Assets** refreshes the registry and captures reflection
and package inputs; worker inspection remains asynchronous and cancellable.
The result list defaults to recommendations and issues, with unchanged assets
available on demand. Upgrade plans preserve revision, fingerprint, dirty-package,
and writable-mount checks in the Developer `AssetMaintenance` service shared
with offline `DurinAssetTool` commands.

Host document-operation updates reconcile and advance work outside ImGui draw.
Closing defers the flow and requests cancellation before another package; a
project identity change drains the old audit and clears old plans. Each package
is atomically published, so already completed writes survive cancellation or
later failure. Results remain available on reopening, and a new check is explicit.
The window can reveal a package through the host-owned Content Browser. There
is no separate asset-maintenance Toolset or automatic startup scan/cache in this
flow. `DurinGame` does not select or link `AssetMaintenance`.

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

- `Documentation/Editor/Architecture/LevelEditing/PlayInEditorArchitecture.md`
- [Property Details views](../PropertyEditing/PropertyDetailsViews.md)
- `Documentation/Editor/Design/UIStyle.md`
- `Documentation/Editor/Guides/StaticMeshInspector.md`
- `Documentation/Runtime/Application/RuntimeLifecycle.md`
