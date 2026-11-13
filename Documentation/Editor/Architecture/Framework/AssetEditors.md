# Asset Definitions And Editor Sessions

Summary: Define reflected asset-type discovery, metadata-based opening, owned editor sessions, toolkit lifetime, and host integration.

Modules: DurinEd, LevelEditor, TextureEditor, StaticMeshEditor, MainFrame, ContentBrowser

Last reviewed: 2026-11-13

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
object-path/class metadata, method and host/mode. It returns a command boolean,
deduplicates selections, rejects recursion and retains definitions across callbacks.
Each definition receives an ordered unique batch. Invalid ordinary inputs do not
prevent valid inputs from executing, but make the overall return false; a requested
single-session group rejects atomically at metadata preflight. There is no per-input
result DTO, request ID, completion table, terminal history or result-refresh API.

Definition `OpenAssets` returns UE's `EAssetCommandResult` (`Handled` or `Unhandled`).
An unhandled command does not fall back to a generic editor. Default definitions
report load/construction diagnostics at the execution site; presentation callers may
show an immediate aggregate failure. These are command returns, not asynchronous
completion receipts. In particular Level may accept work that finishes on later
frames; callers needing scene completion observe the Level/document state.

`GetAssetOpenSupport(FAssetOpenSupportArgs)` returns the requested method, support
and optional required toolkit mode, following the [UE response shape](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Editor/AssetDefinition/FAssetOpenSupport).
It is metadata-only. Dispatch resolves the effective mode before executing the
definition. A required mode overrides the preference; an explicit host supplies its
mode and incompatible explicit hosts reject. Groups combine requirements before
execution. `bIsRestore` identifies restoration. `bPromptToSave` is execution policy
for interactive Level commands; startup and programmatic calls leave it false.

Level owns delayed scene loading, confirmation and cancellation. Generic document
ownership publishes deferred candidates through `CompleteDeferredDocumentOpen(Id,
bSucceeded)` and exposes current pending state; it does not forward completion to an
asset-request service. Toolkit ownership retains the provider during the operation.
The engine's existing scene loading implementation remains shared with runtime
consumers; the editor adds no requirement to preserve a complete old-scene graph
on loading failure or cancellation.

The registry remains responsible for definition discovery, registration and class
lookup; it does not execute opens or own editor instances. `DAssetDefinition::OpenAssets`
is the public type-level opening operation, following UE's registry/definition
responsibility boundary. Default reflected editing and direct StaticMesh inspection
share `DAssetDefinitionDefault::ExecuteOpenAssets`, a private implementation for
selection compatibility, loading/retention, instance matching and grouping.
The protected `OpenAssetsWithToolkit` convenience method takes a Toolkit identity
and construction factory. A direct definition reports no reflected editor class,
so mixed generic/direct groups reject rather than opening through a generic fallback.
Compatibility and reuse policies are not public callback
interfaces and no extra registry or controller is introduced.

New-instance focus belongs to subsystem admission. The shared Definition execution
only focuses reused instances, so both new and reused opens publish one Focused
event per effective editor open. Grouped assets create/focus one editor; ordinary
selections retain their per-editor behavior.

Registry lookup uses `GetAssetDefinitionForClass` for either a loaded `DClass` or
an unloaded class-path identity. `RegisterAssetDefinition`,
`UnregisterAssetDefinition` and `GetAssetDefinitionVersion` expose registration
and invalidation; scoped registration tokens and automatic CDO registration retain
their existing lifetime contracts.

## Session And Toolkit Ownership

