# Asset Definitions And Editor Sessions

Summary: Define reflected asset-type discovery, metadata-based opening, owned editor sessions, toolkit lifetime, and host integration.

Modules: DurinEd, TextureEditor, MainFrame, ContentBrowser

Last reviewed: 2026-11-06

## Definition Discovery And Dispatch

`DAssetDefinition` is immutable type-level behavior. Its const metadata and
capability queries do not load asset packages or construct editors. Eligible
definition CDOs register in `PostInitProperties`; `BeginDestroy` removes only the
same object generation. Scoped dynamic registrations retain their object across
GC and may safely outlive their registry. The process registry rejects duplicate
exact class paths, reports membership revisions, and resolves exact definitions
before nearest ancestors. Ancestor lookup is uncached. Definitions from known
modules are unavailable for dispatch unless the provider is Active; reflected
types without a managed module record support direct native fixtures.

`DAssetEditorSubsystem::OpenAssets` takes explicit requesting subsystem context,
object-path/class metadata, method, host/mode and origin. It preserves per-item
results, deduplicates selections, rejects recursion, and retains definitions
across callbacks. It sends each definition one ordered unique metadata batch and
expands results back to original input order. Conflicting metadata is rejected.
A definition rejection is final; absent definitions return Unhandled. Object convenience opens focus an existing
primary Edit instance first; a View instance cannot intercept an Edit open.
Path opens use resident objects or registry metadata,
falling back to execution-time loading when class metadata is unavailable.

`GetAssetOpenSupport(FAssetOpenSupportArgs)` returns the requested method, support
and optional required toolkit mode, following the [UE response shape](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/AssetDefinition/FAssetOpenSupport).
It is metadata-only. Dispatch resolves the effective mode before executing the
definition: a required mode overrides the requested preference, an explicit host
supplies its own mode, and conflicting explicit hosts are rejected. Single-session
groups combine requirements before any provider executes. Origin is diagnostic
text; `bIsRestore` explicitly identifies restore operations.

Pending definition work registers an identity and cancellation callback with
`BeginOpenRequest`. `GetOpenRequest` observes completion; cancellation refusal
keeps work/provider retention intact, and late completion is rejected. Deferred document candidates link to the same request identity. Publication
updates asset associations before finishing the request; a failed/cancelled
replacement retains the previous document. Terminal history retains the most
recent 256 outcomes and never evicts pending work. Ordinary session creation
requires synchronous initial documents; Level creates its explicit empty host
document first and delegates later asynchronous scene changes to its controller.
`FAssetOpenArgs::Assets` is the only selection. `bCreateSingleSession` requests
one compatible session (including grouped restore); the selected definition is
invoked once. Default execution checks compatible editor classes, loads each
unique asset once and initializes one session. Ordinary batches create/focus
individual sessions. A group with unavailable metadata is rejected before
execution, and a failed group load publishes no session. Level rejects multi-scene
grouping. Pending outcomes retain their originating Edit/View method.
`DAssetDefinitionDefault` supplies common display/category metadata and ordinary
open execution. Definitions may select a feature session class; the default is
the generic reflected-property session, supporting Edit and View. A project type
can inherit this behavior without adding central dispatch code. Asset load and
compatibility rejection remain authoritative.

## Session And Toolkit Ownership

`DAssetEditorSubsystem` strongly owns committed `DAssetEditor` objects per
`DEditorEngine`. Each session is the single externally indexed
`IAssetEditorInstance`, may edit several assets, and may be primary or secondary.
`FToolkitManager` indexes weak toolkits; host lookup derives from their owning
session. The session is authoritative for its host binding and derives its mode
from the host.
A session is admitted only after all initial documents and host attachment
succeed. `InitializeAssetEditor` admits a caller-configured reflected session
through the same validation and publication path as class-based opening. It
rejects foreign editor outers and reused sessions. Failed initialization publishes neither session nor asset associations.
Switching settles the previous toolkit's previews; a deactivation veto rejects
new session creation. Open, focus, close, frame drawing and extension commands enter the host operation
scope, with GC deferred across the public call.
Document models bind their history to the requesting editor. Nested asset
callbacks select a scoped transaction editor for existing graph/property APIs,
restoring the prior context after reentry without changing process `GEditor`.
An expired bound editor cannot redirect commands to another editor.

