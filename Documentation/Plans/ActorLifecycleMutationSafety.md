# Actor Lifecycle Mutation Safety Plan

Summary: Make world, actor, and component lifecycle dispatch safe when callbacks spawn or destroy runtime objects, then introduce a mutation-tolerant actor iteration abstraction.

Last reviewed: 2026-07-30

Status: Active
Completed:

## Current Status

Stage 3 is complete on Stage 2 baseline commit
`db55a9707bd2b1215b58cd2b594bd0d8d1d7a290`. The original planning baseline
remains `a91eaf5f97f20f36c881d22dd9e231eae6985b73`.

Stage 3 working set:

- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Private/Components/ActorComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/World.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/Level.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/Actor.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldLifecycleMutationTests.cpp`

Component BeginPlay, EndPlay, and destruction now use explicit engine-owned
state and dispatch entry points. Actor lifecycle and visibility routing own
handle snapshots, revalidate owner and membership, and defer Component
self-destruction until the active callback unwinds. World and Level
registration/unregistration loops likewise separate candidate collection from
callback publication. Component Tick remains unchanged by explicit scope.
All 52 `WorldTests` pass.

Stage 4 is next. It will introduce the frozen `FActorIterator` and range
surface without changing lifecycle ordering.

`DWorld::BeginPlay()` and `DWorld::EndPlay()` currently retain iterators into
`DLevel::Actors` while invoking virtual actor callbacks. `SpawnActor()` may
grow that vector and `DestroyActor()` erases from it, so either callback can
invalidate the active iterator. `DWorld::Tick()` already copies the actor
handles before dispatch and establishes the immediate safety pattern.

The same class of risk exists one level lower: `AActor` directly traverses
`OwnedComponents` while component callbacks can add or destroy components.
The current boolean play flags and direct virtual calls also permit recursive
BeginPlay, EndPlay, and Destroy routing that a container snapshot alone cannot
resolve.

The selected path is incremental:

1. close the verified World iterator invalidation with frozen entry snapshots;
2. centralize World and Actor play/destruction state transitions;
3. apply equivalent mutation safety to component dispatch;
4. introduce a Durin-owned `FActorIterator` whose state owns a candidate
   snapshot instead of borrowing mutable Level storage.

Component Tick mutation safety is intentionally deferred from this plan as of
2026-07-30. The current Actor-owned Tick path remains unchanged; a later
registration-based Tick scheduling plan will own its mutation and performance
contracts.

The archived
[Actor Component System](Archive/2026-07/ActorComponentSystem.md) plan remains
the ownership baseline. This plan preserves `DWorld` as the session owner,
`DLevel` as the Actor owner, `Outer` as structural containment, and garbage
marking as logical retirement.

## Goal

Guarantee deterministic, exactly-once Actor and Component lifecycle routing
when user callbacks mutate the active Level or an Actor's component set,
without retaining references or iterators into a container across a virtual
callback.

The final API must also provide a reusable Actor iterator that:

- never borrows a mutable `std::vector` iterator;
- filters retired, wrong-World, wrong-Level, and wrong-class entries before
  publication;
- has a documented termination and Spawn-observation policy;
- preserves the single-active-Level ownership model;
- can replace ad hoc forward Actor enumeration where its ordering semantics
  match the caller.

## Scope

- `DWorld` BeginPlay, Tick, EndPlay, level-switch, Spawn, and Destroy
  interactions.
- `DLevel` Actor membership, Spawn, Destroy, and destroy-all routing.
- `AActor` play and destruction state, plus Actor-owned component dispatch.
- `DActorComponent` play state and destruction re-entry where required by
  Actor-owned iteration safety.
- Frozen Actor snapshots and a reusable `FActorIterator`/Actor range surface.
- Focused native regressions for callback-driven Spawn, Destroy, level switch,
  component mutation, and repeated lifecycle calls.
- Lasting World/Level lifecycle documentation after behavior stabilizes.

## Non-Goals

- Multiple active Levels, level streaming, World Partition, networking, or
  replication.
- Reproducing every Unreal Engine `FActorIterator` flag or editor filter.
- Replacing the garbage collector, `TObjectPtr`, Outer hierarchy, or reflected
  ownership model.
- A general deferred-command framework for every runtime container.
- A Tick task graph, Tick prerequisites, parallel Actor Tick, or a permanent
  Tick registration scheduler.
- Making arbitrary Actor enumeration thread-safe; lifecycle and iterator use
  remain game-thread operations.
- Optimizing the existing per-frame Tick snapshot before profiling identifies
  it as a material cost.
- Changing Actor-owned Component Tick dispatch, including mutation safety,
  registration, ordering, or scheduling.
- Changing Actor or Component BeginPlay ordering beyond the rules selected in
  this plan.

## Design Decisions and Invariants

### Virtual Callbacks Are Mutation Boundaries

No iterator, reference, reverse iterator, span, or pointer to an element inside
`DLevel::Actors`, `AActor::OwnedComponents`, or `InstanceComponents` may remain
live across an in-scope virtual lifecycle callback. `DWorld::Tick()` retains
its existing Actor snapshot; Component Tick dispatch is deferred.

A dispatch pass owns independent `TObjectPtr` values. Every candidate is
revalidated immediately before callback publication. Garbage collection owns
physical destruction; a local handle is not permission to route a callback to
an object that has left the active owner or entered retirement.

### Lifecycle Dispatch Uses A Frozen Entry Set

World and Actor lifecycle batches process only candidates present when the
batch begins.

- A World BeginPlay snapshot is forward ordered.
- A World EndPlay snapshot is reverse ordered.
- An Actor component BeginPlay snapshot is forward ordered.
- An Actor component EndPlay snapshot is reverse ordered.
- Tick snapshots are forward ordered.

An Actor spawned while a World is beginning play is not added to the outer
World snapshot. The Spawn path dispatches BeginPlay to that Actor exactly once.
An Actor or Component destroyed before its turn is skipped.

This finite-set rule guarantees termination even when every callback spawns
another object. A future live enumeration mode may observe new Spawns only
through an explicit policy; it is not the lifecycle default.

### World State Is Observable Before User Code

Replace the single World play boolean with an internal state equivalent to:

```text
Stopped -> BeginningPlay -> Playing -> EndingPlay -> Stopped
```

The transition is published before invoking callbacks. `HasBegunPlay()` remains
a compatibility query and returns true only while beginning or playing, never
while ending.

- Spawn is allowed while stopped, beginning, or playing.
- Spawn while beginning or playing dispatches Actor BeginPlay exactly once.
- Spawn is rejected before allocation while the World is ending.
- A level switch requested during BeginPlay or EndPlay cannot recursively
  start the same World transition; the active batch stops after its captured
  Level ceases to be current.
- Repeated BeginPlay or EndPlay calls are idempotent at the World boundary.

The initial snapshot stage may implement the same externally visible ordering
with the existing boolean, but the state enum becomes authoritative before
re-entrant routing is considered complete.

### Actor Dispatch Owns Play And Destruction State

Virtual `AActor::BeginPlay()` and `EndPlay()` are user extension points, not
state-machine entry points. Add non-virtual engine-owned dispatch functions
equivalent to `DispatchBeginPlay()` and `RouteEndPlay()`.

Actor play state distinguishes at least:

```text
NotBegun -> Beginning -> Begun -> Ending -> NotBegun
```

Actor destruction separately distinguishes an accepted destroy request from
active destruction. The following rules apply:

- dispatch marks `Beginning` before calling virtual BeginPlay;
- EndPlay routes at most once for each completed or in-progress play lifetime;
- destroying the Actor that is currently beginning play records a deferred
  destroy request and completes it after BeginPlay unwinds;
- destroying an Actor already being destroyed succeeds without repeating
  notifications or container removal;
- destroying a not-yet-begun Actor removes it without synthesizing EndPlay;
- Actor membership remains available during its EndPlay callback, then Level
  removal and garbage marking complete in one owner-controlled path.

The dispatch wrapper, rather than derived classes calling `Super`, owns the
authoritative state transition. Existing base BeginPlay/EndPlay implementations
continue to route component callbacks, so derived Actor overrides retain the
requirement to call the base implementation when they want base component
routing.

### Component Dispatch Follows The Same Re-entry Rules

Component BeginPlay and EndPlay routing must not directly mutate an Actor-owned
vector being traversed. Actor dispatch copies component handles, then verifies
owner, membership, registration/play state, and retirement state before each
call.

Component play state distinguishes beginning, begun, and ending so a component
cannot recursively BeginPlay or EndPlay itself. Component destruction is
idempotent and removes the component from both owned collections exactly once.
Destroying the current component during BeginPlay defers final destructive
routing until that BeginPlay call returns.

A Component added while its Actor is ending play is registered and owned but
does not begin play in that ending lifetime.

### Membership Validation Is Correct Before It Is Optimized

The first World fix may call `DLevel::ContainsActor()` before each low-frequency
BeginPlay or EndPlay callback. This is deliberately accepted even though the
current linear implementation makes the pass O(N²): it closes the correctness
gap without adding permanent indexing state.

Lifecycle/destruction state introduced by later stages must make the common
eligibility test O(1) using captured Level identity, structural ownership, and
retirement/destruction state. A permanent Actor hash set is added only if a
measured workload requires it.

The cost of the entry snapshot itself is O(N) handles and one contiguous
allocation. It never copies Actor or Component objects.

### `FActorIterator` Owns Candidates

The first Durin iterator is a finite snapshot iterator:

- iterator state retains `TObjectPtr<AActor>` candidates copied at
  construction;
- no state retains a `std::vector` iterator or element reference;
- dereference publishes only a candidate that still belongs to the captured
  World and Level and is not being destroyed or garbage collected;
- optional class and active-Level filters are evaluated before publication;
- begin/end iterator construction and copying cannot leave callbacks or
  delegates targeting moved iterator storage;
- iteration is game-thread only;
- the initial candidate count is fixed, so Spawn during traversal is not
  observed.

This differs intentionally from UE 5.6/5.8, whose iterator owns an initial
candidate array and a second array populated by an `OnActorSpawned` handler.
Durin adds that explicit live-Spawn policy only when a concrete caller requires
it. Lifecycle dispatch continues to use the frozen policy even if that mode is
added.

`FActorIterator` is a correctness and API abstraction, not a Tick
optimization. Migrating `DWorld::Tick()` to it is allowed only if the resulting
allocation and filtering behavior is unchanged or measured.

## Current Foundations and Gaps

Implemented foundations:

- `DWorld` owns one active `DLevel` and already snapshots Actor handles for
  Tick.
- `DLevel` owns reflected Actor handles and centralizes Spawn, Destroy,
  destroy-all, and membership queries.
- `AActor` owns default and runtime-added component handles and routes base
  play/tick callbacks.
- `TObjectPtr` stores an 8-byte generation-checked object handle, so snapshots
  are shallow handle copies.
- logical destruction marks object hierarchies as garbage; collection owns
  physical deletion.
- focused World tests already cover ordinary play routing, Spawn, enumeration,
  ownership, PIE duplication, and level lifetime.

Verified gaps:

- World BeginPlay and EndPlay borrow mutable Actor-container iterators across
  Actor callbacks.
- World EndPlay leaves `bHasBegunPlay` true during callbacks, so a callback can
  Spawn and BeginPlay an Actor that is absent from the EndPlay batch.
- `DestroyActor()` calls virtual EndPlay before removing the Actor and has no
  explicit recursive-destruction guard.
- direct virtual Actor lifecycle entry points mix state transitions with user
  extension points.
- Actor BeginPlay and EndPlay borrow mutable component-container iterators
  across Component callbacks. Actor-owned Component Tick has the same pattern
  but is deferred from this plan.
- Component destruction removes itself from Actor arrays before completing all
  callbacks and has no explicit play/destruction re-entry state.
- there is no reusable Actor iterator or range with documented mutation
  semantics.

## Implementation Stages

### Stage 0: Freeze Mutation Semantics And Regressions

- [x] Record the baseline commit, initial working set, and symbols listed by
  this plan.
- [x] Add focused test Actor and Component types capable of spawning,
  destroying, switching Level, and recording callback order.
- [x] Add regressions for BeginPlay Spawn, destroy-before-turn, self-destroy,
  EndPlay Spawn, sibling destruction, repeated calls, and level replacement.
- [x] Add component regressions for add, destroy-before-turn, and self-destroy
  during BeginPlay and EndPlay.
- [x] Confirm failing tests identify iterator invalidation or lifecycle
  contract violations without depending on allocator-specific crashes.

#### Acceptance Gate

- Every selected mutation scenario has deterministic expected order and
  exactly-once counts.
- The current unsafe implementation fails the new contract through assertions
  or state/count mismatches, not by requiring undefined behavior to crash.
- No unresolved Spawn, Destroy, ordering, or re-entry decision remains.

### Stage 1: Make World Lifecycle Enumeration Snapshot-Safe

Dependencies: Stage 0.

- [x] Capture the current Level and a handle snapshot at entry to World
  BeginPlay and EndPlay.
- [x] Revalidate current-Level identity, Actor membership, pending-kill state,
  and Actor play state before every callback.
- [x] Preserve forward BeginPlay and reverse EndPlay order.
- [x] Publish the non-playing state before EndPlay callbacks so an EndPlay
  callback cannot create a newly playing Actor outside the batch.
- [x] Stop the captured batch if a callback replaces or clears the current
  Level.
- [x] Keep the existing Tick snapshot behavior unchanged in this stage.

#### Acceptance Gate

- Spawn or Destroy from World BeginPlay/EndPlay cannot invalidate iteration.
- Actors destroyed before their turn receive no later callback from the stale
  snapshot.
- Actors spawned during BeginPlay begin exactly once through the Spawn path.
- Actors spawned during EndPlay do not begin play.
- Ordinary lifecycle and existing World tests retain their order and results.

### Stage 2: Centralize World And Actor Lifecycle State

Dependencies: Stage 1.

- [x] Introduce explicit World play transition state and preserve the public
  compatibility query.
- [x] Add non-virtual Actor BeginPlay and EndPlay dispatch entry points with
  explicit Actor play state.
- [x] Route World, Level Spawn, Level Destroy, and Actor destruction through
  the dispatch entry points instead of calling virtual callbacks directly.
- [x] Add Actor destruction request/active-destruction guards.
- [x] Defer self-destruction requested during Actor BeginPlay until the
  BeginPlay callback unwinds.
- [x] Reject Spawn before allocation while the owning World is ending.
- [x] Make repeated and recursive BeginPlay, EndPlay, and Destroy calls
  deterministic and idempotent.
- [x] Replace Stage 1 linear membership checks with O(1) state checks where the
  new state proves equivalent membership and liveness.

#### Acceptance Gate

- No engine-owned path directly calls virtual Actor BeginPlay or EndPlay.
- World and Actor state transitions are published before user callbacks.
- BeginPlay self-destruction, EndPlay self-destruction, sibling destruction,
  repeated Destroy, and level switch complete without recursion or duplicate
  callbacks.
- Rejected Spawn during World EndPlay allocates and registers no Actor.

### Stage 3: Make Actor-Owned Component Dispatch Mutation-Safe

Dependencies: Stage 2.

- [x] Add engine-owned Component play/destruction dispatch state where direct
  virtual routing is re-entrant.
- [x] Snapshot owned component handles for Actor BeginPlay and EndPlay.
- [x] Revalidate owner, owned membership, registration, pending-kill, and play
  state before each Component callback.
- [x] Preserve forward BeginPlay and reverse EndPlay ordering.
- [x] Make `DestroyComponent()` remove owned and instance membership exactly
  once and tolerate repeated or recursive calls.
- [x] Defer Component self-destruction requested during its BeginPlay callback
  until the callback unwinds.
- [x] Audit registration, unregistration, visibility, and destruction loops
  for the same borrowed-container pattern; fix only paths that invoke
  mutation-capable callbacks.
- [x] Leave Actor-owned Component Tick dispatch unchanged and record it as a
  deferred registration-based scheduling task.

#### Acceptance Gate

- Adding or destroying a Component from Component BeginPlay or EndPlay cannot
  invalidate Actor iteration.
- A component destroyed before its turn is skipped.
- A component added while its Actor is already playing begins exactly once
  through the add/register path.
- Component state, owned membership, instance membership, and garbage state
  agree after every tested re-entrant path.

### Stage 4: Introduce Frozen `FActorIterator` And Actor Ranges

Dependencies: Stages 1 through 3.

- [ ] Add a game-thread Actor iterator state that owns generation-checked
  Actor handles captured from one World and its current Level.
- [ ] Add default filtering for retired/destroying Actors and wrong World or
  Level membership.
- [ ] Add optional reflected-class filtering without introducing C++ RTTI.
- [ ] Define end-iterator, copy/move, dereference, increment, and range-for
  behavior without registration callbacks tied to iterator object addresses.
- [ ] Add tests for empty World, empty Level, class filtering, Spawn during
  iteration, destroy-current, destroy-next, level replacement, copied
  iterators, and garbage collection after iterator destruction.
- [ ] Replace ad hoc forward Actor enumeration only where frozen-set and
  ordering semantics match exactly.
- [ ] Retain explicit reverse snapshot routing for EndPlay unless a reverse
  Actor range preserves the same contract without extra complexity.

#### Acceptance Gate

- The iterator never borrows mutable Level container state across caller code.
- Iteration terminates even when every visited Actor spawns another Actor.
- Destroyed, retired, wrong-Level, and wrong-class candidates are never
  published.
- Existing lifecycle ordering and exactly-once tests remain unchanged after
  any call-site migration.

### Stage 5: Validate And Publish Lasting Contracts

Dependencies: Stages 1 through 4.

- [ ] Run the complete focused World/Actor/Component native test target through
  DurinDevTool.
- [ ] Run relevant object-lifecycle and PIE isolation regressions.
- [ ] Run the required build validation for the final affected target set.
- [ ] Update Level and runtime lifecycle documentation with stable state,
  ordering, mutation, and iterator contracts.
- [ ] Record final validation evidence, update plan status, and run the
  all-plan validator.

#### Acceptance Gate

- Focused and integration tests pass without undefined-behavior-dependent
  expectations.
- Required builds pass under the selected Agent Build Profile.
- Lasting contracts are authoritative outside this plan.
- Every required checklist and acceptance gate is complete.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| World BeginPlay | Spawn, destroy-next, self-destroy, level clear/replace, repeated BeginPlay, forward order |
| World EndPlay | Spawn rejection/non-play, destroy-next, self-destroy, level clear/replace, repeated EndPlay, reverse order |
| Actor destruction | not-begun, beginning, begun, ending, repeated and recursive Destroy |
| Component lifecycle | add/destroy/self-destroy during BeginPlay and EndPlay; owned/instance membership |
| Actor iterator | empty state, filtering, frozen Spawn semantics, current/next destruction, copied state, level replacement |
| Ownership and GC | Level/Outer invariants, pending-kill filtering, no stale callback after garbage marking |
| PIE/runtime integration | play start, pause/single-step, play stop, transient World retirement |
| Complexity | one contiguous O(N) handle snapshot; no per-candidate allocation; no O(N²) membership check after Stage 2 |
| Documentation | changed-document checks and all-plan validation |

Build and test commands must follow
[Build And Run](../Development/Build/BuildAndRun.md) and
[Native C++ Tests](../Development/Build/NativeTests.md).

## Definition of Done

- World, Actor, and Component lifecycle code holds no mutable-container
  iterator or element reference across a virtual callback.
- World and Actor lifecycle states make re-entry and self-destruction
  deterministic.
- Spawn and Destroy behavior during every World play transition is documented
  and covered.
- Actor and Component BeginPlay/EndPlay route exactly once per play lifetime.
- The frozen `FActorIterator` publishes only currently eligible Actors and has
  deterministic termination under mutation.
- Focused, integration, build, and plan validation pass.
- Stable contracts are published in the owning Runtime documents.

## Deferred Follow-ups

- Add an explicit UE-style `IncludeSpawned` iterator policy with a World
  actor-added subscription only when a concrete caller needs live Spawn
  observation.
- Replace Actor-owned Component Tick traversal and the World Actor Tick
  snapshot with a registered Tick-function scheduler under a dedicated plan
  that owns mutation, ordering, dependency, and performance semantics.
- Add stable/tombstoned Level Actor slots only when editor deletion churn or
  large-Level compaction cost justifies the persistent complexity.
- Add multi-Level and streaming-aware iterator filters with the corresponding
  World ownership plan.
- Add Tick prerequisites, parallel dispatch, or mutation command buffers under
  a dedicated multithreading/gameplay scheduling plan.

## Related Documentation

- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Actor Component System](Archive/2026-07/ActorComponentSystem.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Engine/World.h`
- `Engine/Source/Runtime/Engine/Private/Engine/World.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Level.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Level.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Actor.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/ActorComponent.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldPlayTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldActorTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldComponentTests.cpp`