`DAssetEditorSubsystem` coordinates two editor-instance shapes per `DEditorEngine`.
Texture, Material and Level use reflected `DAssetEditor` owners; each owns its
Toolkit and is the externally indexed `IAssetEditorInstance`. StaticMesh Inspector
uses a directly owned `FAssetEditorToolkit` implementing `IAssetEditorInstance`,
without allocating a reflected editor. The subsystem strongly retains committed
direct Toolkits; hosts borrow them weakly. A reflected Toolkit reports its optional
owner as the externally indexed instance and is not independently registered. Both shapes support
multiple assets, primary/secondary roles and the same definition/open pipeline.
Toolkit enumeration derives from committed editor ownership; no secondary weak
ToolkitManager index is maintained. The Toolkit owns host binding and derives its
mode from the host. `GetAssetToolkits` creates a bounded strong snapshot from the
two actual ownership collections; it is not a second registration index.
Primary and role inputs exist only in `FAssetOpenArgs`; `GetNormalizedRole`
selects the default secondary role once. Asset-set identity ignores ordering and
duplicate input objects through the shared `HaveSameEditedAssets` helper.
A session is admitted only after all initial documents and host attachment
succeed. `InitializeAssetEditor` admits a caller-configured reflected session
through the same validation and publication path as class-based opening. It
rejects foreign editor outers and reused sessions. Failed initialization publishes neither session nor asset associations.
Switching settles the previous toolkit's previews; a deactivation veto rejects
new session creation. Open, focus, close, frame drawing and extension commands use
internal callback admission and transaction context. This dispatcher neither tracks
callback depth nor guards reentry or lifetime. Callbacks request GC; synchronous
collection requires a later owner-established safe point after the public call and
cleanup return.
Document models bind their history to the requesting editor. Nested asset
callbacks select a scoped transaction editor for existing graph/property APIs,
restoring the prior context after reentry without changing process `GEditor`.
An expired bound editor cannot redirect commands to another editor.

`FAssetEditorToolkit` owns the document controller, host, provider lease, open
method, role, runtime identity, activation time and whole-window close continuation
for both instance shapes. `InitAssetEditor` initializes either shape through one
implementation; subsystem admission remains responsible for validation and
publication. `PostInitAssetEditor` is the feature initialization hook. Focus,
activation, retirement, per-frame reconciliation and shutdown use the same Toolkit
path rather than forwarding back to a reflected owner.

`DAssetEditor` supplies the optional reflected lifetime, `CreateToolkit` and
`OnToolkitClosed` coordination. Its instance methods delegate to its Toolkit;
only the reflected object's initialized/closed lifetime markers survive Toolkit
release. `CreateToolkit` may borrow initialization arguments and the controller
while constructing its feature; these temporary inputs are cleared before normal
execution. No persistent second host, identity or close state lives in the owner.

Callers draw the Toolkit through its final `DrawToolkit` boundary; there is no
reflected-owner drawing wrapper. That boundary alone admits the callback and establishes the requesting
editor's transaction context; it retains the reflected owner, when present, and
Toolkit while feature drawing runs. A draw callback can request its own close,
and retired Toolkits reject later frames. Escaped C++ leases retain the controller
and provider but do not retain the reflected owner. The controller stores only a
weak Toolkit, including in its association callback. These references form no
ownership cycle. A Toolkit initializes only once, even after its weak owner
expires. Retirement disables callbacks and detaches UI before the subsystem
releases the actual owner and publishes its captured closing identity.

Each session-local `FEditorDocumentManager` has one attached toolkit, no registered
asset routes, and plain collector-visible document roots rather than editor-instance
adapters. Its reload participant atomically migrates those roots; publication
refreshes toolkit references and external associations. Package relocation maps
canonical asset/subobject paths while preserving export names, not just package
strings. `OnResourceIdRemapped` updates feature caches and active-resource state.

Direct Toolkit admission is read-only: intrinsically immutable inspectors may
use Edit or View, while otherwise editable feature Toolkits must be fixed to View.
Writable Edit is rejected before host creation, document callbacks or association
publication. Shared close continuation does not widen this admission contract;
the direct entrypoint remains unsuitable for general writable-editor initialization.

Direct Toolkit initialization is an explicit subsystem admission operation. It
checks the requesting editor, metadata support, asset-set identity and host mode,
opens documents and attaches presentation before publishing associations. Failure
retires the candidate and publishes nothing. Uninitialized and retired Toolkits
cannot draw or execute commands. Both shapes use the same final draw/command
boundary and owning-editor transaction context. Direct close removes associations
and ownership, detaches its host and retires UI; a borrowed Toolkit lease retains
its provider until released, while rejecting subsequent frames and commands.

## Methods, Close Policy, And Hosts

`FAssetEditorToolkit` guards Save, Discard, Undo, Redo and dirty/save capability
before calling feature hooks, including rejecting commands from retired frame
leases. View is fixed during reflected or direct Toolkit initialization.
Texture property contexts are read-only, mutation/reimport/build controls are
disabled, and direct save/reimport entrypoints also reject View. Read-only document
activation does not install transaction flags or package checkpoints. Edit and
View root identities differ; existing Edit root keys are preserved.
Session admission combines its feature support policy with applicable asset
definition requirements before creating a host or toolkit. Direct creation cannot
bypass read-only or required-mode restrictions. Reflected-session host transfer uses the same check. The direct read-only pilot
opens/restores in either host mode; live direct-Toolkit transfer is not added.
Material graph and Level definitions/sessions share stateless feature policies;
toolkits no longer expose duplicate support methods. Material and MaterialFunction
graph sessions support Edit only; unsupported View opens are explicit rejections. Collection, VolumeTexture and generic property
sessions support both methods. StaticMesh inspection remains immutable in either.

