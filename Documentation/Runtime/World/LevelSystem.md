# Level System

Summary: Define levels, actors, world ownership, lifecycle mutation, and iteration behavior.

Modules: Engine

Last reviewed: 2026-08-18

`DLevel` is the persistent scene asset. A packaged level is the main asset of a `.dasset` package. Levels retain actors through reflected `TObjectPtr` arrays, and actors retain their components the same way; their Outer hierarchy separately provides structural containment and object paths.

Actors may additionally retain transient generated components through the
native construction registry. Those components participate in live World and
editor lifecycles but are excluded from the reflected package graph. See
[Native Actor Construction](NativeConstruction.md).

`DWorld` is a runtime or editor session container. It activates at most one level, forwards actor APIs to that level, and registers or unregisters the level's components when switching. Each active Level owns the non-owning registry for stable Actor and Component Tick functions; Level detachment clears that registry before the World endpoint is removed. Tick ownership, groups, mutation, and lifetime rules are defined by [Tick Scheduling](TickScheduling.md). Replacing a transient level structurally owned by the world marks that complete level hierarchy as garbage. A persistent packaged level is structurally owned by its package instead, so switching worlds does not destroy it; an object that must survive a transient world must likewise be explicitly reparented before world retirement.

The single `DWorld` contract is implemented in three responsibility-focused
translation units: `WorldCore.cpp` owns level/actor/render-scene ownership,
`World.cpp` owns play, restart, tick, pause, and transition lifecycle, and
`WorldCollision.cpp` owns physics queries and collision debug snapshots.

Each World also owns one synchronous `FPhysicsScene`. Primitive components
publish and retire their query bodies with the same registration lifecycle as
their World membership, independently from render-scene presence. Gameplay
uses the World trace, sweep, and overlap facade rather than enumerating
components or borrowing scene storage. See [Runtime Collision](../Physics/Collision.md).

A world starts without an active level. Actor operations safely return empty or fail until a level is activated. The editor supports this empty state: scene panels remain available, while level-dependent editing actions are disabled.

Scene persistence stores the actor list, primary camera, component relative transforms, attachment parents, and camera projection settings. Attachment children and world transforms are derived. After package fields are applied, `DLevel::PostLoad` validates ownership and attachment cycles, rebuilds child lists, and recalculates world transforms before the package load is published.

The Level Editor exposes New, Open, and Save using virtual asset paths and the
Asset Registry. Dirty package switches require Save, Discard, or Cancel.
Static-mesh components persist cross-package references to `DStaticMesh`
assets. A static-mesh asset serializes an optional mounted `FSourcePath`, exact
source identity, and build settings; CPU/GPU render data is restored from DDC or
rebuilt from its mounted source file and is never serialized in the authored
package. Version one intentionally supports a single active level and does not
include sub-level streaming, PIE cloning, or Save As.

At startup the editor opens the project's optional `Game.DefaultLevel` from
`Configs/Project.yaml`. `FProjectGameSettingsStore` is the single parser and
writer used by the Level Editor, standalone startup, and the project-settings
reference contributor. Editor and runtime code hold the default as
`TSoftObjectPtr<DLevel>` while YAML preserves its authored path string. Picker
assignment stores the path without loading; startup/open resolves redirectors
and validates the final `DLevel`. Relocation leaves YAML unchanged. The
reference store contributes the default as a typed external Cook root and
canonicalizes it in memory for runtime output without editing YAML; explicit
redirector Fix Up is the only transaction that rewrites the setting. Projects
without a default level start with an empty editor; missing or invalid defaults
and failed level loads are non-fatal.

## World Play State

`DWorld` publishes one authoritative game-thread play state:

```text
Stopped -> BeginningPlay -> Playing -> EndingPlay -> Stopped
```

The runtime lifecycle is component registration and initialization, World
BeginPlay, Actor and Component BeginPlay, admitted Tick, Actor and Component
EndPlay, then component uninitialization and unregistration.

`HasBegunPlay()` is a compatibility query. It is true while beginning or
playing and false while ending, so EndPlay callbacks cannot create a newly
playing Actor. Spawn is accepted while stopped, beginning, or playing. A Spawn
while beginning or playing dispatches Actor BeginPlay exactly once through the
Spawn path. Spawn is rejected before allocation while the World is ending.
Repeated or recursive World BeginPlay and EndPlay calls are idempotent.

Process placement of World Tick and the GameThread completion pumps is defined
by [Runtime Lifecycle](../Core/RuntimeLifecycle.md).

## Native Gameplay Session

`DWorld::BeginPlay(const FWorldPlayRequest&)` is the only World play entry.
A request without a game-mode class starts the ordinary lifecycle only and
creates no gameplay Actors. A request with a constructible `AGameMode` class
transactionally creates one game mode, one local `APlayerController`, and one
default `APawn` through `DLevel`. The first live `APlayerStart` in stable Actor
order supplies the pawn transform. The World places and possesses the pawn,
publishes its private gameplay-session roles, and only then dispatches the
normal forward BeginPlay pass.

