# Editor Subsystems

Summary: Define per-editor native services, Actor/Component editing, and notification ownership and update timing.

Modules: DurinEd, MainFrame, Engine

Last reviewed: 2026-10-17

## Host Contract

`DEditorSubsystem` extends the [common subsystem contract](../../Runtime/Core/Subsystems.md)
in DurinEd. `DEditorEngine` owns an Editor collection separately from its inherited
Engine collection. `GetEditorSubsystem<T>()` selects exact Editor types;
inherited `GetSubsystem<T>()` still selects Engine types. Editor services have
no implicit PIE World requirement and survive repeated PIE start/stop.

`FEditorSubsystemRegistration` accepts the common descriptor. Keep tokens in the
providing module and publish them before host initialization. Dependencies must
name concrete Editor types registered in the same snapshot. The host enumerates
both collections for GC. Runtime Engine has no dependency on DurinEd.

Editor Init loads project providers and AssetForgeBuiltins, runs base Init,
retains EditorWorld, loads MainFrame, then initializes Editor services before
CreateEditorHost exposes consumers. The constructor-created transactor is
available; the shell is not yet created. DurinEd's module startup registers the
Actor editing, notification and [tool](EditorTools.md) services. A returned failure, cancellation or exception retires
partial host state and prevents successful initialization.

Shutdown closes both collections' gates, stops PIE, shuts down EditorWorld,
destroys shell consumers, then deinitializes Editor services. Base shutdown
retires the remaining World and Engine services. Initialization and update
operation scopes defer requested destruction until callbacks unwind. Explicit
retirement is idempotent and BeginDestroy supplies the same fallback path.

## Actor Editing

`DEditorActorSubsystem` in `Actors/EditorActorSubsystem.h` provides general Actor
spawn/delete/duplicate/rename/attach/detach and owned Component structural edits.
Resolve it through the admitting host's `GetEditorSubsystem<DEditorActorSubsystem>()`.
Each call identifies its Level or owning Actor and supplies `FActorEditContext`
with the host and an explicit read-only policy. Native non-document adapters must
also state that policy; selection is never an implicit target.

Admission requires the game thread, a live registered service and open work gate,
the same host, its current editor Level, stopped PIE/World play and an idle host
transactor. Detached, foreign, template or unsupported participants fail before
preparation. Results distinguish Changed, NoOp and Failed, retain resulting
participants and transactor failure evidence, and provide an owned display message.
Same-name rename and same-parent reparent produce no history entry. Empty or
duplicate batch participants are rejected; valid batches submit once.

World admission reads the full play state, including subsystem BeginPlay before
Actor play dispatch, and rejects a requested shutdown before teardown occurs.
Replay uses the same World boundaries and requires the owner still belong to
its Level. Component admission checks the thread before reading owner state.

DurinEd owns general replay code and module tags; runtime Level/Actor methods
remain mechanisms. `MakeActorPresenceChange` exports opaque structural-change
ownership for specialized compound commands. Their owners retain responsibility
for private preparation and cleanup before publication. Preparation callbacks
are synchronous and never stored in history. There is one host transactor;
independent native-test injection is private to test access.

Document adapters retain selection, focus, confirmation and error presentation.
The subsystem does not retain a LevelEditor context or depend on UI modules.
See [structural history](ViewportEditing.md#actor-structure-commands) and
[Component history](LevelEditorDetails.md#component-structure-history).

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