Each controller retains its own pending close after a failed save. The shell
uses `DocumentUI::FindPendingClose` to present only the oldest outstanding close
across session controllers. Request sequence prevents another session
or a changed container order from retargeting an already visible prompt. Closing
the last internal document retires its session on reconciliation.

EditorEngine advances session reconciliation once per admitted editor Tick before
the optional MainFrame update, including hosts with no window or World. It refreshes
document state, continues pending closes and completes restore focus through the
editor callback context. MainFrame only presents close confirmation; its result
is reconciled on the next editor Tick. Shutdown closes new admission immediately;
an already admitted document refresh or session close may finish, including its
remaining document work. The host checks shutdown after the complete operation
returns, before the next session or service phase. This is not per-callback
interruption or cancellation of the current close.

`HandleTicker`, `ShutdownAssetEditorSessions` and `DispatchEditorCallback` are
private integration, not public business APIs. EditorEngine alone drives updates.
Deinitialize releases sessions; the internal MainFrame lifecycle also releases
them after saving restoration state and before releasing Level host integration.
Standalone fixtures use owner-driven `PreExit`, not subsystem teardown helpers.
External callback owners establish their own host admission and transaction context.

Calling constraints, rather than a generic callback protection framework, define
reentry. Callbacks must not drive Editor Tick or another frame, call `PreExit`,
collect garbage immediately, change subsystem membership or unload providers.
Query callbacks such as `IsDocumentDirty` must not open/close documents, retire
sessions, mutate the queried collection or recursively refresh it. Queries may
request shutdown or GC without performing cleanup. Document-operation callbacks
must not structurally mutate that operation's borrowed document inputs or reenter
the same operation. Closing another session from a business callback and withdrawing
lifecycle subscriptions remain supported; snapshots and membership checks protect
those specific paths. Hosts retain resources until the full callback chain returns,
then execute teardown and physical GC at their idle boundary. These constraints
are caller obligations, not a callback-depth counter or automatic deferral scope.

`IToolkitHost` owns presentation attachment, focus and close policy rather than
asset ownership. `FNonWindowToolkitHost` supplies semantic standalone/world-centric
modes for direct hosts; dirty or pending close prevents host retirement. MainFrame
installs an editor-local host factory with one shared world-centric host.
Standalone hosts remain independent per editor instance. Detaching one world-centric
toolkit preserves the other attachments. Standalone roots force independent native
Mona/ImGui viewports; world-centric roots attach to the Level-owning MainFrame dock.
The native viewport close bridge authorizes closure through the effective editor instance first,
keeping a dirty or pending window alive. Scoped generation-aware policies are
removed on detachment and backend shutdown. A standalone native host attaches one editor instance; tools extend that instance
rather than creating another root.

A grouped session has one root with internal document tabs. Resource paths identify
edited assets; a process-unique runtime `WindowId` isolates each session's root,
panels and docking. Rename, group membership and host transfer do not change that
window identity. `GetLayoutName`/`GetLayoutVersion` declare a stable toolkit layout;
`GetLayoutProfileKey` scopes it by method, host mode and explicit role. Profile
identity does not include assets or group hashes. Additional identical sessions
still require distinct named roles. Transfer settles previews and rejects failed
attachment or a destination session collision before detaching its previous host.
MainFrame routes active commands to the effective Toolkit for either instance shape. It includes all session
controllers in exit preparation/save and releases their UI before feature module
integrations retire. This teardown does not execute asset Discard.

## Panels And Tool Extensions

Default viewport and details panels are selected solely by the optional
`CreateDefaultViewportPanel` and `CreateDefaultDetailsPanel` factories. Missing
factories allocate no panels; graph-only editors need no constructor opt-outs.

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

Lifecycle values carry a runtime `InstanceId` for both editor shapes. `Session`
is an optional reflected-object key and is empty for direct Toolkits. Notifications
never retain an editor. Runtime instance IDs do not enter restore snapshots.
Snapshot capture, restore focus, shell menus, drawing and close coordination
operate on both shapes through effective Toolkit/instance context.

