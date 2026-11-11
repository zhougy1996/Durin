# Tickable Objects

Summary: Define non-owning Tickable registration, World and Engine dispatch, and callback lifetime.
Modules: Engine
Last reviewed: 2026-11-11

## Registration And Ownership

`Tickable.h` exposes `FTickableObjectBase` and `FTickableGameObject`, following
UE's [Tickable interfaces](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FTickableGameObject?application_version=5.5).
These are ordinary C++ bases, independent of DObject and subsystem ownership.
Implement Tick and associate an optional World through GetTickableGameObjectWorld.
The process-wide registry stores non-owning pointers; it does not keep DObjects
alive or own a World, provider module, or plain C++ object.

Default construction queues a NewObject registration. Before any callbacks in a
dispatch, GetTickableTickType resolves pending registrations to Conditional, Always, or Never. Conditional
queries IsTickable each eligible dispatch; Always bypasses that condition but
still observes IsAllowedToTick, World association, and pause/editor policy. Never removes the object from dispatch. SetTickableTickType changes
the instance policy without re-querying GetTickableTickType, while pending entries may still use NewObject. Active entries cannot return to
NewObject; disable and register again if a new registration is needed. Unresolved or invalid types are contract errors.

Registry access and dispatch are game-thread-only. Objects constructed off-thread
or before initialization must start with Never and activate on the game thread
after construction. Registered objects must unregister before destruction can
run on another thread. Never construction/destruction does not access the registry.
Copying and moving registered-capable objects are disabled.

Destruction unregisters a plain C++ object. DObject implementations must disable
ticking during BeginDestroy or earlier, rather than waiting for their C++
destructor. Providers disable registrations and destroy their objects before
unloading callback code. World-associated owners unregister before releasing the
World. Registration itself provides no lifetime or module-unload protection.

## Dispatch And Mutation

DWorld calls TickObjects for itself after Actor/Component groups and timers,
inside its existing World operation boundary. GameEngine calls the same dispatcher
for a null World after its World ticks with LEVELTICK_All and bIsPaused=false.
EditorEngine dispatches null-World objects only when it dispatched a World, using
LEVELTICK_All if a game World was dispatched, otherwise ViewportsOnly. Editor's suspended EditorWorld is ticked
before the active World and the null-World phase. A standalone World tick never
dispatches null-World objects.

GetTickableGameObjectWorld must equal the dispatched World, including nullptr.
Game/PIE use LEVELTICK_All; Editor/EditorPreview use LEVELTICK_ViewportsOnly.
GameEngine can dispatch null-World objects without a World. EditorEngine cannot;
its mode comes from the current frame, not the initial host type. Null-World
dispatch is not paused by an individual World.
IsTickableInEditor and IsTickableWhenPaused default to false.
Launch sets GIsEditor from process mode, independently of the active World.
Editor opt-in is an alternative admission branch, including paused and TimeOnly
dispatches. Otherwise, a game World (Game/PIE or LEVELTICK_All) ticks when
unpaused and not TimeOnly, or when paused and explicitly opted in.
PauseTick does not independently imply pause; the bIsPaused argument controls it.
World single-step supplies an unpaused dispatch.
World delta is scaled by World TimeScale; null-World delta is the Engine delta.
World and Engine frame entries reject invalid or negative deltas. Direct
TickObjects callers must supply a valid nonnegative delta; the generic dispatcher
does not repeat host input validation.

Each registered object executes at most once per matching dispatch. Tick may
register, disable, or destroy objects, including itself. New registrations
become eligible on the next TickObjects call, which may be another World's
dispatch in the same Engine frame. Disabling or destroying a later object
prevents its current callback. Disabling and re-enabling queues a fresh
registration; it cannot reuse the invalidated entry in the current pass.
In-place policy changes are visible to later entries. There is no dependency
ordering guarantee or Actor tick-group integration.

Policy queries are read-only: they must not mutate registration, destroy objects,
or perform host lifecycle operations. Tick dispatch itself cannot be nested.
A query exception propagates after restoring registry traversal state; Tick
callbacks are noexcept. Callback owners retain responsibility for resource and
code lifetime. TickObjects checks process exit once before preparing a batch,
matching UE 5.8. It has no Engine pointer, GEngine lookup, World subsystem query,
or per-object host retirement check. An admitted batch completes for objects
that remain registered and eligible, even if a callback requests process exit,
Engine shutdown, EndPlay, or a Level transition. Explicit unregister/disable
still suppresses a later object immediately.

World and Engine own phase admission: requests before the Tickable phase prevent
entry, and requests inside it close subsequent phases or frames. World retains
resources until its operation exits; Engine PreExit and immediate GC run after
the complete call chain returns. Callbacks request shutdown or GC rather than
performing owner cleanup themselves. Use Engine `ForceGarbageCollection()` or
Engine-free `RequestGarbageCollection()`; the owner executes collection after the
whole dispatch, including null-World batches, returns. See [Subsystems](Subsystems.md#common-lifecycle).

This layer introduces no worker scheduler, GC retention, generic host operation
scope, or profiling-ID abstraction. UE's GetStatId is not exposed because Durin
does not have its TStatId statistics infrastructure.

## Tickable World Subsystems

DTickableWorldSubsystem inherits DWorldSubsystem and FTickableGameObject.
Its constructor starts with Never. Initialize enables the resolved Tick type;
Deinitialize unregisters it. BeginDestroy diagnoses an instance still initialized;
it does not repair missing deinitialization. The default type is Conditional for
initialized instances and Never for template objects (including CDOs and default
subobjects) or uninitialized instances.
IsAllowedToTick is a final compatibility hook (deprecated upstream) that diagnoses
registration outside initialization; initialization state is private. Use
IsTickable for dynamic conditions. The default Tick checks initialization.

Initialize and Deinitialize must be paired exactly once. Tickable subclasses must
call Super::Initialize before any fallible setup, and Super::Deinitialize during
cleanup. Failure after Super::Initialize still receives paired Deinitialize from
the collection. Failure before that call is a Tickable lifecycle contract
violation, diagnosed during cleanup; the ordinary DSubsystem failed-initialization
contract is otherwise unchanged. Owners must finish Deinitialize before GC or
explicit BeginDestroy. Durin uses its existing fail-fast contract assertions for
these diagnostics, including those expressed as ensure in UE.

FWorldSubsystemCollection only creates, retains, notifies, and deinitializes
subsystems. It does not enumerate Tick callbacks. Dependencies control
initialization and reverse deinitialization, not Tick order. See
[World subsystems](../World/WorldSubsystems.md#tick-and-work-retirement) for play,
pause, and retirement behavior.
