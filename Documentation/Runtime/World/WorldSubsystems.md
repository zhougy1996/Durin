# World Subsystems

Summary: Define native per-World service registration, ownership, lifecycle, Tick, and asynchronous retirement.

Modules: Engine, Core, DurinEd

Last reviewed: 2026-09-07

## Registration And Ownership

A `DWorldSubsystem` is a transient native service owned by one `DWorld`.
Implement a concrete reflected subclass and hold an `FWorldSubsystemRegistration`
in the providing module. Register during module startup, before affected Worlds
initialize, and release the registration during module shutdown. The descriptor
names the concrete `DClass`, providing module, supported World types, required
concrete dependencies, and optional Tick policy. An empty World-type list means
all types. An empty provider is reserved for statically linked native fixtures;
production providers name their actual module using `FModuleStartup::GetModuleName()`.

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

`FModuleManager::AcquireCodeLease` pins the exact active load generation.
Collections retain leases for frozen descriptors, objects retain them through
physical GC destruction, and retained work gates keep provider and Engine code
mapped. Shutdown rejects outstanding leases with `OutstandingCodeLease` before
entering retirement; the module remains active and shutdown may be retried after
consumers release their leases. This is separate from bounded ModularFeature
invocation and module async-operation audits.

## Initialization And Shutdown

Hosts select World type and set required host endpoints, then call
`InitializeSubsystems()` before Level attachment or publication. World type is
immutable after initialization starts. Empty Worlds and absent render scenes
are supported. Changing the render scene does not recreate the collection.

The one-shot state progression is `Uninitialized -> Initializing -> Ready`.
Initialization returns `FWorldSubsystemResult` with a categorized error and
message. It filters by World type before checking duplicate types, required
dependencies, and cycles. Topological selection chooses the first available
qualified class name in lexical order; dependencies initialize before users.
Initialization dependencies do not create arbitrary Actor Tick prerequisites.

Only completed initialization becomes lookup-visible. Initialization exceptions
become failures. On failure, the failed object receives `Deinitialize` too,
then predecessors unwind in reverse order. All gates close, constructed objects
are marked as garbage, and the collection enters terminal `Failed`. A host must
not publish it. Cleanup and other lifecycle callbacks are `noexcept`.

`Shutdown()` immediately closes work gates and records retirement. It ends play,
unregisters Level components, detaches the Level, deinitializes services in
reverse order, and clears the render endpoint. Services remain lookup-visible
for Actor EndPlay and component unregistration. Completed shutdown is idempotent,
and shutdown after partial failure is safe. Reinitialization is not supported.
`BeginDestroy` is a fallback; runtime, editor, PIE, and preview hosts explicitly
shut down before releasing owned scene resources. Suspending EditorWorld through
`DEngine::SetWorld` during PIE is not retirement.

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

Subsystem callbacks cannot synchronously change the active Level. `SetCurrentLevel`
rejects such reentry; use `RequestLevelTransition`. EndPlay and shutdown requests
from subsystem callbacks apply after the callback stack unwinds. Callback depth
also prevents recursive World Tick and premature service/World destruction.

Attachment sets the Level's World endpoint and notifies services before
registering existing components. Detachment unregisters components first,
notifies previously attached services in reverse order while the old Level is
still identifiable, then removes its World endpoint. Services release
Level-derived references on detach and survive Level replacement. The pending
transition Level is explicitly reported to GC.

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

`GetWorkGate()` returns a shared, one-shot lifetime identity and cancellation
token. Capture detached CPU inputs and the gate for asynchronous work; never
capture a subsystem, World, Level, or host endpoint for Worker access. The
GameThread publication boundary must check `IsOpen()` before resolving or
mutating World state. Each World receives fresh gates, so a completion from a
retired World cannot publish into a later one. Shutdown closes gates without
waiting; retained gates continue to reject publication after physical GC.

A service unregisters external callbacks and releases owned resources in
`Deinitialize`. Module-provided task callables additionally use the existing
module-owned async operation scope so code and retained results remain audited.
Cancellation is cooperative: closing a gate does not stop a running Worker or
release its captured data. Detached captures remain alive until work retires.
Do not synchronously wait on a task graph that requires GameThread completion.
This framework uses the current task API and owns no scheduler or worker threads.

## Collision Debug Service

Engine registers `DCollisionDebugSubsystem` for every World. It owns debug
enablement (default false) and the last recorded hit. World query and debug
facades retain their signatures and snapshot limits. Disabling debug, Level
detachment, and shutdown clear the last hit; detach preserves enablement for
the next Level. Physics scene ownership and component/body registration stay
in their existing owners. See [Runtime Collision](../Physics/Collision.md).
