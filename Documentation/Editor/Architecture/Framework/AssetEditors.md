# Asset Definitions And Editor Lifetimes

Summary: Define asset opening, toolkit ownership, host integration, and editor closing.

Modules: DurinEd, LevelEditor, TextureEditor, StaticMeshEditor, MaterialEditor, MainFrame, ContentBrowser

Last reviewed: 2026-11-16

## Definition Discovery And Dispatch

`DAssetDefinition` supplies immutable type metadata and opening policy. Metadata
queries never load packages or construct editors. The process registry resolves
exact class paths before nearest ancestors, rejects duplicates, and dispatches
only through active providers. CDO registration is automatic; scoped registrations
retain their definition across GC and remove only their own generation.

`DAssetEditorSubsystem::OpenAssets` receives the requesting subsystem, asset
metadata, method, mode, and optional host. It deduplicates input and dispatches an
ordered batch per definition. Invalid ordinary inputs make the aggregate result
false without suppressing valid inputs; an explicitly grouped selection rejects
atomically at metadata preflight. Definitions return `EAssetCommandResult`.
Unhandled commands do not fall back to a generic editor.

`GetAssetOpenSupport` is metadata-only. Required modes override preferences;
incompatible explicit hosts reject. Group requirements are combined before
execution. Admission combines definition and feature policy before constructing
a host. Direct toolkits are read-only: immutable inspectors may accept Edit or
View; otherwise editable toolkits must use View. Material graph editors accept
Edit only. View rejects mutation, Save, Undo, and Redo.

Default definitions share selection, loading, reuse, and grouping through
`ExecuteOpenAssets`; direct definitions use `OpenAssetsWithToolkit`. New-instance
focus belongs to subsystem admission; definition execution focuses reused
instances. Each effective open publishes one focus event.

Definition withdrawal prevents new dispatch. Existing toolkits retain their
provider until their last C++ lease is released. This object lifetime does not
authorize unloading provider machine code; module-unload preflight still applies.

## Editor Instance And Toolkit Ownership

`FToolkitManager` persistently owns every admitted `FAssetEditorToolkit`.
`GetToolkits` returns an activation-ordered strong snapshot, optionally scoped to
one subsystem, including asset-less and deferred-document roots.

The ownership chain is:

- ToolkitManager strongly owns open toolkits.
- A toolkit owns its document controller and provider lease, and strongly retains
  its optional reflected `DAssetEditor` while open.
- `DAssetEditor` supplies reflected identity, `CreateToolkit`, and instance
  forwarding; its toolkit back-reference is weak.
- Toolkits and host attachments reference each other weakly. MainFrame owns its
  presentation hosts; the subsystem owns non-window fallback hosts. Custom host
  factories must provide a presentation owner.
- `DAssetEditorSubsystem` coordinates opening, non-owning asset/instance indexes,
  weak active selection, and restoration. It owns no parallel toolkit collection.

Texture, Material, and Level use reflected editors. StaticMesh exposes its toolkit
directly as `IAssetEditorInstance`. Both use the same admission, drawing, transfer,
and close paths. Subsystem business queries `FindEditorForAsset`,
`FindEditorsForAsset`, and `GetAllOpenEditors` cover asset associations rather than
all toolkit roots.

Admission publishes only after document initialization and host attachment
succeed. `InitializeAssetEditor` also accepts a caller-configured reflected editor,
rejecting foreign outers and reused instances. Failure closes partial UI and
publishes no associations. A toolkit initializes once.

Each toolkit owns its method, role, activation time, and process-unique
`WindowId`. Asset-set identity ignores order and duplicates. Identical editors
require distinct stable roles. Resource relocation preserves window identity;
document reload participants migrate roots and notify feature caches through
`OnResourceIdRemapped`.

## Methods, Close Policy, And Hosts

`CloseWindow` requests permission through `OnRequestClose`, whose default uses
document Save/Discard/Cancel. A veto or pending confirmation keeps the editor open.
`FToolkitManager::CloseToolkit` commits closing in this order:

1. Mark closed before invoking feature `OnClose` exactly once.
2. Release feature previews and UI, then detach the host.
3. Remove manager ownership and release the reflected-editor reference.
4. Remove subsystem associations and publish the captured close identity.

A temporary close lease keeps callback arguments alive. Retained closed toolkits
reject drawing and commands without keeping their reflected editor alive.
`IsEditorOpen` requires initialization, manager membership, and an admitting
subsystem; `IsClosed` records committed closing independently of destruction or GC.

