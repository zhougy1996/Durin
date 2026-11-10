# Editor Subsystems

Summary: Define per-editor native services, reflected PIE controls, Actor/Component editing, and notification ownership and update timing.

Modules: DurinEd, LevelEditor, MainFrame, DurinPython, DurinMCP, Engine

Last reviewed: 2026-11-10

## Host Contract

There is one active editor engine per process. Construction is inert;
ActivateEditor is the sole publication boundary and rejects repeated activation,
including activation of the same instance. Launch and headless fixture setup
call it explicitly. Full Init and subsystem initialization require that already
active editor and never publish one themselves. Shutdown releases activation only after callback and World work
is quiescent. A later editor lifetime can then activate with fresh consumer-owned cancellation state.

Native headless tests share Editor/EditorEngineTestSupport.h for explicit startup
and private test access. Ordinary fixtures construct real DEditorEngine instances;
specialized subclasses remain only for additional lifecycle probes. Startup-only
reflected subclasses and translation-unit-specific definitions of the same test
accessor are not needed.

`DEditorSubsystem` extends `DDynamicSubsystem` from the
[common subsystem contract](../../../Runtime/Engine/Subsystems.md) in DurinEd.
`FEditorSubsystemCollection` derives from `FDynamicSubsystemCollection`, which owns
live membership reconciliation while the common collection owns initialization
and cleanup. The single active `DEditorEngine` owns an Editor collection separately from its inherited
Engine collection. `GetEditorSubsystem<T>()` selects exact Editor types;
inherited `GetSubsystem<T>()` still selects Engine types. Editor services have
no implicit PIE World requirement and survive repeated PIE start/stop.

`FEditorSubsystemRegistration` accepts the common descriptor. Keep tokens in the
providing module. Registration and token destruction publish changes for the active editor
and later editor lifetimes; instances change only at a host safe point. Dependencies name
concrete Editor types in the same collection. The host enumerates
both collections for GC. Runtime Engine has no dependency on DurinEd.

Editor Init runs base Init, loads MainFrame plus Python/MCP service providers,
then initializes currently available Editor services before
CreateDefaultMainFrame exposes shell consumers. The constructor-created transactor is
available; the shell is not yet created. After first presentation, project admission
loads project providers and AssetForgeBuiltins, initializes project content, and
synchronizes Editor services. Workspace startup then loads all concrete workspace
modules, synchronizes services at a host safe point, and only then creates workspaces.
Module loading and consumer creation are separate startup phases; synchronization
runs between them after provider callbacks return. DurinEd's module startup registers the
Actor editing, asset editor, notification and [tool](../Tools/EditorTools.md) services. LevelEditor
registers the Level editing service, PIE Toolset and Content Browser extensions.
MaterialEditor, TextureEditor and StaticMeshEditor likewise publish their contributions
in StartupModule, independently of workspace instances. Module and workspace
operations report business failures through their own startup results. Unexpected
subsystem callback exceptions propagate after collection cleanup.

The host records retirement and stops tool and notification admission once. These
early-stop hooks are private host integration; the public subsystem lifecycle
remains `Initialize` and `Deinitialize`. Each frame stops after the tool, script,
World or shell callback that requested shutdown. At the safe boundary, shutdown
stops PIE, shuts down EditorWorld, destroys shell consumers, then deinitializes
Editor services. Base shutdown
retires the remaining World and Engine services. The owner calls `PreExit()` after initialization, update and external callbacks
return; none of these calls automatically perform Engine teardown. Explicit
retirement is idempotent and `DEditorEngine::BeginDestroy` supplies the same
fallback path. Editor subsystem destruction neither closes the editor nor waits
for its finish-destroy readiness. External subsystem callbacks request GC with `RequestGarbageCollection()`;
physical GC, module unload and membership changes run only at owner-controlled
safe points, not inside callbacks.

