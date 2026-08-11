# Registered Tick Scheduling Plan

Summary: Replace Actor-owned Component Tick traversal with stable registered Tick functions, deterministic Tick groups, and mutation-safe game-thread scheduling.

Last reviewed: 2026-08-12

Status: Completed
Completed: 2026-08-12

## Current Status

All six stages are complete. Actor and Component primary Tick functions now
register stable nodes with the active Level, World executes serial PrePhysics,
Physics, and PostPhysics groups, and `AActor::Tick()` no longer traverses
`OwnedComponents`. Stable-node frame state owns cancellation and
re-registration without a Component registration generation counter.

The complete `WorldTests` and `SkeletalAssetTests` targets pass, including the
`FWorldTickSchedulingTests.*` mutation, ordering, transition, and large-set
regressions. The default-target-granularity complete native test aggregate and
the full `all` build pass under the `windows-msvc-x64`
`Win64-Debug-DurinEditor` profile. Runtime Tick scheduling documentation,
changed-document validation, and all-plan validation pass.

This plan is the explicit follow-up deferred by the archived
[Actor Lifecycle Mutation Safety](Archive/2026-07/ActorLifecycleMutationSafety.md)
plan.

## Goal

Make Actor and Component Tick mutation-safe and deterministic by registering
stable Tick functions with the active Level and executing them through
World-owned group boundaries instead of traversing ownership containers.

The completed system must:

- keep Actor and Component ownership independent from Tick scheduling;
- allow callbacks to destroy, add, register, unregister, enable, or disable
  Tick targets without invalidating scheduler storage;
- execute each eligible Tick function at most once per admitted World frame;
- define observable same-frame mutation and ordering behavior;
- establish explicit pre-physics, physics, and post-physics barriers without
  introducing parallel execution;
- preserve World pause, single-step, level-transition, play, and destruction
  contracts.

## Scope

- Primary Tick functions embedded in `AActor` and `DActorComponent`.
- A Level-owned registration structure driven by `DWorld::Tick()`.
- Registration, unregistration, enable, disable, BeginPlay, EndPlay, Actor
  destruction, Component destruction, and Level replacement integration.
- Serial `PrePhysics`, `Physics`, and `PostPhysics` Tick groups.
- Deterministic registration order and same-group prerequisites required to
  preserve Actor-before-owned-Component behavior.
- Migration of `DPhysicsComponent` and `DSkeletalMeshComponent` to explicit
  groups.
- Focused mutation, ordering, pause/single-step, physics, animation, and
  lifetime tests.
- Publication of the lasting Tick contract in Runtime documentation.

## Non-Goals

- Worker-thread Tick execution, TaskGraph integration, parallel Tick groups,
  or thread-safe registration from arbitrary threads.
- Async physics, substepping, a new physics solver, or changing the existing
  `DPhysicsComponent` integration model.
- Tick intervals, time dilation, per-target pause policies, editor-only Tick,
  dedicated-server policies, batching, budgets, or priority lanes.
- Multiple active Levels, Level streaming, World Partition, networking, or
  replication.
- A general event scheduler or replacement for the Core CPU task system.
- Arbitrary public prerequisite graphs in the first implementation. The only
  required prerequisite is the compatibility edge from an owned Component to
  its enabled owning Actor when both use the same group.
- Changing BeginPlay or EndPlay snapshot ordering and exactly-once behavior.
- Replacing `TObjectPtr`, garbage collection, or the `Outer` ownership model.

## Design Decisions and Invariants

### Tick Group Is A Serial Phase Barrier

A Tick group is a named phase of one admitted World frame. Every eligible Tick
function in one group completes before the next group begins:

```text
input preparation
    -> PrePhysics
    -> Physics
    -> PostPhysics
    -> return from DWorld::Tick
```

The groups define data-visibility boundaries, not threads:

