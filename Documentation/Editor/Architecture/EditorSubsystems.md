# Editor Subsystems

Summary: Define per-editor native services and the notification subsystem's ownership and update phase.

Modules: DurinEd, MainFrame, Engine

Last reviewed: 2026-09-08

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
notification service. A returned failure, cancellation or exception retires
partial host state and prevents successful initialization.

Shutdown closes both collections' gates, stops PIE, shuts down EditorWorld,
destroys shell consumers, then deinitializes Editor services. Base shutdown
retires the remaining World and Engine services. Initialization and update
operation scopes defer requested destruction until callbacks unwind. Explicit
retirement is idempotent and BeginDestroy supplies the same fallback path.

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
