# World Subsystem Framework Plan

Summary: Introduce per-World subsystem ownership, deterministic lifecycle and optional ticking, qualified through collision debug state and concurrent editor, PIE, and preview worlds.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

Stage 0 source audit is complete. The contracts below freeze the implementation
boundaries; Stage 1 is next. Runtime implementation and native validation remain
outstanding.

## Goal

A module can implement and register a World service without adding a dedicated
World member or editing World initialization, Tick, and destruction for that
service. Every eligible World receives its own instance, and host teardown
retires its work and resources deterministically.

## Scope And Non-Goals

- Implement `DWorldSubsystem`, `FWorldSubsystemCollection`, a native descriptor
  registration path, typed lookup, lifecycle dispatch, and optional Tick.
- Integrate explicit World initialization and shutdown into runtime, editor,
  PIE, and preview creation and retirement paths, including partial failures.
- Move existing collision debug enablement and last-hit state into a concrete
  World subsystem while retaining the public World collision/debug facade.
- Preserve `FPhysicsScene` ownership, physics-component registration, native
  gameplay bootstrap and rollback, possession, and renderer ownership.
- Do not introduce Engine, Editor, LocalPlayer, or persistent game-session
  subsystem families, automatic reflected-class discovery, hot reload, dynamic
  replacement of live services, or parallel Tick scheduling in this plan.
- Do not move the process CPU task scheduler into a World. Services own their
  submitted work and cancellation/completion lifetime, not worker threads.

## Selected Decisions

### Ownership And Discovery

The collection belongs to `DWorld`; subsystem objects use that World as Outer.
The collection must also retain them through the repository's supported GC
reference mechanism: Outer alone is not a reachability contract. Runtime
instances are transient and excluded from asset serialization and PIE cloning.
Lookup uses the requested concrete subsystem type and returns null when absent;
lookup never lazily creates an instance. One concrete type has at most one
instance per World. Access and lifecycle mutation are game-thread-only.

Modules register descriptors/factories, not global subsystem instances. Each
descriptor identifies its type, supported World types, initialization
dependencies, optional Tick policy, and providing module. Registration completes
before affected Worlds initialize. A collection freezes its selected descriptors
for its lifetime; later registration affects only subsequently created Worlds.
Provider code must remain loaded until all its instances and callbacks retire.
Stage 0 must verify how existing module ownership can enforce this; bounded
ModularFeature invocation alone is insufficient for escaped instances.

### Initialization And Failure

Add an explicit initialization boundary after World type and required host
context are known, before Level attachment and component registration. World
type becomes immutable after initialization. Empty Worlds are valid; subsystem
initialization cannot assume a Level or render scene exists. Existing render
scene replacement remains supported and does not recreate the collection.

Filter descriptors before resolving dependencies. A required dependency that
is unavailable for this World, a duplicate type, or a dependency cycle fails
initialization with a diagnostic. Dependencies initialize before dependents;
independent nodes use stable type identity ordering, not hash iteration order.
Initialization dependencies do not implicitly define gameplay Tick order.

Only initialized services are returned by lookup. Failed initialization retires
partial resources and reverses completed initialization before the host can
publish a ready World. Repeated shutdown and shutdown after partial failure are
safe. Stage 0 must fix the exact state/result APIs and failed-service cleanup
contract before runtime edits.

### World And Play Lifetimes

World initialization/shutdown and BeginPlay/EndPlay are separate lifetimes.
The same subsystem instance may observe multiple play lifetimes and Level
replacements. Editor and preview services do not require BeginPlay to exist.
Level-derived references must be released on detachment; attachment/detachment
notifications must bracket existing component membership transitions explicitly.

Services must be initialized before components can use them. Keep services
available during Actor EndPlay and component unregistration. World shutdown
then deinitializes services in reverse dependency order before clearing host
endpoints. Hosts must explicitly perform this shutdown before releasing owned
render resources; `BeginDestroy` is an idempotent fallback, not the only path.

BeginPlay callback placement must preserve native bootstrap's transactional
failure behavior, including Actors spawned during bootstrap. Callback-driven
EndPlay, World retirement, and Level transitions must stop later callbacks once
the current callback unwinds. Do not retain mutable collection iterators across
extension callbacks. Stage 0 records the exact play-entry/exit sequence and
rollback pairing against the existing implementation.

### Tick And Asynchronous Work

Tick is opt-in. Reuse the existing `PrePhysics`, `Physics`, and `PostPhysics`
phase meanings. Initially execute subsystem callbacks before the Level registry
within their selected phase; use stable dependency initialization order among
services in the same phase. Arbitrary Actor/subsystem prerequisites are outside
scope. Keep subsystem Tick registration World-owned so Level detachment cannot
silently erase a surviving service's Tick.