A session owns its toolkit and document controller. Toolkit callback/frame leases
can extend C++ lifetime; the toolkit retains its controller and definition
provider, while the controller stores only a weak toolkit and its callback holds
a weak reflected session. These references form no ownership cycle. A toolkit
binds only once, even after its weak owner expires. Closing removes external
associations and live-toolkit membership before releasing session UI ownership.
An escaped frame lease does not keep the reflected session alive or allow later
notifications through a retired owner.

Each session-local `FEditorDocumentManager` has one attached toolkit, no registered
asset routes, and plain collector-visible document roots rather than editor-instance
adapters. Its reload participant atomically migrates those roots; publication
refreshes toolkit references and external associations. Package relocation maps
canonical asset/subobject paths while preserving export names, not just package
strings. `OnResourceIdRemapped` updates feature caches and active-resource state.

## Methods, Close Policy, And Hosts

`FAssetEditorToolkit` guards Save, Discard, Undo, Redo and dirty/save capability
before calling feature hooks, including rejecting commands from retired frame
leases. View is fixed when the toolkit binds to its session.
Texture property contexts are read-only, mutation/reimport/build controls are
disabled, and direct save/reimport entrypoints also reject View. Read-only document
activation does not install transaction flags or package checkpoints. Edit and
View root identities differ; existing Edit root keys are preserved.
Session admission combines its feature support policy with applicable asset
definition requirements before creating a host or toolkit. Direct creation cannot
bypass read-only or required-mode restrictions. Host transfer uses the same check.
Material graph and Level definitions/sessions share stateless feature policies;
toolkits no longer expose duplicate support methods. Material and MaterialFunction
graph sessions support Edit only; unsupported View opens are explicit rejections. Collection, VolumeTexture and generic property
sessions support both methods. StaticMesh inspection remains immutable in either.

Each controller retains its own pending close after a failed save. The shell
uses `DocumentUI::FindPendingClose` to present only the oldest outstanding close
across session controllers. Request sequence prevents another session
or a changed container order from retargeting an already visible prompt. Closing
the last internal document retires its session on reconciliation.

`IToolkitHost` owns presentation attachment, focus and close policy rather than
asset ownership. `FNonWindowToolkitHost` supplies semantic standalone/world-centric
modes for direct hosts; dirty or pending close prevents host retirement. MainFrame
installs an editor-local host factory with one shared world-centric host.
Standalone hosts remain independent per session. Detaching one world-centric
toolkit preserves the other attachments. Standalone roots force independent native
Mona/ImGui viewports; world-centric roots attach to the Level-owning MainFrame dock.
The native viewport close bridge authorizes closure through the session first,
keeping a dirty or pending window alive. Scoped generation-aware policies are
removed on detachment and backend shutdown. A standalone native host attaches one
session; tools extend that session rather than creating another root.

A grouped session has one root with internal document tabs. Resource paths identify
edited assets; a process-unique runtime `WindowId` isolates each session's root,
panels and docking. Rename, group membership and host transfer do not change that
window identity. `GetLayoutName`/`GetLayoutVersion` declare a stable toolkit layout;
`GetLayoutProfileKey` scopes it by method, host mode and explicit role. Profile
identity does not include assets or group hashes. Additional identical sessions
still require distinct named roles. Transfer settles previews and rejects failed
attachment or a destination session collision before detaching its previous host.
MainFrame routes active commands to the session toolkit. It includes all session
controllers in exit preparation/save and releases their UI before feature module
integrations retire. This teardown does not execute asset Discard.

## Panels And Tool Extensions

`FBaseAssetToolkit::RegisterUIExtension` atomically admits unique panel and command
identities. Its move-only token withdraws that generation and can outlive the
toolkit. Extensions do not register asset routes or own another session. Commands
require the current active session; mutating commands reject View. Eligibility
callbacks may withdraw their registration; execution resolves membership again.
Closing a session clears registered callbacks even when a frame retains its toolkit.

Registered panels use session-specific dock classes and window names. The Window
menu reopens panels; reset restores default visibility and the active live dock
layout. YAML files under launch configs `AssetEditorPanels/` store visibility by
layout-profile hash. Writes merge edited panel keys so stale windows cannot replace
other saved choices. Existing windows retain independent live state; newly opened
windows use saved editor-profile defaults.

`RestoreDockLayout`/`SaveDockLayout` copy named dock templates through existing
ImGui docking/settings APIs. Only the active toolkit updates the shared template.
Runtime window names and root geometry are not saved. Host changes select the
new profile and discard only that session's live dock spaces. No separate layout
registry or dock-tree persistence format exists.