`Subscribe` returns a scoped, non-owning lifecycle subscription. Value payloads
carry canonical metadata and weak object keys. Requested precedes definition
execution; Opened follows association publication; AssetRemoved follows index
removal; Closed follows retirement. Subscriber snapshots permit callbacks to
withdraw subscriptions or close other sessions. Association reconciliation is
incremental, avoiding false close/open churn on ordinary refresh.

Local usage counters and editing-time accounting are not maintained. Lifecycle
notifications drive MRU and subscribers without an unused statistics subsystem.

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
context through the same `FExtensionHost` value in `FExtensionInvocation::Host`
and presentation callbacks. It carries a weak browser lifetime and weak editor
subsystem; a missing editor never redirects to another host. Scoped withdrawal
stops host-owned dialogs before project retirement. `FScopedHostPresentation`
centralizes weak-owner matching, expiration and explicit stop for Mesh, glTF and
RoadWeaver dialogs. Expiration drains a dialog even when another host next draws;
explicit stop for another owner has no effect. Texture import uses the same
owner matching while retaining its feature-owned cancellation/draining protocol.

## Production Migration Boundary

Texture2D uses automatically registered `DTextureAssetDefinition` and on-demand
`DTextureAssetEditor` sessions. Its preview, compile, save/discard, property editing
and reimport services remain feature-owned. Reimport callbacks use weak toolkit
leases and reject retired sessions. Closing View does not cancel compilation;
closing Edit preserves compilation still observed by another editor. VolumeTexture,
Material, MaterialInstance, MaterialFunction and Collection also use
on-demand reflected sessions. TextureCube metadata is definition-backed and its
View method uses the generic inspector. StaticMesh overrides definition execution
to initialize direct Inspector Toolkits; repeated opens focus the matching
method/mode/role/asset selection. Mesh remains immutable in both Edit and View.
Its existing Engine package reload limitation remains an explicit Unsupported
failure without replacing the original graph; this migration adds no mesh reload
participant or new Engine capability. Ordinary feature module startup registers
independent creation/import/details/thumbnail contributions. MainFrame loads the
built-in modules without connecting toolkit batches or import callbacks.
Level uses `DLevelAssetDefinition` and a specialized `DLevelAssetEditor` with one
stable document. The definition forwards to `DLevelEditorSubsystem`. Programmatic
`LoadLevel` starts loading without an unsaved-changes prompt; interactive `OpenLevel`
resolves Save/Discard/Cancel first. `CancelLevelLoad` cancels the current Level
operation directly and settles its deferred document immediately; a later Engine
callback observes no pending action. Host shutdown may discard that callback.
Confirmation cancellation and load cancellation are no longer
public asset-request outcomes. These services use the same Level controller;
there is no second scene-loading implementation. Durin retains asynchronous loading
internally, so its load boolean means accepted/already active, not a promise that
rendering has completed. This timing differs from UE's conventional LoadMap return.

Startup stores only the configured Level path, observes current pending documents
and the actual editor World, then independently checks primary-window presentation.
No historical operation result can make a missing or different scene ready. The
Level subsystem depends on the asset-editor service for teardown ordering and
cancels its own work before retirement. MainFrame releases the Level owner before
retiring remaining asset Toolkits. Level still supports Edit in its world-centric
host and opts out of asset-session restore. Host settings/browser callbacks live
in the per-editor Level subsystem; there is no module-owned host/session map.

Registration batches/handles, route tables, singleton descriptors, workspace
instance adapters and shell document registries have been removed.

Definition registration withdrawal prevents new dispatch without closing existing
sessions. Live Toolkits retain their provider independently of the registry lease, including
while a Level document is loading. A retained toolkit
lease retains its provider until the lease itself is released. These are object
lifetime guarantees, not permission to unload provider machine code. Module unload
preflight rejects non-reloadable/process-resident providers before shutdown;
unsupported editor DLL reload remains disabled under the
`FModuleManager::UnloadModule` preflight contract.

Native interface interaction remains skipped and unqualified. It was explicitly
excluded from the completion scope of the
[UE asset editor architecture plan](../../../Plans/UEAssetEditorArchitecture.md);
completion of that plan is not a manual interface-acceptance claim.

The [subsystem host contract](EditorSubsystems.md) owns service admission and
retirement. The [workspace framework](WorkspaceFramework.md) retains the current
startup, document and presentation contracts.