Gameplay-only Tick follows existing pause and single-step admission. An explicit
editor/preview Tick policy may run without play or an attached Level when the
host ticks that World. It does not create a background timer. Each eligible
service runs at most once per World tick; pending transitions, shutdown, and
play-state changes are rechecked at callback boundaries. Runtime collection
membership is fixed; enabling or disabling Tick takes effect at a documented
safe boundary rather than mutating the active iteration.

Shutdown closes work admission, unregisters external callbacks, cancels or drains
service-owned work, and prevents late completions from accessing retired World
state. Do not synchronously wait on work that needs the blocked game thread to
complete. Stage 0 selects the cancellation/generation and retirement mechanism
from the task API available at implementation time.

## Implementation Stages

### Stage 0: Resolve Lifecycle And Module Integration Contracts

- [x] Trace all runtime/editor/PIE/preview construction, publication, failure,
  Level switching, and retirement paths; record insertion points.
- [x] Verify reflected object retention, transient duplication exclusions,
  factory identity, and provider-module lifetime enforcement.
- [x] Specify initialization/shutdown states, error propagation, partial-service
  cleanup, play callback ordering/rollback, and Level notification ordering.
- [x] Specify Tick admission for empty, stopped, paused, stepped, editor, and
  preview Worlds, including host update placement and mutation behavior.
- [x] Select asynchronous retirement mechanics, checking the active
  [Async Task Framework Refactor](AsyncTaskFrameworkRefactor.md) for API overlap;
  record any actual implementation dependency before proceeding.

Completion: the above decisions are recorded in this plan with concrete source
touchpoints and no unresolved ordering or ownership choice for Stage 1.

### Stage 0 contract handoff

- `Engine.cpp::Init` must select the initial World type through a virtual host
  policy before initializing and publishing MainWorld. Editor currently changes
  the type after base Init; replace that ordering. `PrepareForShutdown` and
  `BeginDestroy` retire the World before viewport/scene release. `SetWorld` is
  also used to suspend/restore EditorWorld for PIE and must not retire it.
- `EditorEngine.cpp::StartPlaySession` initializes the PIE World before Level
  duplication and publication; every pre-publication failure shuts it down.
  `TeardownPlaySession` shuts down before its render fence and SetWorld(nullptr).
  The stopped EditorWorld receives host ticks even while PIE is active.
  `PreviewScene.cpp` initializes after its renderer endpoint is set, before
  attaching its Level; construction failure and destruction explicitly retire.
- Use native reference enumeration on DWorld for collection objects. Objects
  carry Transient and are absent from reflected serialization/duplication
  fields. Factories use concrete DClass native construction and qualified class
  names for ordering, without reflected-class discovery. A move-only descriptor
  registration belongs to its provider; collections snapshot registered values.
- Core's reflected pre-shutdown callback drains class defaults, not all escaped
  runtime instances (`ObjectLifecycle.cpp::ReleaseClassDefaultObjectsForModule`).
  Add an explicit module code lease acquired from the active load generation.
  Module shutdown rejects outstanding leases before entering Retiring and may
  be retried after release. Frozen descriptors and constructed subsystem objects
  retain leases; object leases last through physical destruction, including GC
  delay. An empty provider is allowed only for statically linked native fixtures.
- Collection states: Uninitialized, Initializing, Ready, ShuttingDown, Shutdown,
  Failed. Initialize returns a categorized error and message; failure is terminal.
  A failed service receives Deinitialize even if Initialize returns failure.
  Completed predecessors unwind in reverse order. Lookup exposes only completed
  initialization and remains available during Actor EndPlay/unregistration.
- World callback depth defers EndPlay/shutdown/Level replacement requested by a
  subsystem until the callback unwinds. Pending requests stop later callbacks.
  Native gameplay bootstrap completes before subsystem BeginPlay; then snapshot
  Actors (including service-spawned Actors) and dispatch Actor BeginPlay. Mark
  each service entered before its callback, pair reverse EndPlay only with those
  entered, and roll back aborted play through normal World EndPlay.
- Level attachment sets World membership then notifies services before component
  registration; detachment unregisters components while lookup is available,
  notifies services while the old Level remains identifiable, then clears
  membership. A Level callback may request a deferred transition, not recursively
  replace the active Level. Render-scene replacement does not recreate services.
- Gameplay Tick requires play and the existing pause/single-step admission.
  Explicit Editor/Preview policy admits stopped and empty editor/preview Worlds.
  Snapshot enabled flags at World Tick entry; changes apply next World Tick.
  Within each existing phase dispatch services in initialization order before
  the Level registry; recheck shutdown, pending transition and play state after
  each callback. Services retain their Tick policy independently of Levels.