- `PrePhysics` produces gameplay intent and transforms consumed by physics.
- `Physics` performs the current physics-component integration.
- `PostPhysics` consumes final transforms for animation and other late
  gameplay updates.

Actor and ordinary Component primary Tick functions default to `PrePhysics`.
`DPhysicsComponent` uses `Physics`; `DSkeletalMeshComponent` uses
`PostPhysics`. All three groups execute serially on the game thread in this
plan. `DuringPhysics` is intentionally not used because Durin has no concurrent
physics interval for a Tick to overlap.

### Stable Tick Functions Own Scheduling Identity

`AActor` embeds one `FActorTickFunction`; `DActorComponent` embeds one
`FActorComponentTickFunction`. Both derive from a non-reflected or
reflection-safe `FTickFunction` scheduling base whose address remains stable
for the containing object's lifetime.

The Tick manager stores pointers to Tick functions, not pointers or iterators
into `DLevel::Actors` or `AActor::OwnedComponents`. A registered Tick function
may not be copied or moved. Garbage marking is logical retirement; physical
destruction must not occur while a World Tick queue can still reference the
embedded node.

`FActorTickFunction` invokes the Actor Tick extension point.
`FActorComponentTickFunction` invokes the Component Tick extension point after
the scheduler has admitted the node. `AActor::Tick()` no longer routes
Component Tick and its base implementation becomes empty.

### Level Owns Registration; World Owns Execution

The active `DLevel` owns an `FTickRegistry` containing registered stable nodes.
`DWorld::Tick()` captures the active Level, starts one Tick frame, runs the
three groups, and ends the frame. Before each group and after every callback,
World play state, current-Level identity, and pending Level transition remain
authoritative; losing admission stops the remaining groups.

Actor Tick registration occurs when a Level admits the Actor and is removed
when that ownership ends. Component Tick registration follows Component
registration and unregistration. Enabled and registered are separate states:
disabled Tick functions remain registered but are not queued.

All registration and execution operations in this plan are game-thread-only
and assert that contract in Debug builds.

### Current-Frame Mutation Uses A Node State Machine

The scheduler uses stable-node states and frame stamps rather than a
`RegistrationGeneration` comparison. A node distinguishes at least
unregistered, registered-idle, queued, executing, completed, and cancelled
states, plus a pending re-registration request when necessary.

Selected mutation semantics:

- enabling or registering before a group is sealed may admit the node to the
  earliest not-yet-started compatible group;
- enabling or registering after its selected group starts defers it to the
  next admitted World frame;
- disabling, unregistering, EndPlay, or destruction cancels a queued node that
  has not started;
- cancelling an executing node does not interrupt its callback, but prevents
  any second execution;
- re-registering or re-enabling a cancelled node cannot revive its stale queue
  slot; it is admitted only through the pending-registration path;
- adding a Component from another Tick never mutates the active Tick queue's
  backing storage;
- every node executes at most once per World frame, including disable/enable
  and unregister/register cycles.

This deliberately differs from UE's documented rule that unregistering does
not cancel work already scheduled in the current Tick frame. Durin cancellation
is selected because unregistration immediately uninitializes Components, so a
later callback into the old registration lifetime would violate the existing
Component lifecycle contract.

### Eligibility Is Centralized

The registry admits only a Tick function whose target and owner satisfy the
current lifecycle contract. Actor execution requires an active current Level,
live structural ownership, playing state, and an enabled registered Actor Tick
function. Component execution additionally requires a live owner, current
owned membership, Component registration, Component playing state, and no
pending destruction or garbage state.

Eligibility checks live in Tick-function setup/execution helpers and registry
state transitions. Call sites do not reproduce ad hoc combinations of
`HasBegunPlay()`, `IsRegistered()`, and pending-kill checks.

### Ordering Is Explicit And Deterministic

Group order is fixed. Within a group, unconstrained Tick functions use a
monotonic Level-local registration order so test and gameplay behavior is
repeatable.