Tool subsystem synchronous calls, submissions, availability queries and pumps
call their service directly after `CanCallNow` admits them. Service admission
checks endpoint state and `HostReady`/`CanTransact`, including host retirement,
without an intervening user callback. These paths do not additionally wrap the
call in `DispatchSubsystemCallback`. Other external callback owners retain that
wrapper where it supplies their shutdown gate. This does not change callback
cleanup, GC, or module-unload contracts.

## Asset Editors

`DAssetEditorSubsystem` in `Subsystems/AssetEditorSubsystem.h` follows the core
native asset-editor lifecycle of UE's `UAssetEditorSubsystem`. The active editor owns
the service. `OpenEditorForAsset(DObject*)` first finds and focuses an
existing primary `IAssetEditorInstance`; otherwise it enters definition-based
dispatch. Path overloads use resident objects or registry metadata first and load
only when class metadata is unavailable. Batch opening
deduplicates objects, attempts every distinct input, and returns aggregate success;
an accepted deferred request is not yet an actually opened editor. A recursive
open of the same asset is rejected while its open/focus callback is in progress.

`NotifyAssetOpened`, `NotifyAssetsOpened`, `NotifyAssetClosed` and
`NotifyEditorClosed` maintain non-owning, many-to-many associations in both
directions. Duplicate notification is idempotent. Object keys distinguish object
identity across GC and replacement; the registry does not keep assets alive.
`FindEditorForAsset` selects a primary instance, `FindEditorsForAsset` includes
secondary instances, and `GetAllEditedAssets`/`GetAllOpenEditors` enumerate the
current associations. Instance owners must notify before destroying their code
or instance. Returned pointers are borrowed until the next editor lifecycle change.

`CloseAllEditorsForAsset`, `CloseOtherEditors` and `CloseAllAssetEditors` forward
the corresponding `EAssetEditorCloseReason` to each instance. Close requests use
snapshots and recheck membership before entering callbacks, so an earlier close
may remove a later instance. A rejected close or pending dirty-document modal
is not reported as a completed close. Service calls require the game thread,
registered owning host with no pending shutdown. Open/focus/close callbacks use host
admission checks; shutdown requests close admission immediately and the owner
executes `PreExit()` only after callbacks return. Close notifications remain legal during cleanup.

MainFrame supplies its host-owned `FEditorDocumentManager` to the service's workspace
adapter. The service does not own tabs or the main window. Each committed asset
document has one `IAssetEditorInstance` adapter. Successful open, deferred
completion, singleton resource replacement, close, route removal, package reload
and resource relocation update the associations. Tracking only resolves resident
objects and never loads additional packages. Destroying the manager detaches its
routing endpoint and instances before Editor services deinitialize. Native hosts
may construct a manager with their own service without creating a window.

`DAssetDefinition` provides immutable reflected type metadata and const open
behavior. Eligible CDOs register after full construction in `PostInitProperties`
and retire their exact object generation in `BeginDestroy`. The process-level
`FAssetDefinitionRegistry` rejects duplicate exact class paths and resolves the
nearest registered ancestor when no exact definition exists. Membership revision
changes on insertion/removal; ancestor lookup is uncached, so later definitions
and removals apply immediately without invalidating stored class pointers.
Queries do not load asset packages or construct toolkits. Scoped dynamic
registrations retain their definition across GC; their handles may outlive the
registry. Dispatch retains a selected definition across callbacks and pending work.
Automatic registration does not authorize physical DLL reload.

`OpenAssets(FAssetOpenArgs)` requires the requesting subsystem as explicit context
and returns one result per input, preserving mixed success/failure and duplicate
outcomes. Metadata includes canonical object path and full class identity.
Definitions explicitly support Edit or View; unsupported methods are rejected
before execution. `FAssetOpenArgs::LoadAsset` is execution-only and rejects loaded
objects whose class disagrees with the selection metadata. A registered definition
is authoritative: its rejection never falls through to another editor.

