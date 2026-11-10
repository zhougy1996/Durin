# World Subsystems

Summary: Define native per-World service registration, ownership, lifecycle, Tick, and asynchronous retirement.

Modules: Engine, Core, DurinEd

Last reviewed: 2026-11-10

## Registration And Ownership

A `DWorldSubsystem` is a transient native service owned by one `DWorld`.
Implement a concrete reflected subclass and hold an `FWorldSubsystemRegistration`
in the providing module. Register during module startup, before affected Worlds
initialize, and release the registration during module shutdown. The descriptor
names the concrete `DClass`, providing module and optional Tick policy.
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
EditorPreview to retain their tool behavior. Tick opt-in remains independent
of creation eligibility. World Tick configuration is validated only for eligible
services, before that service instance is constructed.

`FWorldSubsystemRegistration` owns a strongly typed registration catalog, and
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
from `DSubsystem`; the World state name remains a compatibility alias.
World eligibility, play, Level and Tick policy stay in this adapter.
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

The one-shot state progression is `Uninitialized -> Initializing -> Ready`.
Initialization returns void; invalid configuration and lifecycle entry assert.
It checks creation eligibility per class before constructing it.
Dependencies initialize recursively when requested by Initialize; recursive
cycles assert, while unavailable dependencies return null for the consumer to check. Repeated
registration of an existing class reuses its instance.
Initialization dependencies do not create arbitrary Actor Tick prerequisites.

Only completed initialization becomes lookup-visible. Initialization exceptions
propagate after cleanup. On failure, the failed object receives `Deinitialize` too,
then predecessors unwind in reverse order. External references are invalidated, collection references
are released for ordinary GC, and the collection enters terminal `Shutdown`. A host must
not publish it. Cleanup and other lifecycle callbacks are `noexcept`.

`Shutdown()` immediately records retirement and rejects further forward dispatch. It ends play,
unregisters Level components, detaches the Level, deinitializes services in
reverse order, and clears the render endpoint. Services remain lookup-visible
for Actor EndPlay and component unregistration. Completed shutdown is idempotent,
and shutdown after partial failure is safe. Reinitialization is not supported.
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
snapshots the complete Actor list and dispatches Actor BeginPlay. Each entered
service receives one reverse `OnWorldEndPlay`, after Actor EndPlay and removal
of native session Actors. The same service can observe multiple play lifetimes.

A callback that requests EndPlay, World shutdown, or a Level transition stops
later forward callbacks. An interrupted BeginPlay returns `PlayAborted` and
pairs entered callbacks through EndPlay. A queued Level transition remains for
the next World Tick, including its captured resume-play request. Teardown still
performs the reverse cleanup needed to retire all entered services.

World operations, rather than individual subsystem callbacks, define the
non-reentrant execution boundary. `SetCurrentLevel` rejects operation reentry;
use `RequestLevelTransition`. EndPlay and shutdown requests stop forward
admission immediately but execute only after the complete operation, including
Tick registry cleanup, exits. Input, Actor, Component, and Subsystem extensions
inside that operation obey the same rules. Nested synchronous BeginPlay and
World Tick cannot start a new play lifetime or frame. See
[World Operation Boundaries](LevelSystem.md#world-operation-boundaries).

Attachment sets the Level's World endpoint and notifies services before
registering existing components. Detachment unregisters components first,
notifies previously attached services in reverse order while the old Level is
still identifiable, then removes its World endpoint. Services release
Level-derived references on detach and survive Level replacement. Both pending and executing
transition Levels are explicitly reported to GC. Collection requested inside a
World operation is deferred to a later collector safe point.

## Tick And Work Retirement

Tick requires descriptor opt-in. World Tick snapshots each service's enabled
flag at entry; `SetTickEnabled` changes admission for the next World Tick.
Services execute at most once per World Tick, in their selected `PrePhysics`,
`Physics`, or `PostPhysics` phase, in initialization order before that phase's
Level registry. Their registration belongs to the World and survives Level
registry reset.

Gameplay admission uses play plus the existing pause/single-step decision.
Stopped Game and PIE Worlds do not tick services. An explicit
`bTickInEditorAndPreview` policy allows Editor/Preview services to run without
play or a Level, including host ticks while gameplay is paused. The editor host
also ticks its suspended EditorWorld while PIE is active. No background timer
is created. Each callback boundary rechecks pending transitions, retirement,
and the admitted play lifetime. Actor/Component scheduling remains governed by
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