To preserve the existing effective order, an owned Component in the same group
has an implicit prerequisite on its owning Actor when that Actor Tick function
is registered and enabled. A prerequisite in an earlier group is already
satisfied by the group barrier. A prerequisite in a later group is rejected in
Debug and ignored with a diagnostic in non-Debug builds rather than silently
moving the dependent node.

The first implementation supports only this engine-owned prerequisite edge.
General user prerequisites, cycle resolution, priority, and group promotion
remain deferred until a concrete caller requires them.

### Component Tick Does Not Depend On Actor Tick Enablement

An enabled registered Component ticks even when its owning Actor Tick function
is disabled. This matches the independent Tick-function model and fixes the
current accidental requirement that a Component arrange for its owner Actor to
be tick-enabled. `DPhysicsComponent` therefore stops enabling its owner merely
to gain Component dispatch, and enabled skeletal components become eligible
without an unrelated Actor Tick flag.

### Tick Queues Do Not Own Runtime Objects

The Level and reflected ownership graph retain Actors and Components. The Tick
registry stores non-owning stable-node pointers and clears all registrations
before the Level can be detached or physically destroyed. No Tick queue extends
an object's gameplay lifetime.

## Current Foundations and Gaps

Implemented foundations:

- World play, Actor play, Component play, and destruction state machines are
  published before virtual callbacks.
- World Tick already captures the active Level and an Actor-handle snapshot.
- Garbage marking separates logical retirement from physical destruction.
- `DLevel` centralizes Actor admission/removal; Components centralize register
  and unregister transitions.
- pause, single-step, deferred Level transition, native gameplay, PIE, physics,
  and skeletal animation tests already provide integration baselines.
- `RegistrationGeneration` exists for editor picking mutation identity but is
  not required to become the Tick scheduling identity.

Verified gaps:

- `AActor::Tick()` borrows `OwnedComponents` iterators across virtual
  Component callbacks.
- Actor membership and Component ownership containers double as scheduling
  lists.
- enabled Components are reached only through an enabled owning Actor.
- there is no named phase barrier between gameplay, physics integration, and
  post-physics consumers.
- Tick mutation ordering and same-frame registration behavior are unspecified.
- there is no stable scheduling node, per-frame queue state, or centralized
  eligibility policy.

## Implementation Stages

### Stage 0: Lock Scheduling Semantics And Regression Coverage

- [x] Add focused test Actor and Component types that record Actor and
  Component Tick order and can mutate Tick state from callbacks.
- [x] Add deterministic regressions for Component self-destruction, sibling
  destruction before its turn, addition during Tick, disable/enable,
  unregister/register, and owning-Actor destruction.
- [x] Add tests that distinguish registration before a future group is sealed
  from registration after the selected group starts.
- [x] Record the compatibility baseline for Pawn movement, physics integration,
  skeletal animation, pause, single-step, and pending Level transition.
- [x] Confirm tests detect state/order violations without requiring undefined
  behavior to crash.

#### Acceptance Gate

- Every same-frame mutation has one expected execution count and order.
- The unsafe implementation fails the new mutation contract through assertions
  or count/order mismatches.
- No registration, cancellation, group, or Actor/Component ordering decision
  remains unresolved.

### Stage 1: Introduce Stable Tick Functions And A Level Registry

Dependencies: Stage 0.

- [x] Add `FTickFunction`, `FActorTickFunction`, and
  `FActorComponentTickFunction` with stable-address and non-copyable contracts.
- [x] Add the Level-owned `FTickRegistry`, fixed group queues, monotonic
  registration order, current-frame state, and game-thread assertions.
- [x] Implement register, unregister, enable, disable, queue, cancel, execute,
  and end-frame transitions without registration generations.
- [x] Ensure queue growth and registry mutation never invalidate the queue slot
  currently being executed.
- [x] Add focused registry tests for at-most-once execution, stale-slot
  cancellation, re-registration, deterministic order, and frame reset.