When no definition exists, the transitional exact-route workspace adapter supports
Edit only. MainFrame's browser callback now passes its metadata selection directly
to this dispatch rather than preloading it. Concrete editor migration and generic
property editing are separate later steps; `DAssetDefinitionDefault` currently
supplies shared display/category metadata, not a generic editing implementation.

Definitions create pending operations with `BeginOpenRequest`, supplying an
explicit cancellation callback. `GetOpenRequest` observes terminal outcomes and
`CompleteOpenRequest` rejects late completion. A refused cancellation leaves work
and provider retention unchanged. Legacy pending documents have observable request
IDs but cancellation stays with their existing controllers; the subsystem must
not claim that erasing a document drained its asynchronous work. Definition-owned
pending work must cancel/drain before subsystem retirement. Terminal request data
remains until teardown; bounded history is introduced with MRU/session restoration.

Texture2D now uses a reflected session and reusable toolkit instead of a
per-document instance adapter; its View method guards commands and property
mutation. Other feature editors still use the Workspace document host. See
[asset definitions and editor sessions](AssetEditors.md) for the implemented
ownership, host, close-coordination and migration boundary. Native hosting,
restore/MRU, usage accounting, editor modes and lifecycle multicast events remain
unimplemented. Concrete module startup/rollback stays in MainFrame during migration.

## Level Editor Play Controls

`DLevelEditorSubsystem` in `Editor/LevelEditorSubsystem.h` belongs to LevelEditor,
which depends publicly on DurinEd's subsystem contract. Each editor owns one instance
that survives repeated PIE sessions. Its `DFUNCTION` entrypoints are
`GetPlayStatus()`, `StartPlaySession()`, `StopPlaySession()` and
`SetPlaySessionPaused(bool bPaused)`. They return the reflected
`FLevelEditorPlayResult` containing `bSucceeded`, `Message` and `State`.
Admission requires GameThread, a live service registered with its owning host
with no pending host shutdown. Rejected admission leaves State empty; admitted
calls report `stopped`, `playing` or `paused`, including business failures.

The subsystem owns script-facing domain checks and delegates actual play-world
creation, teardown and pause changes to `DEditorEngine`. Stopping or pausing
without an active session fails; starting without a current editor level fails.
These methods are not `CallInEditor` actions and do not create Undo transactions
or automatically grant Python/MCP exposure. The existing `editor.pie` Toolset
resolves this service on its admitting host and converts successful results to
the unchanged `{message, state}` contract; failures remain DomainFailure
diagnostics. Python and MCP continue through the existing tool service.

LevelEditor's module startup publishes both the subsystem provider and PIE
Toolset without constructing a workspace or native window. Workspace activation
loads LevelEditor normally; no hard-coded pre-initialization load is required.
Workspace startup explicitly synchronizes services after module loading and
before creating consumers. Minimal native hosts may load LevelEditor after
`InitializeEditorSubsystems()` and then call `AddRegisteredEditorSubsystems()` at
an idle boundary before driving tools. Ordinary editor Tick never synchronizes
subsystem registrations.
MCP and Python adapters keep their existing interfaces and generic Toolset
routing. Dynamic test providers belong in the runtime deployment closure.

## Controlled Dynamic Membership

Only Editor collections update live membership; Engine and World collections
remain frozen. Providers retain registration tokens for their published services.
Module-load callers invoke `AddRegisteredEditorSubsystems()` after the complete
load batch returns and before creating or driving its consumers. Project and
Workspace startup provide these explicit idle boundaries. This enumerates the
registration catalog and adds missing instances; it does not compare memberships
or remove anything. Individual callers use `AddEditorSubsystem(Type)` and
`RemoveEditorSubsystem(Type)`. Operations require an idle host. Bulk addition and
removal return void; single addition returns the existing or created instance,
or null when unavailable. Invalid entry and registration contracts assert.
Unexpected callback exceptions propagate after cleanup; startup does not convert
them into subsystem failure diagnostics. Ordinary Tick never polls registrations. Loading a module or
changing a token alone does not change an existing collection. Callback-time
changes require a later explicit operation. Tool execution is unavailable during
collection mutation. A provider must finish Startup before its services are added.

