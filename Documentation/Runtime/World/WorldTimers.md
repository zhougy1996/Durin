# World Timers

Summary: Define play-scoped World timers, gameplay time, object bindings, and callback mutation behavior.

Modules: Engine

Last reviewed: 2026-09-16

## Ownership And Time

Every `DWorld` directly owns one `FTimerManager`, available through
`GetTimerManager()`. All manager operations, including queries, are confined to
the game thread. Timers are transient native state, excluded from serialization
and PIE duplication. Registration is accepted only during BeginningPlay or
Playing while World work admission is open. Failed registration returns a
handle with zero generation.

Timers belong to one play lifetime. EndPlay clears timers and resets accumulated
game time; Level replacement and World shutdown also clear all state. Handles
from a previous play lifetime or another World cannot operate on fresh timers.
The manager survives Level replacement, but its callbacks do not. Editor and
Preview Worlds can use gameplay timers during explicit play; stopped host ticks
do not advance them. There are no background or wall-clock timers.

`DWorld::SetTimeScale` accepts finite nonnegative values, defaults to one, and
leaves the old value unchanged on rejection. World Tick captures the multiplier
at entry, before transitions, input, or user callbacks. The same scaled float
delta feeds gameplay subsystems, Actor/Component Tick, and the manager's double
accumulator. Non-gameplay Editor/Preview subsystem ticks retain the host delta.
Negative/nonfinite input deltas or a scaled delta outside float range reject
the entire Tick without consuming a single-step request or a pending transition.

World pause suppresses gameplay frame admission and time advancement. A single
step admits one frame with that call's scaled delta. Scale zero is distinct:
it admits frames with zero elapsed gameplay time, so next-frame and zero-delay
callbacks may execute while positive-duration timers remain pending.

## API And Scheduling

```cpp
auto& Timers = World.GetTimerManager();
auto Delayed = Timers.SetTimer(2.0, [] { /* once after two game seconds */ });
auto Repeating = Timers.SetTimer(0.5, [] { /* periodic check */ }, 0.5);
auto NextFrame = Timers.SetTimerForNextTick([] { /* next admitted game frame */ });
Timers.ClearTimer(Repeating);
```

Delay must be finite and nonnegative. Interval zero means one-shot; a looping
interval must be positive and finite. Invalid callbacks and unrepresentable
deadlines are rejected. Zero delay is asynchronous, never an inline invocation.
Changing an existing timer means clearing its handle and registering a new one.

World admits pending timers and advances time before input preparation. Timers
registered or resumed after that admission, including during any Tick group or
timer callback, wait until at least the next admitted gameplay frame. Timers
registered between frames become eligible in the next one. Positive delays
start at the current gameplay clock; a long next frame may already reach them.

Dispatch runs after all PostPhysics Actor/Component callbacks, inside the
existing [World operation](LevelSystem.md#world-operation-boundaries). Eligible
callbacks are ordered by deadline, then original registration sequence.
Next-frame callbacks use their registration time as the ordering deadline.
This is a frame-tail callback boundary, not a new Tick group or prerequisite.

A looping timer executes at most once per admitted frame. Missed periods are
coalesced: after an overdue callback, the next deadline is the first future
point on its original cadence. No unbounded catch-up execution occurs. Precision
exhaustion advances to the next representable time; a nonfinite future deadline
retires the timer. Timers are unsuitable for numerical integration requiring
every fixed step.

## Cancellation, Pause, And Mutation

Handles contain World identity, slot, and generation. Cancellation never revives
an old handle when storage is reused. `ClearTimer` returns true only when it
actually retires an active timer; repeated clears are harmless and return false.
`IsTimerActive` includes individually paused timers. `GetTimerRemaining` returns
empty for inactive handles, zero for next-frame timers, and nonnegative remaining
game seconds otherwise.

`PauseTimer` freezes the remaining duration. `UnpauseTimer` restores that duration
relative to the current gameplay clock and defers eligibility until a later
frame. Both return false when the handle is invalid or already in the requested
state. A looping callback observes its next deadline, so pausing itself retains
the next period's remainder. Pausing a one-shot already executing cannot undo
its invocation or turn it into a repeating timer.

Cancellation immediately prevents an unstarted callback, including one already
in the due batch. It does not interrupt an executing callback. Callables execute
from independent storage: self-cancellation, bulk cancellation, registration
growth, and slot reuse cannot destroy the active callable underneath its stack.
Pause/resume revisions invalidate stale queue entries. User exceptions cancel
the offending timer and propagate; World operation/registry cleanup still runs,
and untouched due entries can run on a later admitted frame.

EndPlay, shutdown, and deferred Level transition requests stop further timer
dispatch after the current callback returns. Recursive World Tick remains
rejected. Cleanup runs at the existing World operation boundary, where physical
GC was deferred throughout callback execution.

## Object Bindings

`SetTimerForObject` and `SetTimerForObjectNextTick` accept an owner and a callback
taking `DObject&`. They retain only a generation-checked `FWeakObjectPtr`, resolve
it immediately before invocation, and never report it as a GC root. Ordinary
lambda captures are opaque to the collector; use the supplied callback argument
instead of capturing an unguarded object pointer.

Owners must belong to the current World: a World-owned native object, the
current Level hierarchy, or a live Actor/Component in that Level. Registration
and invocation reject garbage objects, foreign membership, Actors outside play
or being destroyed, and Components that are unregistered, outside play, no
longer owned, or being destroyed. Actor and Component bindings therefore become
available in their BeginPlay callbacks. Generic nested objects inherit the
eligibility of their containing hierarchy.

Engine Actor EndPlay/destruction clears directly bound Actor and Component
timers; Component EndPlay/unregistration clears its bindings. Re-registering or
restarting the owner cannot revive them. `ClearAllTimersForObject` clears exact
owner bindings, including paused timers. Generic object garbage marking is
checked before invocation; it need not eagerly remove a distant deadline.
Cancelled callable captures are released immediately except captures needed by
a currently executing callback, which live until it returns.

## Storage

Reusable slots hold stable timer records. A deadline min-heap avoids scanning
all active timers every frame; next-frame candidates and pending admissions use
separate queues. Queue records snapshot revisions and deadlines, allowing stale
entries to be rejected without mutating a callback's active storage. Stale heap
and pending records are periodically compacted relative to allocated slot
capacity. Due callbacks require deterministic sorting; bulk owner cancellation
scans slots and is intended for lifecycle boundaries.
