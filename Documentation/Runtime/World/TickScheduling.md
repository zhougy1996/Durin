# Tick Scheduling

Summary: Define registered Actor and Component Tick ownership, serial groups, mutation semantics, and lifetime safety.

Modules: Engine

Durin schedules gameplay Tick callbacks through stable Tick functions rather
than Actor or Component ownership containers. `DLevel::Actors` and
`AActor::OwnedComponents` retains every live component. The persistent
`AActor::AuthoredComponents` field defines authored structure; neither
collection is a per-frame execution list.

## Tick Functions And Registration

Each `AActor` embeds one `FActorTickFunction`, and each `DActorComponent`
embeds one `FActorComponentTickFunction`. The containing object gives the Tick
function a stable address for its lifetime. Tick functions are non-copyable and
the scheduler retains only non-owning pointers to them.

The active Level owns one `FTickRegistry`. Actor admission to the Level
registers its primary Tick function. Component registration registers its
primary Tick function; Component unregistration removes it. Registered and
enabled are independent states, so a disabled function may remain registered
without entering a frame queue.

Actor and Component Tick eligibility also requires their normal World play,
structural membership, registration, and destruction invariants. Eligibility
is checked when a node is queued and immediately before execution. Marking an
object for garbage collection does not preserve its execution eligibility.

Component Tick is independent of Actor Tick enablement. Disabling an Actor's
primary Tick does not disable eligible owned Components.

All registration, mutation, and execution operations are game-thread-only.

## Serial Tick Groups

One admitted `DWorld::Tick()` executes three ordered groups:

```text
input preparation
    -> PrePhysics
    -> Physics
    -> PostPhysics
    -> return from DWorld::Tick
```

A group is a serial completion barrier, not a worker thread. Every admitted
Tick in one group completes before the next group begins.

- `PrePhysics` produces gameplay intent and transforms consumed by physics.
- `Physics` performs synchronous physics-component integration.
- `PostPhysics` consumes final transforms for animation and late gameplay work.

Actor and ordinary Component Tick functions default to `PrePhysics`.
`DPhysicsComponent` uses `Physics`; `DSkeletalMeshComponent` uses
`PostPhysics`. Durin does not expose a `DuringPhysics` group because the current
physics path has no asynchronous interval to overlap.

Changing a Tick group while its registry is executing a frame is rejected. A
Tick function cannot move into a group whose execution has already begun.

## Ordering

Group barriers are the primary ordering mechanism. Within one group,
unconstrained Tick functions execute in deterministic Level-local registration
order. Unregistering and later registering a function gives it a new position
after functions that remained registered.

An enabled owned Component in the same group has an engine-owned prerequisite
on its enabled owning Actor. The registry executes the Actor first even if the
Component's queue slot is encountered first. An Actor in an earlier group is
already ordered by the barrier. A prerequisite in a later group is invalid and
cancels the dependent Tick with a diagnostic.

General user-authored prerequisites, priorities, batching, Tick intervals, and
parallel dispatch are not part of this contract.

## Same-Frame Mutation

At frame start the Level registry admits all currently eligible functions. A
callback may register, unregister, enable, disable, add, or destroy Tick
targets without mutating the active queue's backing storage.

The same-frame rules are:

- registration or enabling before a future group begins may enter that future
  group in the current frame;
- registration or enabling after the selected group begins waits until the
  next admitted World frame;
- disabling, unregistering, EndPlay, or destruction cancels a queued callback
  that has not started;
- cancelling an executing Tick does not interrupt its callback;
- re-enabling or re-registering a cancelled node cannot revive its stale queue
  slot;
- each Tick function executes at most once in one admitted World frame;
- a Component added in its currently executing group begins Tick on the next
  admitted frame;
- destroying an Actor cancels the Actor Tick and all remaining Component Ticks
  before their callbacks can start.

These rules are implemented by stable-node queued, executed, cancelled, and
executing state plus frame stamps. Component registration generations are not
part of Tick scheduling identity.

## World Admission And Lifetime

`DWorld` captures its current Level, starts the registry frame, runs each group,
and ends the registry frame. Losing World play state, changing the current
Level, or accepting a pending Level transition stops the active group and all
remaining groups after the current callback unwinds.

Pause suppresses the complete registry frame. A requested single step admits
exactly one normal registry frame. Input preparation remains before
`PrePhysics`, so one-frame input edges are visible to the admitted frame at
most once.

Level detachment unregisters Components and resets every remaining Actor Tick
registration before clearing the Level's World endpoint. The registry never
owns or retains an Actor or Component. Garbage collection owns physical object
destruction and cannot release an embedded Tick function while an active queue
may still reference it.

## Complexity

Frame admission performs one ordered pass over registered Tick functions.
Each admitted function is appended to one retained group vector and is
executed at most once; the engine-owned owner prerequisite adds at most one
edge visit per Component. Group vectors retain capacity across frames, and no
ownership-container snapshot or per-callback allocation is required by the
scheduler.