#### Acceptance Gate

- A stable node can mutate its own or a sibling node's state without invalid
  memory access or duplicate execution.
- Cancelled queue slots cannot be revived by re-registration.
- The registry retains no Actor or Component and contains no ownership-container
  iterator.

### Stage 2: Migrate Actor And Component Tick Lifecycle

Dependencies: Stage 1.

- [x] Embed and configure primary Tick functions in `AActor` and
  `DActorComponent`.
- [x] Route Actor admission/removal and Component registration/unregistration
  through the active Level registry.
- [x] Route Tick enable setters through the primary Tick-function state instead
  of standalone booleans.
- [x] Make Actor and Component BeginPlay/EndPlay/destruction transitions update
  Tick eligibility before user callbacks can observe stale admission.
- [x] Replace the World Actor snapshot Tick loop with registry execution.
- [x] Remove Component traversal from `AActor::Tick()` and make the base Actor
  Tick implementation empty.
- [x] Remove the `DPhysicsComponent` workaround that enables its owning Actor.

#### Acceptance Gate

- No Actor or Component Tick path traverses `DLevel::Actors` or
  `AActor::OwnedComponents` as a scheduling list.
- Tick-time Component addition, removal, self-destruction, sibling destruction,
  and owner destruction satisfy the Stage 0 contract.
- Component Tick enablement is independent of Actor Tick enablement.
- BeginPlay, EndPlay, unregister, Level replacement, and garbage retirement
  leave no registered node targeting an ineligible object.

### Stage 3: Establish Tick Groups And Compatibility Ordering

Dependencies: Stage 2.

- [x] Add the public `ETickingGroup` values `PrePhysics`, `Physics`, and
  `PostPhysics` and fixed serial World barriers.
- [x] Default Actor and ordinary Component Tick functions to `PrePhysics`.
- [x] Assign `DPhysicsComponent` to `Physics` and
  `DSkeletalMeshComponent` to `PostPhysics`.
- [x] Implement the engine-owned owner-Actor prerequisite for same-group
  Components and deterministic registration-order fallback.
- [x] Reject impossible later-group prerequisites with actionable diagnostics.
- [x] Stop all remaining groups when World play admission, captured-Level
  identity, or pending-transition state changes.
- [x] Add group-order, visibility, owner-before-Component, future-group
  registration, and Level-transition regressions.

#### Acceptance Gate

- All `PrePhysics` work completes before Physics, and all Physics work completes
  before `PostPhysics`.
- Pawn/gameplay transforms are visible to Physics; final physics transforms are
  visible to post-physics animation consumers in the same frame.
- Same-group owner-before-Component compatibility is deterministic.
- A callback cannot cause a Tick to execute in a group that has already begun.

### Stage 4: Validate Runtime Integration And Performance

Dependencies: Stages 1 through 3.

- [x] Run the complete focused World/Actor/Component native test target through
  DurinDevTool.
- [x] Run relevant physics, skeletal animation, native gameplay lifecycle, PIE,
  pause, single-step, and Level-transition regressions.
- [x] Measure registry traversal and per-frame queue construction against a
  representative Actor/Component population; record counts and allocation
  behavior without making an unsupported optimization claim.
- [x] Confirm registration order does not depend on hash iteration or allocator
  addresses.
- [x] Run the required affected-target build validation under the selected
  Agent Build Profile.

#### Acceptance Gate

- Focused and integration tests pass with deterministic execution counts and
  order.
- The scheduler performs one bounded pass over registered Tick nodes plus
  executed dependency edges per frame and has no per-callback container copy.
- No Tick queue retains logically retired objects across frame completion.
- Required builds pass.

### Stage 5: Publish The Lasting Tick Contract

Dependencies: Stage 4.

- [x] Update Runtime lifecycle documentation with Tick registration, group,
  mutation, cancellation, ordering, and lifetime contracts.
