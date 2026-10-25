# Editor Subsystems

Summary: Define per-editor native services, reflected PIE controls, Actor/Component editing, and notification ownership and update timing.

Modules: DurinEd, LevelEditor, MainFrame, Engine

Last reviewed: 2026-10-20

## Host Contract

There is one active editor engine per process. Construction is inert;
ActivateEditor is the sole publication boundary and rejects repeated activation,
including activation of the same instance. Launch and headless fixture setup
call it explicitly. Full Init and subsystem initialization require that already
active editor and never publish one themselves. Shutdown releases activation only after callback and World work
is quiescent. A later editor lifetime can then activate without reusing old gates.

Native headless tests share Editor/EditorEngineTestSupport.h for explicit startup
and private test access. Ordinary fixtures construct real DEditorEngine instances;
specialized subclasses remain only for additional lifecycle probes. Startup-only
reflected subclasses and translation-unit-specific definitions of the same test
accessor are not needed.

`DEditorSubsystem` extends the [common subsystem contract](../../Runtime/Engine/Subsystems.md)
in DurinEd. The single active `DEditorEngine` owns an Editor collection separately from its inherited
Engine collection. `GetEditorSubsystem<T>()` selects exact Editor types;
inherited `GetSubsystem<T>()` still selects Engine types. Editor services have
no implicit PIE World requirement and survive repeated PIE start/stop.

`FEditorSubsystemRegistration` accepts the common descriptor. Keep tokens in the
providing module. Registration and token destruction publish changes for the active editor
and later editor lifetimes; instances change only at a host safe point. Dependencies name
concrete Editor types in the same collection. The host enumerates
both collections for GC. Runtime Engine has no dependency on DurinEd.

Editor Init loads project providers and AssetForgeBuiltins, runs base Init,
retains EditorWorld, loads MainFrame, then initializes currently available Editor services before
CreateEditorHost exposes consumers. The constructor-created transactor is
available; the shell is not yet created. DurinEd's module startup registers the
Actor editing, asset editor, notification and [tool](EditorTools.md) services. LevelEditor
registers the Level editing service and PIE Toolset. A returned failure, cancellation or exception retires
partial host state and prevents successful initialization.

Shutdown closes both collections' gates, stops PIE, shuts down EditorWorld,
destroys shell consumers, then deinitializes Editor services. Base shutdown
retires the remaining World and Engine services. Initialization and update
operation scopes defer requested destruction until callbacks unwind. Explicit
retirement is idempotent and BeginDestroy supplies the same fallback path.

## Asset Editors

`DAssetEditorSubsystem` in `Subsystems/AssetEditorSubsystem.h` follows the core
native asset-editor lifecycle of UE's `UAssetEditorSubsystem`. The active editor owns
the service. `OpenEditorForAsset(DObject*)` first finds and focuses an
existing primary `IAssetEditorInstance`; otherwise it resolves the exact-class
workspace route. Path overloads load the asset before routing. Batch opening
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
registered owning host and open work gate. Open/focus/close callbacks use the
host operation scope; shutdown closes admission immediately and defers resource
retirement until callbacks unwind. Close notifications remain legal during cleanup.

MainFrame supplies its host-owned `FWorkspaceManager` to the service's workspace
adapter. The service does not own tabs or the main window. Each committed asset
document has one `IAssetEditorInstance` adapter. Successful open, deferred
completion, singleton resource replacement, close, route removal, package reload
and resource relocation update the associations. Tracking only resolves resident
objects and never loads additional packages. Destroying the manager detaches its
routing endpoint and instances before Editor services deinitialize. Native hosts
may construct a manager with their own service without creating a window.

This implements the supported object/path open, lookup, notification and close
subset of UE's API, using Durin's `D` object prefix, standard containers and
Workspace document host. UE Toolkit hosting modes, AssetDefinition-based dispatch,
read-only open methods, restore/MRU, analytics, editor modes and lifecycle multicast
events are not provided by this contract. Concrete module startup/rollback remains
in MainFrame; introducing the service does not change module loading policy.

## Level Editor Play Controls

`DLevelEditorSubsystem` in `Editor/LevelEditorSubsystem.h` belongs to LevelEditor,
which depends publicly on DurinEd's subsystem contract. Each editor owns one instance
that survives repeated PIE sessions. Its `DFUNCTION` entrypoints are
`GetPlayStatus()`, `StartPlaySession()`, `StopPlaySession()` and
`SetPlaySessionPaused(bool bPaused)`. They return the reflected
`FLevelEditorPlayResult` containing `bSucceeded`, `Message` and `State`.
Admission requires GameThread, a live service registered with its owning host
and an open subsystem work gate. Rejected admission leaves State empty; admitted
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
The next editor Tick reconciles services before draining tool requests. Minimal
native hosts may load LevelEditor after `InitializeEditorSubsystems()` and then
call `RefreshEditorSubsystems()` at an idle boundary before driving tools.
MCP and Python adapters keep their existing interfaces and generic Toolset
routing. Dynamic test providers belong in the runtime deployment closure.