`ShouldCreateSubsystem` selects new instances only. Adding an existing class
returns its existing instance without reevaluating the CDO. Registration token
churn and duplicate publication do not replace it. There is no replacement or
consumer re-registration protocol.

To recreate a service, callers stop and explicitly remove its consumers, remove
the service, add it again, then reconnect the consumers. External strong references
may retain stopped objects while new instances run. The collection does not track
a descriptor dependency graph or validate consumer removal order.

Dependencies are requested by `Initialize(Collection)` through
`InitializeDependency<T>()`. Missing or excluded dependencies return null for the
consumer to check; wrong-scope and recursive dependencies assert. The framework protects collection
iteration and GC lifetime; host retirement requested by a callback takes precedence.

Removal deinitializes the selected objects in
reverse initialization-completion order, then releases collection references.
Dependencies remain queryable until explicitly removed. Retained
objects stay valid but stopped; ordinary GC reclaims them after their remaining
strong references are released, without shutting down the active editor.
Script proxies and tool endpoints own their retirement checks and reject use once
work closes. Service removal does not broadcast object destruction or purge
arbitrary external references. Collection cleanup is synchronous at a host-owned
boundary, without a deferred-close queue or supplemental cleanup request.

Additions run sequentially. A failed object is cleaned up; earlier successful
additions and dependencies remain alive. There is no whole-update validation,
transactional rollback, registration revision poll or force-refresh mode. A later
explicit addition can retry a failed class. Host shutdown closes external
admission but leaves an already-started addition batch running. The host cleans
the collection after the batch and its callbacks unwind; no initialization
callback is canceled or skipped by the collection.

Providers own removal in `ShutdownModule()`: they stop their consumers, call
`GEditor->RemoveEditorSubsystems(Provider)` when an editor exists, then destroy
registration tokens and remaining provider resources. The host validates its safe
point; the collection removes its actual live entries for that provider under its
existing mutation boundary. Initialize and Deinitialize callbacks request GC;
they must not execute either synchronous collector. Requests survive success
and failure cleanup without relying on automatic business-stack refusal. The owner
collects only after dynamic mutation returns, before any required provider unload.
The collection does not consult the active editor, transactions or module dispatch.
There is no global subsystem update permission query or scheduling manager.

ModuleManager owns control-thread and feature-dispatch restrictions. A provider's
read-only `CanShutdownModule()` protects its current host callbacks, unfinished
host retirement and GC; its
ShutdownModule relies on that established sequence rather than repeating the
preflight. Requests from callbacks must be retried after they unwind. The host's
explicit update entries check the active host, retirement, GC and feature-dispatch
boundary. The common collection batch/removal entries check initialization and
mutation state; the Editor adapter does not repeat those checks. Single-type
additions use the same batch entry as catalog additions, including unregistered
no-op requests. Unrelated transaction
recovery does not block service membership changes. LevelEditor retains its own
transaction restriction because its shutdown discards module-owned custom changes.

Consumer-owned cancellation tokens do not provide module residency. Providers must finish
module-owned work and detach external callbacks in their existing ShutdownModule
contract. DurinEd and LevelEditor remain process-resident: unregistering their
services releases collection references without releasing their DLLs.
A provider that opts into `SupportsDynamicReloading()` must also ensure no live
object, reflected type, callable or task can enter its code after shutdown.
Dynamic service membership does not imply reflected-module hot reload support;
ModuleManager does not implement that object/type teardown protocol.

## Actor Editing

