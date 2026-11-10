# World Subsystems

Summary: Define native per-World service registration, ownership, lifecycle, Tick, and asynchronous retirement.

Modules: Engine, Core, DurinEd

Last reviewed: 2026-11-10

## Registration And Ownership

A `DWorldSubsystem` is a transient native service owned by one `DWorld`.
Implement a concrete reflected subclass and hold an `FWorldSubsystemRegistration`
in the providing module. Register during module startup, before affected Worlds
initialize, and release the registration during module shutdown. The descriptor
is an `FSubsystemDescriptor` naming the concrete `DClass` and providing module.
Dependencies are requested inside `Initialize(Collection)` using
`InitializeDependency<T>()`. An empty provider is reserved for statically linked native fixtures;
production providers name their actual module using `FModuleStartup::GetModuleName()`.

World types are `Game`, `Editor`, `PIE` and `EditorPreview`; the last two replace
the former PlayInEditor and Preview names while preserving their numeric values.
Play state remains independent of World type. Additional UE World types are not
introduced without matching host behavior.

`DWorldSubsystem::ShouldCreateSubsystem` checks the prospective World and calls
`DoesSupportWorldType(EWorldType) const` on the concrete class's CDO. The default
supports Game, Editor and PIE, excluding EditorPreview. Override this method to
admit a tool-preview service or restrict it to particular World types. Further
host conditions can override ShouldCreateSubsystem and must preserve the base
World eligibility check. Registration has no WorldTypes list or opaque policy.
Collision debug and material parameter collection services explicitly support
EditorPreview to retain their tool behavior. Tick capability is selected by deriving
from `DTickableWorldSubsystem`, independently of creation eligibility.

`FWorldSubsystemRegistration` publishes into the shared catalog under the World scope, and
`FWorldSubsystemCollection` snapshots descriptors once. Registration removal or
addition affects subsequent Worlds, without replacing existing objects or
changing live callback membership. Native `DClass` construction is the factory;
there is no reflected-class scan and no global subsystem instance.

The World reports collection objects through `AddReferencedObjects`. Each
object also has the World as Outer, but Outer alone does not retain it. Native
collection fields are absent from serialization, and `Transient` objects are
excluded from child duplication. PIE clones the Level and creates fresh
services. `GetSubsystem<T>()` performs exact concrete lookup, returns null for
absent or incompletely initialized types, and never constructs an object.
Registration, lookup, mutation, and lifecycle callbacks are game-thread-only.

Shared dependency initialization, failure cleanup and GC retention are owned by
[the common subsystem contract](../Engine/Subsystems.md). `DWorldSubsystem` derives
from `DSubsystem`; there is no World-specific subsystem state enum.
World eligibility, play and Level notifications stay in this adapter; Tick
policy belongs to Tickable. The collection uses its inherited Host reference
and does not keep a second World reference.
Typed World lookup rejects Engine and Editor subsystem classes.

## Initialization And Shutdown

Hosts select World type and set required host endpoints, then call
`InitializeSubsystems()` before Level attachment or publication. World type is
immutable after initialization starts. Empty Worlds and absent render scenes
are supported. Changing the render scene does not recreate the collection.

Package-owned editor Play worlds pass their Engine explicitly to
`InitializeSubsystems(Host)`. The World keeps a weak host association for the
initialization scope, world-context registration and shutdown removal. Ordinary
Engine-owned Worlds infer the host from Outer; preview Worlds can remain hostless.
An initialized host association cannot be replaced by another Engine.

`AreSubsystemsInitialized()` reports collection initialization; it does not
replace World operation and shutdown admission checks.
Initialization returns void; invalid configuration and lifecycle entry assert.
It checks creation eligibility per class before constructing it.
Dependencies initialize recursively when requested by Initialize; recursive
cycles assert, while unavailable dependencies return null for the consumer to check. Repeated
registration of an existing class reuses its instance.
Initialization dependencies do not create arbitrary Actor Tick prerequisites.

Only completed initialization becomes lookup-visible. Initialization exceptions
propagate after cleanup. On failure, the failed object receives `Deinitialize` too,
then predecessors unwind in reverse order. Collection references
are released for ordinary GC, and `AreSubsystemsInitialized()` becomes false. A host must
not publish it. Cleanup and other lifecycle callbacks are `noexcept`.

`BeginTearingDown()` records retirement and rejects further forward dispatch without
cleaning resources. Callbacks use this request; only owners call `CleanupWorld()`
at an idle World boundary. Cleanup ends play,
unregisters Level components, detaches the Level, deinitializes services in
reverse order, and clears the render endpoint. Services remain lookup-visible
for Actor EndPlay and component unregistration. Completed shutdown is idempotent,
and shutdown after partial failure is safe. World cleanup completion is tracked
by the World itself: a collection already cleaned after an initialization
exception does not skip World resource and context teardown. Reinitialization is not supported.
The request does not cancel collection initialization. An already-started batch
finishes normally unless a callback throws, with external admission closed.
The World operation boundary runs shutdown after initialization and callbacks unwind;
external consumers validate World retirement and clear their own endpoints.
`DWorld::BeginDestroy` is a fallback; runtime, editor, PIE, and preview hosts explicitly
shut down before releasing owned scene resources. Suspending EditorWorld through
`DEngine::SetWorld` during PIE is not retirement.
World subsystem destruction does not shut down its World or wait for World
finish-destroy readiness; the World owns service cleanup.

## Play And Level Boundaries