- [x] Add a focused Runtime Tick-scheduling contract document if the resulting
  API surface no longer fits concisely in Runtime lifecycle documentation.
- [x] Update direct links from the archived lifecycle plan where needed.
- [x] Record validation evidence, update plan lifecycle metadata, and run the
  all-plan validator.

#### Acceptance Gate

- Implemented behavior is authoritative in Runtime documentation rather than
  only in this plan.
- Every required checklist and acceptance gate is complete.
- Plan and changed-document validation pass.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Iterator safety | self-destroy, destroy-next, add-next, forced registry/queue growth |
| State mutation | enable, disable, unregister, re-register, repeated transitions, at-most-once execution |
| Lifetime | owner destruction, Component destruction, EndPlay, Level replacement, garbage collection after frame completion |
| Group order | PrePhysics before Physics before PostPhysics; future-group admission; passed-group deferral |
| Compatibility order | enabled owner before same-group owned Components; independent Component Tick when owner Tick is disabled |
| Gameplay | Pawn intent consumption, pause, single-step, restart, pending Level transition |
| Physics and animation | physics integration and toggle; skeletal animation advancement after Physics |
| Determinism | stable registration-order fallback; no pointer/hash iteration ordering |
| Complexity | bounded registered-node walk; no ownership-vector snapshot per Actor Tick; recorded allocation behavior |
| Documentation | Runtime contract update, changed-document checks, all-plan validation |

Build and test commands must follow
[Build And Run](../Development/Build/BuildAndRun.md) and
[Native C++ Tests](../Development/Build/NativeTests.md).

## Definition of Done

- Actor and Component Tick scheduling is independent of ownership-container
  traversal.
- Tick callbacks may mutate Actor and Component membership and Tick state
  without iterator invalidation, stale callbacks, or duplicate execution.
- Stable Tick-node state, not registration generations, owns current-frame
  scheduling identity and cancellation.
- Tick group ordering and same-frame registration behavior are documented and
  covered by deterministic tests.
- Component Tick enablement is independent from Actor Tick enablement.
- World pause, single-step, play, Level transition, physics, animation, PIE,
  and native gameplay behavior pass their affected regressions.
- Lasting contracts are published outside the plan and required validation
  passes.

## Deferred Follow-ups

- General public Tick prerequisites, cycle diagnostics, group promotion, and
  multiple Tick functions per gameplay object.
- Tick intervals, priority, batching, budgets, time dilation, pause policies,
  editor-only Tick, and dedicated-server policies.
- Worker-thread execution, parallel groups, async physics overlap, and Core
  task-system integration.
- Stable/tombstoned or intrusive registry storage only if profiling shows
  registration churn or compaction cost is material.
- Live multi-Level and streaming-aware Tick registries.

## Related Documentation

- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [CPU Task System](../Runtime/Core/TaskSystem.md)
- [Build And Run](../Development/Build/BuildAndRun.md)
- [Native C++ Tests](../Development/Build/NativeTests.md)
- [Actor Lifecycle Mutation Safety](Archive/2026-07/ActorLifecycleMutationSafety.md)
- [Actor Component System](Archive/2026-07/ActorComponentSystem.md)
- [UE Actor Ticking](https://dev.epicgames.com/documentation/en-us/unreal-engine/actor-ticking-in-unreal-engine)
- [UE `FTickFunction`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FTickFunction)
- [UE `FTickTaskManagerInterface`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FTickTaskManagerInterface)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Engine/World.h`
- `Engine/Source/Runtime/Engine/Private/Engine/World.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Level.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Level.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Actor.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/ActorComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/PhysicsComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/PhysicsComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/SkeletalMeshComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/SkeletalMeshComponent.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldLifecycleMutationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldPlayTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldComponentTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/NativeGameplayCoreTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Physics/PhysicsSceneTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAnimationTests.cpp`
