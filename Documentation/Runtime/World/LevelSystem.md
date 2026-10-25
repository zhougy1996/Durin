# Level System

Summary: Define levels, actors, world ownership, lifecycle mutation, and iteration behavior.

Modules: Engine

Last reviewed: 2026-10-21

`DLevel` is the persistent scene asset. A packaged level is a top-level asset
in a `.dasset` package. Levels retain actors through reflected `TObjectPtr` arrays.
Each Actor's transient reflected `OwnedComponents` array is the ordered runtime
authority for every live component. Native inner objects persist through the
shared object graph; `InstanceComponents` records persistent instance membership.
`ResetOwnedComponents` derives live ownership from that graph and membership.
The Outer hierarchy provides structural containment and object paths.

Actor object `Name` and serialized `ActorLabel` have different contracts. Object
names identify paths and are unique across live objects directly under the Level,
including unpublished preparations and objects retained for Undo. Runtime and
editor spawn/rename paths share `DLevel::ResolveActorName`: `Requested` permits
suffixes, `RequiredReturnNull` rejects a collision, and
`RequiredErrorAndReturnNull` also logs the rejection. None of these modes replaces
an existing object. With no requested Name, spawn generates a unique class-based
name regardless of NameMode. `FindActorByName` queries published membership only.

`GetActorLabel` supplies the human-readable display value. Old assets without
`ActorLabel` fall back to their object Name and materialize it on load; internal
renaming preserves that label. Labels are serialized, duplicated and restored
independently of object names. `MakeUniqueActorLabel` considers published Actors
and optional batch reservations, so Undo-retained objects do not occupy display
labels. Duplicate labels are allowed when explicitly requested; labels are not
identity keys. Persistent reflected `Tags` support script/application membership
markers independently of mutable names and labels.