Any class, start, spawn, placement, possession, or view-target failure returns
an `FWorldPlayResult`, destroys partial runtime Actors in reverse creation
order, preserves authored Actors, and leaves the World stopped. EndPlay first
routes the normal reverse Actor pass and then destroys only the session-created
Actors. `RestartPlayer` preserves the controller, removes the old pawn, and
uses the same select/place/possess ordering; a failed restart leaves the
controller alive and unpossessed.

`AController` exclusively owns `Possess` and `UnPossess`. Controller and pawn
hold reciprocal transient pointers, validate live same-level membership before
mutation, detach prior pairs before attaching a transfer, and clear the pair on
EndPlay, destruction, World stop, and Level replacement. `DLevel::DestroyActor`
invokes protected `AActor::OnActorDestroyed()` exactly once after accepting
destruction and before EndPlay, component teardown, or membership removal, so
never-begun Actors use the same cleanup path.

`APawn` owns a root scene component and accepts at most one finite, clamped
`FPawnControlIntent` per advancing tick. It consumes that value exactly once
before delegating to its single pawn-owned `DPawnMovementComponent` authority.
The abstract base movement component exposes velocity and semantic movement
dispatch only; gravity, collision, grounding, and jump policy belong to a
concrete game module.

The opt-in `--native-gameplay-lifecycle-smoke` process diagnostic exercises the
native start, tick, pause-step, restart, and stop sequence in an isolated
temporary World after full host initialization, then restores the original
World. Ordinary startup never enables it.

## Lifecycle Mutation

World and Actor lifecycle passes never retain an iterator or element reference
into a mutable Actor or Component container across a virtual callback. Each
pass copies generation-checked `TObjectPtr` handles at entry and revalidates a
candidate immediately before publication. The snapshot is a finite entry set:
Spawn during traversal is not appended, and an object destroyed before its turn
is skipped.

Ordering is stable:

- World and Component BeginPlay are forward ordered.
- World and Component EndPlay are reverse ordered.
- the existing World Actor Tick snapshot is forward ordered.

World Tick receives an `FWorldTickContext`. On a tick admitted by pause or
single-step state, the local player controller translates the optional raw
`FGameInputState` snapshot into one source-neutral intent before the Actor tick
snapshot. A paused tick that does not advance produces no intent, and pending
semantic state is cleared at pause and possession boundaries.

Level switching ends the active World play lifetime before detaching the old
Level. A callback-driven clear or replacement stops the captured lifecycle
batch after the Level identity changes. During World EndPlay, Spawn is rejected
before object allocation.

`SetCurrentLevel` is an immediate stopped-World operation.
`RequestLevelTransition` retains the requested Level and, when applicable, the
active native game-mode class. The next World tick ends the old play session,
activates the requested Level, and resumes play. Only one pending transition is
applied per tick, so a transition requested by EndPlay or BeginPlay remains
deferred to a later safe point. A pending transition suppresses the remainder
of the current gameplay tick.

### Actor And Component Dispatch

Engine-owned code enters Actor lifecycle through non-virtual
`DispatchBeginPlay()` and `RouteEndPlay()`. The virtual `BeginPlay()` and
`EndPlay()` functions remain user extension points; derived implementations
call their base implementation when they want base Component routing. Actor
state is published before virtual code runs and distinguishes not begun,
beginning, playing, and ending.

Destroy requested during Actor BeginPlay or EndPlay is recorded and completed
after the active callback unwinds. Actor membership remains visible throughout
its EndPlay callback; owner-controlled removal and garbage marking follow it.
Destroying an Actor already being destroyed succeeds without repeating
EndPlay, Component teardown, or Level removal. An Actor destroyed before it has
begun play receives no synthetic EndPlay.

Component BeginPlay and EndPlay use equivalent engine-owned dispatch and
destruction states. Actor Component BeginPlay uses a forward snapshot and
EndPlay uses a reverse snapshot. Owner, membership, registration, retirement,
and play state are revalidated before publication. Components added while an
Actor is beginning or playing begin exactly once through the add path.
Components added while the Actor is ending remain registered and owned but do
not begin in that ending lifetime. Self-destruction during Component BeginPlay
or EndPlay completes after the active callback returns and removes owned and
instance membership exactly once.

## Actor Iteration

`FActorRange` and `FActorIterator` provide finite, game-thread Actor
enumeration. A range captures the World's current Level and makes one
contiguous copy of its `TObjectPtr<AActor>` candidates. Iterator copies share
that stable candidate state; they never borrow a `std::vector` iterator or
register a Spawn callback.

Before dereference, the iterator filters candidates whose captured World or
Level is no longer valid, whose Level no longer belongs to the captured World,
whose structural Level membership changed, or which are destroying or pending
kill. `FActorIteratorFilter::ActorClass` optionally accepts only Actors whose
reflected class derives from the requested class without using C++ RTTI.
`bRequireCurrentLevel` additionally requires the captured Level to remain the
World's active Level.

The initial candidate count never changes. Actors spawned after range
construction are not observed, so iteration terminates even when each visited
Actor spawns another. Destroying the current or a later candidate is safe; the
invalid entry is skipped when iteration advances. Replacing the active Level
invalidates every remaining candidate from the captured Level.

Lifecycle dispatch keeps its explicit forward or reverse snapshots rather than
routing through this public range, and the per-frame Tick path is not migrated
to it. Those paths retain their own ordering and performance contracts.