`DEditorActorSubsystem` in `Actors/EditorActorSubsystem.h` provides general Actor
spawn/delete/duplicate/rename/attach/detach and owned Component structural edits.
Resolve it through the admitting host's `GetEditorSubsystem<DEditorActorSubsystem>()`.
Each call identifies its Level or owning Actor and supplies `FActorEditContext`
with an explicit read-only policy. The service resolves its owning active editor;
callers cannot select a different host or history. Native non-document adapters must
also state that policy; selection is never an implicit target.

Admission requires the game thread, a live registered service with no pending host shutdown,
its current editor Level, stopped PIE/World play and an idle or
recording host transactor. Synchronous Actor and Component changes join the
innermost recording scope and commit with the outer transaction, retaining its
description; with no outer scope they submit an independent history entry.
Executing, Undoing, Redoing, pending asynchronous operations, RecoveryRequired
and Destroying still reject admission. Detached, foreign, template or unsupported
participants fail before preparation. Creation returns the Actor/Component pointer
or null; Actor duplication returns the new Actors in input order or an empty
array. Other edits return bool, with true also covering successful no-ops. Public
operations carry no error strings, status enum or history result. Detailed
execution/recovery state stays in DTransactor.
Same-name rename, same-label assignment and same-parent reparent produce no history entry. Empty or
duplicate batch participants are rejected; valid batches submit once.

World admission reads the full play state, including subsystem BeginPlay before
Actor play dispatch, and rejects a requested shutdown before teardown occurs.
Replay uses the same World boundaries and requires the owner still belong to
its Level. Component admission checks the thread before reading owner state.

DurinEd owns general replay code and module tags; runtime Level/Actor methods
remain mechanisms. `MakeActorPresenceChange` exports opaque structural-change
ownership for specialized compound commands. Their owners retain responsibility
for private preparation and cleanup before publication. Preparation callbacks
are synchronous and never stored in history. There is one editor transactor.
Native Actor service tests initialize real editor subsystems and use their
editor-owned history; production services have no injected-history or hostless mode.