`ActorLabel` is transactional string storage (`std::string`, corresponding to
UE's `FString`); object Name remains `FName`. `SetActorLabel` calls `Modify`,
assigns the value and emits its property notification. Label-only notifications
refresh the hierarchy without rerunning construction. `SetInitialActorLabel`
prepares an unpublished Actor without recording or notifications and rejects
published Level members. Editor admission and optional label uniqueness belong
to `DEditorActorSubsystem`; ordinary label and object-name history use the shared
whole-object record rather than dedicated rename commands.

Actor presence commands capture the Level through the common SaveObject path
before native publication. The resulting object record owns Actor-array order
and primary-camera history. Native presence replay uses the Level's temporary
PreEditUndo membership baseline to recognize an already-loaded array without
accepting unrelated earlier membership edits. It then registers/unregisters the
exact retained Actor through the normal lifecycle boundary; ordinary field
restoration alone does not activate it.
As in UE's Level completion, still-live Actors omitted by an older array are
preserved. Durin distinguishes reversible absence by preserving pre-replay
published Actors only when no presence adapter changed them in that operation;
older absent objects are not accidentally republished. This also preserves
nontransactional Actors added outside the recording.

Actors index transient generated components by stable native-construction keys.
Those components also participate in unified live ownership and World/editor
lifecycles but are excluded from authored package and duplication state. See
[Native Actor Construction](NativeConstruction.md).

`DLevel::SetActorPresence` provides reversible editor membership without changing
object identity or terminal destruction state. Removal unregisters Actor Tick and
components, updates Level ordering and primary-camera fallback, and leaves owned
component graphs intact. Restoration accepts an insertion index and can defer
component registration until the caller restores attachment state. It rejects
play transitions, invalid ownership, duplicate membership and name collisions.
The editor owns attachment snapshots, registration-state restoration, collector
retention and persistence admission; see
[Actor structure commands](../../Editor/Architecture/ViewportEditing.md#actor-structure-commands).

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

Hosts explicitly initialize [World subsystems](WorldSubsystems.md) before
Level attachment. Services survive Level changes; attachment notifications
precede component registration and detachment notifications follow unregistration.

A world starts without an active level. Actor operations safely return empty or fail until a level is activated. The editor supports this empty state: scene panels remain available, while level-dependent editing actions are disabled.

Scene persistence stores the actor list, primary camera, component relative transforms, attachment parents, and camera projection settings. Attachment children and world transforms are derived. After package fields are applied, `DLevel::PostLoad` validates ownership and attachment cycles, rebuilds child lists, and recalculates world transforms before the package load is published.

The Level Editor exposes New, Open, and Save using virtual asset paths and the
Asset Registry. Dirty package switches require Save, Discard, or Cancel.
Static-mesh components persist cross-package references to `DStaticMesh`
assets. A static-mesh asset serializes canonical imported geometry, an optional
schema-2 reimport hint, exact imported-data identity, and build settings;
CPU/GPU render data is restored from DDC or rebuilt from canonical authored
geometry. Version one intentionally supports a single active level and does not
include sub-level streaming, PIE cloning, or Save As.

At startup the editor opens the project's optional `Game.DefaultLevel` from
`Configs/Project.yaml`. `FProjectGameSettingsStore` is the single parser and
writer used by the Level Editor, standalone startup, and the project-settings
reference contributor. YAML stores a package path rather than exposing the
package's exact top-level object spelling. Startup and the editor follow package
redirects, then require the resolved package to contain exactly one top-level
`DLevel`; zero or multiple matching assets are invalid. The editor retains that
resolved exact object identity internally, while its picker displays and saves
the package path without loading the level. Relocation leaves YAML unchanged.
The reference store contributes the default as a typed external Cook root and
canonicalizes it in memory for runtime output without editing YAML; explicit
redirector Fix Up is the only transaction that rewrites the setting. Projects
without a default level start with an empty editor; missing or invalid defaults
and failed level loads are non-fatal.

## Engine World Contexts

One active editor engine owns the editor session and its services. Editor, PIE
and preview Worlds are separate contexts, not separate editor engines.
`DEngine::GetWorldContexts()` and `GetWorldContextFromWorld()` expose borrowed
role/World views. Contexts contain weak identities; existing engine fields or
preview owners retain World lifetime. Do not retain a context address across
registration/removal. SetWorld registers an active World, and World shutdown
removes its context. Successful World subsystem initialization automatically
registers an engine-owned World; failed or retiring initialization does not
publish one. Context mutation is private to Engine and World. Preview owners
and tools never maintain the registry manually.

`DEditorEngine::GetEditorWorldContext()` requires an initialized editor World;
`GetPIEWorldContext()` returns null outside PIE. Context roles do not change the
single shared editor transaction history. World operation boundaries and render
fences still govern destruction. The context list also closes World admission
and checks operation quiescence during engine shutdown. After retiring editor
consumers, Engine shuts down any remaining registered Worlds before engine
subsystems, including initialized Worlds that never became the active World.

## World Play State

`DWorld` publishes one authoritative game-thread play state:

```text
Stopped -> BeginningPlay -> Playing -> EndingPlay -> Stopped
```

The runtime lifecycle is component registration and initialization, World
BeginPlay, Actor and Component BeginPlay, admitted Tick, Actor and Component
EndPlay, then component uninitialization and unregistration.

`HasBegunPlay()` is a compatibility query. It is true while beginning or
playing (after subsystem BeginPlay dispatch) and false while ending, so EndPlay callbacks cannot create a newly
playing Actor. Spawn is accepted while stopped, beginning, or playing. A Spawn
while beginning or playing dispatches Actor BeginPlay exactly once through the
Spawn path. Spawn is rejected before allocation while the World is ending.
Repeated or recursive World BeginPlay and EndPlay calls are idempotent.

Process placement of World Tick and the GameThread completion pumps is defined
by [Runtime Lifecycle](../Application/RuntimeLifecycle.md).

## World Operation Boundaries

World initialization, BeginPlay, Tick, EndPlay, Level changes, player restart,
render-scene changes, and shutdown execute on the Game Thread in explicit
non-reentrant phases. Public entry points reject incompatible synchronous
operations. Private implementations compose the steps of transition and
shutdown without reopening public admission. Play state describes gameplay;
the operation phase describes the active execution stack.

`GetPlayState()` exposes the full play phase, including subsystem BeginPlay while
`HasBegunPlay()` is still false. `IsShutdownRequested()` exposes the closing
intent before deferred teardown. Editor structural commands and history replay
use both queries to reject work throughout those lifecycle boundaries.

EndPlay and shutdown called during an operation record intent. Shutdown also
closes subsystem work gates immediately. Forward dispatch stops at the next
callback boundary, while reverse cleanup remains paired. The operation exits
before applying shutdown (highest priority) or EndPlay. In Tick, input callbacks
are followed by admission checks before StartFrame; every started registry frame
ends before stop requests execute. Recursive World Tick and BeginPlay are
rejected, including calls from input and Actor extensions.

Level transition requests execute at the next Tick entry, at most one per
entry. World transfers the pending request to active storage before EndPlay;
both storage locations enumerate their target references to GC. Callbacks may
publish a new pending request without overwriting the active target. A new
request interrupts forward admission and remains for the following Tick.
Shutdown prevents target publication. Garbage or foreign targets are rejected.
Services must tolerate a Level attachment interrupted by a newer request.

Complete operations defer physical garbage collection through CoreDObject.
Leaving an operation never runs collection; it only permits the existing host
GC safe point or a later explicit collector call to service the retained request.
Explicit garbage marking still invalidates objects immediately. The deferral
protects storage, not permission to keep dispatching to retired objects. The
[Runtime Lifecycle](../Application/RuntimeLifecycle.md) owns the process GC boundary.

## Native Gameplay Session

`DWorld::BeginPlay(const FWorldPlayRequest&)` is the only World play entry.
A request without a game-mode class starts the ordinary lifecycle only and
creates no gameplay Actors. A request with a constructible `AGameMode` class
transactionally creates one game mode, one local `APlayerController`, and one
default `APawn` through `DLevel`. The first live `APlayerStart` in stable Actor
order supplies the pawn transform. The World places and possesses the pawn,
publishes its private gameplay-session roles, and only then dispatches the
subsystem callbacks followed by the normal forward Actor BeginPlay pass.

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

## Lifecycle Mutation

World and Actor lifecycle passes never retain an iterator or element reference
into a mutable Actor or Component container across a virtual callback. Each
pass copies generation-checked `TObjectPtr` handles at entry and revalidates a
candidate immediately before publication. The snapshot is a finite entry set:
Spawn during traversal is not appended, and an object destroyed before its turn
is skipped.

Pure component reads use the Actor's allocation-free `GetComponents()` view.
Only mutation-capable lifecycle and notification boundaries create an explicit
`GetComponentsSnapshot()`; callers must not retain live-view iterators across
virtual or externally supplied callbacks.

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
of the current gameplay tick. It also interrupts subsequent World BeginPlay
callbacks: BeginPlay reports `PlayAborted`, pairs entered callbacks through
EndPlay, and leaves the pending transition for the next tick.

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