Host destruction commits closing for attached toolkits without a veto.
`IToolkitHost` notifies the manager from its base destructor; the manager compares
a non-owning identity without dereferencing the destroyed host. Cached toolkit
mode survives host destruction.

`FAssetEditorToolkit::TransferToHost` handles both editor shapes. It settles
document operations, validates mode policy and destination identity, attaches the
destination, then detaches the source. Liveness is rechecked after callbacks.
Closing during attachment, detachment, or `OnHostChanged` fails transfer and
removes provisional attachments.

MainFrame supplies a shared world-centric host and independent standalone hosts.
Native-window closure first requests editor permission. Closing one world-centric
toolkit preserves other attachments. A grouped editor has one root and internal
document tabs.

EditorEngine refreshes documents and pending closes once per admitted Tick.
MainFrame presents the oldest pending close across controllers; failed saves keep
that controller's request pending. Closing the last document closes its toolkit
on reconciliation. Shutdown stops new admission but lets an admitted operation
finish before moving to the next toolkit or service phase.

PreExit settles document operations and commits editor closes before releasing
thumbnail previews and unloading rendering services. Feature `OnClose` releases
preview resources; preview draws hold local shared leases against reentrant
closing. Thumbnail resource release preserves renderer registrations for later
editor instances; final manager shutdown removes them. Cleanup must not depend
on a later GC pass.

`DrawToolkit` and command boundaries establish the requesting editor's transaction
context and retain active callback inputs. Nested contexts restore their caller
without changing `GEditor`. Callbacks may close another editor or withdraw a
subscription. They must not drive another frame, run PreExit or immediate GC,
change subsystem membership, or unload providers. Query callbacks must not mutate
their queried collections; document operations must not structurally mutate or
reenter their borrowed inputs. Owners perform teardown and GC after the callback
chain returns. See the [subsystem host contract](EditorSubsystems.md).

## Panels And Tool Extensions

Optional `CreateDefaultViewportPanel` and `CreateDefaultDetailsPanel` factories
supply default panels. `RegisterUIExtension` atomically registers unique panel
and command IDs; its scoped token withdraws only that generation. Commands require
the active toolkit, mutating commands reject View, and execution rechecks
membership after eligibility callbacks. Closing clears callbacks even if a caller
retains the toolkit.

Runtime window and dock IDs belong to each `WindowId`. Stable layout profiles use
`GetLayoutName`, `GetLayoutVersion`, method, host mode, and role, excluding asset
paths and group hashes. Panel visibility is stored under `AssetEditorPanels/`;
writes merge edited keys. Named dock templates use ImGui settings, with only the
active toolkit updating the shared template. Transfer selects the new profile
and resets that toolkit's live dock spaces.

Material preferences and graph viewports use version-3 files under
`MaterialEditor/Profiles/`, keyed by the same profile. Resource relocation rekeys
live and stored viewport records. Closing withdraws preference callbacks.

Content Browser reads definition metadata without loading assets. Import,
creation, thumbnails, and optional `RegisterAssetTypeExtras` contributions have
separate registrations. Queued browser actions and feature dialogs use weak host
context and cancel or drain when their owner stops.

## Lifecycle And Restoration

Lifecycle notifications carry a runtime `InstanceId`; the `Editor` field is an
optional reflected-object key, empty for direct toolkits. Scoped subscriptions
retain no editor. Requested precedes dispatch, Opened follows association
publication, AssetRemoved follows index removal, and Closed follows committed
closing. Subscriber snapshots permit withdrawal and closing other editors.

MainFrame saves version-1 restoration data under
`AssetEditorState/<project-path-hash>.yaml`, with an embedded project identity.
The `OpenEditors` field contains asset groups, methods, modes, primary
status, stable roles, and activation order. It stores no pointers, document IDs,
provider leases, runtime window IDs, or geometry.

Restore uses `OpenAssets` and reports missing assets/providers or rejected methods.
Focus waits for engine and presentation readiness, then runs once; explicit
opening or focus cancels it. Level opts out so configured startup-Level loading
retains precedence. Level owns its deferred loading and cancellation; an accepted
open does not imply rendering readiness. Startup observes pending documents and
the actual World rather than historical request outcomes.

The [workspace framework](WorkspaceFramework.md) owns startup and presentation
integration. Native real-window interaction remains unqualified; headless tests
do not establish manual interface acceptance.