## Controlled Dynamic Membership

Only Editor collections opt into reconciliation; Engine and World collections
retain frozen membership. `RefreshEditorSubsystems()` is a checked native safe
point and returns `FSubsystemResult`. It rejects wrong threads, active host/World
operations, collection callbacks, feature dispatch, GC and non-idle history.
Registration inside Initialize, Deinitialize or another callback is deferred to
a later pass. Tool execution is unavailable during reconciliation. Each pass
uses an owned snapshot; a callback cannot alter the pass being processed.
Providers still in module Startup are not instantiated. Initial module loading
and later loading therefore share the same registration mechanism.

Registration identities distinguish replacement from an unchanged type. Valid
services initialize in deterministic dependency order. Missing dependencies,
duplicate types and cycles exclude affected services while retaining unrelated
instances. Removing or replacing a dependency also retires its dependents:
all affected work gates close first, then Deinitialize runs in reverse dependency
order, with dependencies still queryable during dependent cleanup. Objects are
marked as garbage; their physical destruction does not shut down the host.
An unsuccessful addition rolls back that pass's new instances, including the
failed object, while retaining pre-existing services. Host shutdown during an
update aborts additions and completes host retirement after callbacks unwind.

Failed passes are not retried every frame. A publication change retries the
pending graph; an explicit `RefreshEditorSubsystems(true)` can retry an unchanged
failed batch. Its result exposes missing-dependency, duplicate, cycle, provider,
initialization and safe-point diagnostics to native hosts.

Providers own removal in `ShutdownModule()`: DurinEd and LevelEditor destroy
their registration tokens and call `FEditorSubsystemCollection::SynchronizeActiveEditor()`
before releasing the remaining provider resources. This removes active-editor instances
and dependent services before shutdown returns. There is no module-manager
consumer callback or separate suppressed-registration list.

Their read-only `CanShutdownModule()` calls `CanSynchronizeActiveEditor()` to preflight
the active Editor collection. Initialization, cleanup, host callbacks, GC, feature
dispatch and non-idle history postpone the entire shutdown. `UnloadModule()`
returns false without changing an active module at an unsafe point; explicit
`ShutdownModule()` requires the caller to establish that safe point. Providers
check it again before mutating registrations, including direct cleanup after
partially completed startup. Requests from callbacks must be retried after they
unwind.

Work gates provide cancellation, not module residency. Providers must finish
module-owned work and detach external callbacks in their existing ShutdownModule
contract. DurinEd and LevelEditor remain process-resident: unregistering their
services marks the objects as garbage for later GC without releasing their DLLs.
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

Admission requires the game thread, a live registered service and open work gate,
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
See [structural history](ViewportEditing.md#actor-structure-commands) and
[Component history](LevelEditorDetails.md#component-structure-history).

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
the shared [Level rules](../../Runtime/World/LevelSystem.md).

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
property for transform editing; see the [script guide](../Guides/PythonScripts.md).

## Notifications

`DEditorNotificationSubsystem` owns the authoritative `FNotificationManager`.
`DEditorEngine::GetNotificationManager()` is a forwarding facade requiring a
successfully initialized notification service. The host does not construct or
separately destroy a manager. There is one notification history per editor host,
independent of World replacement and PIE.

MainFrame publishes transaction events and calls `UpdateNotifications` before
notification overlay drawing. The editor host wraps that update in its operation
scope and advances the manager once using the UI frame delta. No Tick virtual is
added to every subsystem and no second Engine Tick update advances notification
lifetimes. Standalone manager tests may still call Tick directly.

Producer methods serialize through the pending-command mutex. Gate closure
rejects new commands; Post/BeginProgress return zero when rejected. Deinitialize
retires the manager, discards pending commands and clears notifications, status
and history. Retained action, enablement and cancellation callbacks capture a
shared admission identity and work gate, so they cannot invoke provider callbacks
after retirement, even after physical service destruction. Manager destruction
also retires standalone instances. Actions, enablement predicates and cancellation
callbacks enter the host's external callback scope. A shutdown request closes all
gates immediately but leaves entries and consumers alive until the next host
boundary, so drawing and the initiating callback can finish. An enablement
predicate that retires its manager prevents the action from being invoked.

Raw manager references are valid only while their owning service remains alive.
Workers must retain detached data and a work gate and publish at the game-thread
boundary; copying a raw manager or host pointer into a worker is not lifetime
protection. Gates keep callback code resident but never join workers.