- The active async refactor has only its transitive-wait repair implemented; its
  proposed task groups/completion tickets are unavailable. Use current cancellation
  tokens and detached shared completion state with a closed generation gate.
  Worker captures cannot access DObjects or raw World endpoints. Retirement
  closes the gate and cancels before Deinitialize; later GameThread publication
  must validate the gate. Provider work must additionally use existing module
  operation scopes. No wait on GameThread-dependent work and no dependency on
  future async-refactor stages is introduced.

### Stage 1: Implement Collection And Native Registration

Depends on Stage 0.

- [ ] Add subsystem base, descriptor registration, World-owned collection,
  explicit initialization states, and non-creating typed lookup.
- [ ] Implement World-type filtering, deterministic dependency resolution,
  diagnostics, rollback, reverse shutdown, and module lifetime enforcement.
- [ ] Add focused fixtures covering separate World instances, filtered and
  missing dependencies, duplicates, cycles, failure cleanup, GC retention,
  repeated shutdown, and provider retirement with live instances.

Completion: collection fixtures demonstrate deterministic ownership and failure
handling without changing existing gameplay or physics behavior.

### Stage 2: Integrate Hosts, Play Lifecycle, And Tick

Depends on Stage 1.

- [ ] Wire explicit initialization and shutdown through all identified hosts,
  including failed PIE startup and preview construction failure.
- [ ] Add play and Level notifications without weakening existing bootstrap
  rollback, component registration, or deferred transition semantics.
- [ ] Add World-owned optional Tick dispatch and admission rules; retain the
  existing Level Actor/Component scheduler contract.
- [ ] Verify callback reentrancy, shutdown/transition during dispatch, repeated
  play, Level replacement, empty Worlds, pause/single step, and late async
  completion after World retirement with focused fixtures.

Completion: every host publishes only initialized Worlds and retires services
before host resources; a service survives Level replacement and ticks according
to the selected policy without stale callbacks or duplicate play notifications.

### Stage 3: Migrate Collision Debug State And Qualify Isolation

Depends on Stage 2.

- [ ] Introduce a registered collision debug subsystem for enablement and
  last-hit state, preserving existing World facade results and defaults.
- [ ] Define and test last-hit clearing at Level detachment and shutdown so a
  surviving World does not expose collision state from its previous Level.
- [ ] Verify editor, PIE, and preview services coexist with distinct state;
  retiring PIE or preview leaves remaining Worlds intact.
- [ ] Run the relevant collision, World lifecycle, Tick, and editor/preview
  regression checks; add coverage only for new behavior or uncovered risks.
- [ ] Document implemented subsystem contracts under Runtime/World, update the
  relevant Level/Tick contracts and documentation routing, and record validation
  evidence and any limitations here before completing the plan.

Completion: a real service is supplied by registration without dedicated World
storage, existing public collision APIs remain compatible, required regressions
pass, and long-lived contracts live outside this plan.

## Validation And Handoff

Follow [agent build/run instructions](../Agents/BuildAndRun.md) before native
target operations and [agent testing instructions](../Agents/Testing.md) before
selecting native tests. Record exact targets and outcomes at each stage; no
native test result is claimed by this planning change. Follow
[documentation validation](../Agents/Documentation.md) for plan and contract
changes. Implementation commits update this plan and carry its exact Plan and
Stage trailers according to repository handoff rules.

## Deferred Session Scope

The existing native gameplay session is one World play lifetime and is cleared
by EndPlay. Cross-World progress, player-session, or matchmaking state needs a
separate persistent owner with an explicit lifetime across travel and PIE stop.
Create a separate bounded plan when a concrete consumer requires it; do not
silently extend `FNativeGameplaySession` lifetime as part of this work.

## Related Code

- [World API and state](../../Engine/Source/Runtime/Engine/Public/Engine/World.h)
- [World ownership and teardown](../../Engine/Source/Runtime/Engine/Private/Engine/WorldCore.cpp)
- [World play and Tick](../../Engine/Source/Runtime/Engine/Private/Engine/World.cpp)
- [World collision facade](../../Engine/Source/Runtime/Engine/Private/Engine/WorldCollision.cpp)
- [Runtime host](../../Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp)
- [Editor and PIE host](../../Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp)
- [Preview host](../../Engine/Source/Editor/DurinEd/Private/Preview/PreviewScene.cpp)
- [ModularFeature registration](../../Engine/Source/Runtime/Core/Public/Modules/ModularFeature.h)
- [Module manager](../../Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h)
- [Level lifecycle contract](../Runtime/World/LevelSystem.md)
- [Tick contract](../Runtime/World/TickScheduling.md)
- [Collision contract](../Runtime/Physics/Collision.md)