Native gameplay bootstrap first validates and creates/possesses its gameplay
roles. Failure at this point produces no subsystem play callbacks. Then services
receive `OnWorldBeginPlay` in initialization order. Actors spawned by those
callbacks remain stopped until all service callbacks finish; the World then
snapshots the complete Actor list and dispatches Actor BeginPlay. Each service
receives one reverse `OnWorldEndPlay`, after Actor EndPlay and removal
of native session Actors. The same service can observe multiple play lifetimes.

Begin/End and Attach/Detach notifications are complete batches over fixed
membership. A callback may request EndPlay, retirement or a Level transition,
but cannot interrupt the current batch or destroy its resources. The World
checks admission after the batch, before starting Actor BeginPlay or component
registration. An aborted BeginPlay returns `PlayAborted` and pairs the entire
Begin batch through reverse End notification. Bootstrap failure before entering
the Begin batch produces no End notification. World play state and the attached
Level guarantee pairing; the collection keeps no per-service playing/attached flags.
A queued Level transition remains for the next World Tick, including its
captured resume-play request. An admitted generic Tickable batch also completes
before owner cleanup; explicitly disabling or unregistering a later Tickable still prevents its callback.

World operations, rather than individual subsystem callbacks, define the
non-reentrant execution boundary. `SetCurrentLevel` rejects operation reentry;
use `RequestLevelTransition`. EndPlay and retirement requests close new work
admission immediately but execute only after the complete operation, including
Tick registry cleanup, exits. Input, Actor, Component, and Subsystem extensions
inside that operation obey the same rules. Nested synchronous BeginPlay and
World Tick cannot start a new play lifetime or frame. See
[World Operation Boundaries](LevelSystem.md#world-operation-boundaries).

Attachment sets the Level's World endpoint and notifies services before
registering existing components. Detachment unregisters components first,
notifies all services in reverse order while the old Level is
still identifiable, then removes its World endpoint. Services release
Level-derived references on detach and survive Level replacement. Both pending and executing
transition Levels are explicitly reported to GC. Collection requested inside a
World operation is deferred to a later collector safe point.

## Tick And Work Retirement

Ordinary `DWorldSubsystem` has no Tick API or enabled state. Derive from
`DTickableWorldSubsystem` and implement `Tick(float DeltaSeconds)` for per-frame
work. This follows UE's [UTickableWorldSubsystem](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/UTickableWorldSubsystem)
lifecycle: overrides of `Initialize(Collection)` and `Deinitialize()` must call
Super. Initialization enables ticking; deinitialization disables it.
BeginDestroy diagnoses missing deinitialization instead of performing it. `IsInitialized()` reports this state. `IsAllowedToTick()` checks
initialization before other tick queries. This compatibility hook is final;
override IsTickable for dynamic conditions. Tickable subclasses call
Super::Initialize before fallible work; failure after that call still receives
paired Deinitialize. Initialization state is private and pairing is checked.
The World owns safe dispatch and cleanup boundaries; this state does not replace
owner admission or allow callbacks to destroy their host.

DTickableWorldSubsystem also inherits FTickableGameObject. It starts unregistered
with Never, registers during Initialize, and unregisters during Deinitialize.
The default GetTickableTickType is Conditional for initialized instances and
Never for CDOs or uninitialized instances. The subsystem collection owns retention and lifecycle, but has
no Tick dispatch responsibility.

Services participate in the [generic Tickable dispatcher](../Engine/TickableObjects.md),
once per admitted World Tick after Actor/Component groups and timers.
Initialization dependencies do not guarantee Tick order. There is no descriptor
TickGroup, Actor prerequisite, or per-frame enabled snapshot. Conditional objects
query IsTickable immediately before dispatch, so an earlier callback can change
a later object's eligibility in the same frame. GetTickableGameObjectWorld
returns the owning World.

Initialized Game and PIE Worlds tick services even without a Level or BeginPlay,
and continue after EndPlay. Editor and EditorPreview require
`IsTickableInEditor()` (default false), independently of
`DoesSupportWorldType()`. Paused gameplay requires `IsTickableWhenPaused()`
(default false), unless editor-process opt-in independently admits the object; a gameplay single step admits one normal tick. DeltaSeconds is
the validated World delta multiplied by TimeScale. Ticking while paused uses
that delta only for opted-in services. The editor host also ticks its suspended
EditorWorld while PIE is active. No background timer is created.

Pending EndPlay, Level transition, World teardown, or host retirement prevents
entry into a new Tickable phase. A request inside an admitted Tickable batch
does not interrupt its remaining eligible objects. The World operation retains
resources through the batch and applies cleanup on exit; a Level transition is
processed on the next World frame. Subsequent admitted frames can tick retained
services after EndPlay. Complete lifecycle notification batches retain
their existing pairing contract. Actor/Component scheduling remains governed by
[Tick Scheduling](TickScheduling.md).

Each service owns cancellation for its asynchronous work, using the existing task
or tool cancellation APIs. Capture detached CPU inputs and consumer-owned tokens;
never capture a subsystem, World, Level, or host endpoint for Worker access.
Game-thread completion checks cancellation and the original World's shutdown state
before publishing. `Deinitialize` cancels work, unregisters external callbacks and
releases owned resources. Module-provided task callables additionally use the
module-owned async operation scope so code and retained results remain audited.
See [work and code lifetime](../Engine/Subsystems.md#work-and-code-lifetime) for
shared cancellation and draining requirements. This framework owns no scheduler
or worker threads.

## Collision Debug Service

Engine registers `DCollisionDebugSubsystem`, whose applicability includes every supported World type. It owns debug
enablement (default false) and the last recorded hit. World query and debug
facades retain their signatures and snapshot limits. Disabling debug, Level
detachment, and shutdown clear the last hit; detach preserves enablement for
the next Level. Physics scene ownership and component/body registration stay
in their existing owners. See [Runtime Collision](../Physics/Collision.md).