Material/Function preferences and graph viewports use version-3 files under
`MaterialEditor/Profiles/`, keyed by the same stable profile. Viewport records keep
resource paths and merge individual edits, preserving writes from other windows.
Authoritative relocation rekeys live viewports and stored records, including when
material windows are closed. Preference-page IDs remain unique to live windows;
closing a material session withdraws its callbacks.

Old path/group-keyed layouts are intentionally abandoned at the user's direction.
There are no migration keys, aliases or fallback reads. Session snapshots retain
only assets, methods, host modes and roles; window identity and geometry are not
added. Native real-window interaction remains unqualified while explicitly skipped.

Browser type queries read definition display name, icon, color and category paths
without loading assets. Nested category paths produce prefix filters, including
categories contributed by project definitions. Definition metadata supplies all
production asset editor families, including Level. Import, creation, thumbnail rendering
and menu execution remain separate registrations.
`RegisterAssetTypeExtras` supplies optional rich details and thumbnail badges
independently of display metadata. Browser invocations carry a weak requesting
host lifetime; queued actions and import-result callbacks cannot access a stopped
browser. Feature import controllers cancel/drain when their requesting host ends.

## Lifecycle And Session Restoration

`Subscribe` returns a scoped, non-owning lifecycle subscription. Value payloads
carry canonical metadata and weak object keys. Requested precedes definition
execution; Opened follows association publication; AssetRemoved follows index
removal; Closed follows retirement. Subscriber snapshots permit callbacks to
withdraw subscriptions or close other sessions. Association reconciliation is
incremental, avoiding false close/open churn on ordinary refresh.

Usage is local to the editor lifetime: committed asset-association opens, explicit
focus notifications, retired sessions and elapsed editing time. No external
telemetry is sent. Recent assets are canonical, unique and limited to 32; opens
and focus update recency, and authoritative relocation remaps entries.

MainFrame automatically restores supported ordinary sessions and saves a version-1
snapshot on orderly host retirement. Files live under the launch config directory
in `AssetEditorSessions/<project-path-hash>.yaml`; an embedded project identity
also rejects accidental cross-project loads. Snapshots contain asset groups,
Edit/View, Standalone/WorldCentric, primary status, stable roles and activation
order, with no live document IDs, pointers or provider leases. Restore uses
`OpenAssets`; missing assets/providers and rejected methods produce per-item
results and user-visible warnings. Compatible groups reuse one feature session.
Level is excluded and rejects Restore-origin requests, preserving configured
startup-Level precedence and the engine's readiness owner.

Focus waits for presentation readiness, including engine Ready and actual native
standalone-window creation. A weak pending focus target is consumed once or
cancelled by explicit opening/focus; no persistent restore session owner is added.
Browser extension invocation and drawing receive their owning editor/host
context. Scoped withdrawal stops host-owned dialogs before project retirement.

## Production Migration Boundary

Texture2D uses automatically registered `DTextureAssetDefinition` and on-demand
`DTextureAssetEditor` sessions. Its preview, compile, save/discard, property editing
and reimport services remain feature-owned. Reimport callbacks use weak toolkit
leases and reject retired sessions. Closing View does not cancel compilation;
closing Edit preserves compilation still observed by another editor. VolumeTexture,
Material, MaterialInstance, MaterialFunction, Collection and StaticMesh also use
on-demand reflected sessions. TextureCube metadata is definition-backed and its
View method uses the generic inspector. Ordinary feature module startup registers
independent creation/import/details/thumbnail contributions. MainFrame loads the
built-in modules without connecting toolkit batches or import callbacks.
Level uses `DLevelAssetDefinition` and a specialized `DLevelAssetEditor` with
one stable LevelEditor document. Its scene controller still owns loading,
confirmation, map cancellation, PIE restrictions, startup association and first
presentation. The Level toolkit only supports Edit in its owning world-centric
host. Closing/reopening that surface does not introduce a second scene-loading
route. The per-editor Level subsystem retains host settings and browser callbacks
for reopening; each configured reflected Level session receives that context before
initialization. The module has no host/session map, and lookups use committed
asset sessions. Module startup retains only independent feature registrations.
Registration batches/handles, route tables, singleton descriptors, workspace
instance adapters and shell document registries have been removed.

Full native interaction qualification and remaining parity work stay open in
the [UE asset editor architecture plan](../../../Plans/UEAssetEditorArchitecture.md).

The [subsystem host contract](EditorSubsystems.md) owns service admission and
retirement. The [workspace framework](WorkspaceFramework.md) retains the current
startup, document and presentation contracts.