Document adapters retain selection, focus, confirmation and error presentation.
The subsystem does not retain a LevelEditor context or depend on UI modules.
See [structural history](../LevelEditing/ViewportEditing.md#actor-structure-commands) and
[Component history](../LevelEditing/LevelEditorDetails.md#component-structure-history).

## Script Actor Adapter

The Actor subsystem publishes ScriptCallable native adapters with explicit
ScriptName aliases. Scripts resolve the existing service through
`durin.get_editor_subsystem(durin.EditorActorSubsystem)`; methods use the generic
reflection bridge without an Actor Toolset dependency.

| Script method | Inputs / result |
| --- | --- |
| `get_editor_level()` | Current editor Level, or None when absent |
| `get_all_level_actors(level)` | Live published Actors in Level order |
| `get_actor_info(level, actor)` | ActorScriptInfo snapshot |
| `spawn_actor(level, actor_class, transform, name="", label="", name_mode=SpawnActorNameMode.REQUESTED)` | Actor or None |
| `destroy_actors(level, actors)` | bool |
| `duplicate_actors(level, actors)` | Array[Actor], empty on failure |
| `rename_actor(level, actor, name, name_mode=SpawnActorNameMode.REQUESTED)` | bool |
| `set_actor_label(level, actor, label, make_unique=True)` | bool |
| `attach_actors(level, actors, parent, rule=AttachmentTransformRule.KEEP_WORLD)` | bool |
| `detach_actors(level, actors, rule=DetachmentTransformRule.KEEP_WORLD)` | bool |

Queries require the active job's Host and current editor Level. They may inspect
that editor Level during PIE, but never substitute the play World. An invalid
query raises a script access error rather than returning an empty snapshot.
ActorScriptInfo contains actor, internal name, display label, actor_class, parent, direct children in Level
order, root_component and world_transform, all read-only detached fields.

`rename_actor` changes the internal object Name. `set_actor_label` changes only
its display label, with ordinary admission, dirty state and history; exact replay
does not uniquify recorded labels. The serialized label field is excluded from
automatic whole-object transaction snapshots because the label command owns its
history. Outliner rename uses this label operation.
Creation initializes a unique display label from `label`, the requested Name or
the class default name, independently of any suffix on the internal Name.
Duplication reserves labels across the entire prepared batch. Object naming uses
the shared [Level rules](../../../Runtime/World/LevelSystem.md).

Edits derive FActorEditContext from native job/document authority. Native admission
rechecks live script authority on entry and after preparation callbacks, keeping
the existing stopped-play, ownership, hierarchy, batching and history rules.
DocumentManaged requires live writable document policy; non-document access is
an explicit native session option. Native read-only policy always wins.

Script edits expose the same simple return values as native calls. A rejected
domain edit returns None, an empty array or false; no per-operation failure
diagnostic is exposed. Binding rejection of invalid types or stale arguments
still precedes domain execution and raises an exception. Successful no-ops return
true without entering history. Structural deletion preserves identity for Undo,
and retained proxies remain subject to published-membership checks.

Structural edits and generic property edits join the same Host transaction;
there is no automatic rollback of earlier completed edits on later failure.
Component structural adapters, selection/focus and specialized transform setters
are not exposed. Use the root Component's generic relative_transform editor
property for transform editing; see the [script guide](../../Guides/PythonScripts.md).

## Notifications

`DEditorNotificationSubsystem` owns the authoritative `FNotificationManager`.
`DEditorEngine::GetNotificationManager()` is a forwarding facade requiring a
successfully initialized notification service. The host does not construct or
separately destroy a manager. There is one notification history per editor host,
independent of World replacement and PIE.

MainFrame publishes transaction presentation feedback. After the optional
main-frame update, the normal editor tick calls `DEditorNotificationSubsystem::Tick`
once using the engine frame delta, including when no MainFrame exists.
The subsystem owns notification time advancement; the overlay never ages the
manager. The host suppresses dispatch after shutdown; the subsystem forwards Tick
without repeating the manager's admission check. The manager retains that check
for direct standalone use. No Tick virtual is added to every subsystem.

Producer methods serialize through the pending-command mutex. Manager admission closure
rejects new commands; Post/BeginProgress return zero when rejected. Deinitialize
retires the manager, discards pending commands and clears notifications, status
and history. Retained action, enablement and cancellation callbacks capture a
shared manager admission state, so they cannot invoke provider callbacks
after retirement, even after physical service destruction. Manager destruction
also retires standalone instances. Actions, enablement predicates and cancellation
callbacks use host admission checks; they request GC instead of collecting immediately. A shutdown request stops tool and notification admission immediately but leaves entries and consumers alive until the next host
owner-driven `PreExit()` boundary, so drawing and the initiating callback can finish. An enablement
predicate that retires its manager prevents the action from being invoked.

Progress notifications normalize nonfinite fractions to unknown and clamp finite
fractions to `[0, 1]` on both begin and update. Completion, failure, and cancellation
share one terminal transition policy across toast, status, and history entries;
late progress or terminal commands do not overwrite an already terminal entry.
Failure uses the supplied message even when empty; completion/cancellation with
an empty message retain the current message. Errors stay until dismissed, while completed
and canceled progress use the normal informational lifetime.

Toast and history progress use the same indicator rendering: measured work shows
a percentage, while unknown work animates without inventing one. Toast, history,
and status-bar buttons derive their label and cancellation/action availability
from the same notification. A cancellable status notification does not require a
separate Action to expose Cancel. Cancellation remains a one-shot request, and
terminal notifications no longer offer cancellation.

Raw manager references are valid only while their owning service remains alive.
Workers retain detached data and consumer-owned cancellation state and publish at
the game-thread boundary after admission checks. A raw manager or host pointer
does not protect lifetime. Consumers own cancellation, draining and callback cleanup.
