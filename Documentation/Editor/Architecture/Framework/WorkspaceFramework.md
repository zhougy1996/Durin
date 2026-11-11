# Editor Framework, Toolkits and Documents

Summary: Define editor toolkits, document ownership, tab presentation, panels, commands, layout, and host integration.

Modules: AssetMaintenance, DurinEd, LevelEditor, MainFrame, MaterialEditor, StaticMeshEditor, TextureEditor

This document defines editor-host ownership, persistent settings, application
commands, session integration, tab presentation, and document lifecycle contracts.

## C++ Ownership Boundary

Shared editor infrastructure lives directly in `Durin`: toolkits, documents,
tabs, transactions, property editing, Actor/Component edit contracts, asset
retention, import support, previews, thumbnails, notifications, and settings.
Consumers and forward declarations use the owning namespace without compatibility
aliases. Existing responsibility-specific groups such as `Durin::TabLayout`,
`Durin::DocumentUI`, `Durin::AssetPicker`, and `Durin::DocumentPrivate` remain.

Concrete feature ownership follows this mapping:

| Previous namespace | Owning namespace |
| --- | --- |
| `Durin::Editor::Level` | `Durin::LevelEditor` |
| `Durin::Editor::Material` | `Durin::MaterialEditor` |
| `Durin::Editor::Texture` | `Durin::TextureEditor` |
| `Durin::Editor::StaticMesh` | `Durin::StaticMeshEditor` |
| `Durin::Editor::MainFrame` | `Durin::MainFrame` |
| `Durin::Editor::ContentBrowser` | `Durin::ContentBrowser` |
| `Durin::Editor::Startup` | `Durin::EditorStartup` |
| `Durin::Editor::AssetPicker` | `Durin::AssetPicker` |
| `Durin::Editor` shared contracts | `Durin` |

Feature-local `Workspace`, `Helpers`, `Testing`, `GraphEditInternals`, and other
implementation groups remain under their owning feature. Shared thumbnail and
source-reference internals use `Durin::ThumbnailPrivate` and
`Durin::SourceReferencePrivate`, respectively. Editor feature names
include `Editor` where needed to distinguish them from runtime asset domains.
The existing root-owned module entry points and reflected runtime classes stay
in `Durin`. Editor-only reflected asset definitions, asset editors, and thumbnail
renderers follow their owning C++ namespace; they are discovered by reflected
class relationships and `StaticClass()`, not persisted namespace strings.
Module lookup strings, asset class paths, and persisted window/document/layout
identities remain independent of this C++ migration.

## MainFrame Ownership

`DurinEd` owns the implementation-neutral `Durin::IMainFrameModule` lifecycle
contract and the bootstrap states under `Durin::EditorStartup`. `DEditorEngine`
uses this interface to drive main-frame startup and shutdown. `MainFrame` depends on that contract and owns the editor
root window plus its concrete implementation under
`Durin::MainFrame`. `FMainFrameSettings` stores the native window size and
maximized state together with the global UI scale and color theme in the stable
`EditorHostSettings.yaml` file. The historical settings filename and dock/window
identities remain unchanged for compatibility; C++ APIs use MainFrame naming. When that file does not exist, the host uses
monitor-derived defaults and creates it when host display state is first
persisted. The reflected `DEditorEngine` remains in `Durin` and does not depend
on `MainFrame` headers.

`LevelEditor::FLevelEditorSessionSettings` persists only Level workspace state
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

## Startup Initialization

`DEditorEngine::Init()` constructs MainFrame through
`IMainFrameModule::CreateDefaultMainFrame()`, then initializes project services
and workspaces synchronously. MainFrame applies persisted maximized state while
the primary native window is hidden and creates its ordinary viewport. No shell
first-presentation gate, hidden-host bypass, startup-phase transition table, or
per-frame editor bootstrap pump exists.

Project providers and roots start before the initial content scan. DurinEd
reconciles Editor subsystems after each module batch and before creating the
corresponding consumers. `IMainFrameModule::LoadWorkspaceModules()` starts
workspace providers; `InitializeWorkspaces(OutError)` creates their consumers,
clearing the diagnostic on success and preserving it on failure. Callback-driven
shutdown stops subsequent initialization. Project Browser skips project work.

Editor publishes Ready or Failed service readiness before `Init()` returns.
Expected project/workspace failures retain the interactive browser/error UI;
base host construction failures use the normal partial-startup unwind. Widget
callbacks only present state and never advance initialization. The runtime
[lifecycle contract](../../../Runtime/Application/RuntimeLifecycle.md) owns the
process result and shutdown ordering.

Once services are Ready, DurinEd resolves the optional default Level and submits
one ordinary asset-editor document request. LevelEditor owns unsaved-change
confirmation, asynchronous MapChange, document association, transaction revision
handoff and viewport restoration through the same path as later opens. Empty
default-Level configuration submits no request. No startup-specific workspace
callbacks or separately owned loading operation exist. Document failure leaves
the usable workspace available and is reported by the normal document UI.

Normal editor ticks drain tools/scripts, update Worlds, then call
`IMainFrameModule::TickMainFrame` before rendering and ticking notifications.
MainFrame updates window settings, project upgrades, documents, Python snapshots,
Console receipts/logs and transaction feedback there. These services do not
require drawing and continue on minimized frames; modal continuations do not
reenter editor ticks. Python/MCP providers own service startup and shutdown.

Automation separately observes the ordinary initial request's outcome and its
current document association. A completed open is historical success even if its
document subsequently closes; that closure can fail automation's initial-document
requirement without changing the request result. Scene readiness then uses
`FSceneViewport::QueryPresentation` with the current generation and the exact
composition window supplied by Launch. Auxiliary windows, stale receipts and
replaced mappings cannot satisfy the gate. With no scene, automation waits for
the main window's successful presentation. These checks do not read profiling or
consume resize/presentation-policy requests.

Shutdown closes admission, invalidates document callbacks and retires workspace
consumers before MainFrame and presentation resources. Async document completion
remains owned by the ordinary frame; it cannot publish through a retired host.

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
