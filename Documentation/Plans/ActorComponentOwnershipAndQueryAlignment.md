# Actor Component Ownership And Query Alignment Plan

Summary: Align Actor component ownership and query APIs with Unreal Engine while preserving Durin's authored serialization and transient native reconstruction contracts.

Last reviewed: 2026-08-20

Status: Completed
Completed: 2026-08-20

## Current Status

Implementation, validation, and the repository asset migration are complete.
`OwnedComponents` is now the single ordered live-membership authority, and
public queries use UE-compatible
`GetComponents()`, typed `GetComponents<T>()`, `FindComponentByClass<T>()`,
`AddOwnedComponent()`, and `RemoveOwnedComponent()` semantics without allocating
for membership or first-match reads.

The persistent authored member is `AuthoredComponents`; the transient all-live
member is `OwnedComponents`, matching the selected UE ownership meaning. The
two repository level packages containing the former persistent
`OwnedComponents` schema field were decoded, rewritten, canonically re-encoded,
and verified before the transitional member was removed.

Generated candidates remain unpublished until successful reconstruction commit.
Lifecycle paths use explicit frozen snapshots, ordinary readers use the live
view, and authored-first/generated-desired ordering, rollback, duplication,
package round trips, PIE, spline generation, and editor selection all passed.

Validation evidence on Win64 Debug DurinEditor after the final rename and asset
migration: `WorldTests` 105/105, `SplineTests` 40/40, `ViewportTests` 104/104,
`LevelAuthoringTests` 15/15, the full `all` build, and the authored-asset audit
with 29/29 compatible packages passed on 2026-08-20.

## Goal

Establish one allocation-free Actor component membership and query authority,
adopt UE-compatible names where the semantics match, and make snapshot creation
explicit at mutation-capable lifecycle boundaries.

The completed API and storage model must:

- establish one runtime collection for every currently live component owned by
  the Actor;
- answer membership and first-match queries without allocating;
- provide UE-style `GetComponents()` and typed `GetComponents<T>()` access;
- preserve strong typing in all templated query results;
- keep authored components persistent and generated components transient;
- preserve deterministic component order and callback mutation safety;
- publish or retire generated ownership only at a successful reconstruction
  commit boundary.

## Scope

- `AActor` component storage, ownership mutation, lookup, and enumeration APIs.
- Native-default, instance, and generated component creation and destruction.
- `FActorConstructionContext` commit and rollback membership publication.
- Actor load, duplication, reconstruction, garbage retention, play, visibility,
  registration, and destruction paths affected by component enumeration.
- Runtime and editor call-site migration from the allocating
  `GetOwnedComponents()` snapshot API.
- Focused ownership, construction, lifecycle, persistence, PIE, spline, and
  editor selection regressions.
- Lasting ownership and query contracts in the Runtime documentation.

## Non-Goals

- Reproducing Blueprint Simple Construction Script or User Construction Script
  machinery.
- Renaming native generated components to `BlueprintCreatedComponents` when
  Durin has no equivalent Blueprint creation authority.
- Introducing archetypes, default-subobject templates, or renaming
  `CreateDefaultComponent()` to UE's `CreateDefaultSubobject()` before those
  semantics exist.
- Changing component registration, BeginPlay, EndPlay, or destruction order.
- Exposing a mutation-safe live iterator across arbitrary user callbacks.
- Adding child-Actor recursive component queries before Durin has a matching
  child-Actor ownership feature.
- Adding a hash membership index without profiling evidence that the linear
  lookup of the normally small ordered collection is insufficient.
- Changing package format beyond the ordinary reflected field rename/migration
  required to preserve existing authored component data.

## Design Decisions and Invariants

### `OwnedComponents` Is The Runtime Membership Authority

`AActor::OwnedComponents` contains every live native, instance, and generated
component for which `Component->GetOwner() == Actor` and
`Actor->OwnsComponent(Component)` is true. Generic runtime and editor systems
must not concatenate category-specific collections to discover the Actor's
components.

The collection remains ordered rather than adopting UE's `TSet` representation.
Durin already defines forward BeginPlay, reverse EndPlay, authored-first query
order, and generated desired-set order. A `std::vector<TObjectPtr<...>>`
preserves those contracts and gives cache-friendly traversal; a separate hash
index is deferred until measured membership cost justifies its synchronization
burden.

The intended fields are equivalent to:

```cpp
// Every currently live component owned by this Actor; never serialized.
DPROPERTY(Transient)
std::vector<TObjectPtr<DActorComponent>> OwnedComponents;

// Persistent native and per-instance authored component state.
DPROPERTY()
std::vector<TObjectPtr<DActorComponent>> AuthoredComponents;

// Per-instance authored subset, matching UE's InstanceComponents concept.
DPROPERTY()
std::vector<TObjectPtr<DActorComponent>> InstanceComponents;

// Keyed transient native-construction subset and reconstruction index.
std::vector<FGeneratedComponentRecord> GeneratedComponents;
```

The set relationships are:

```text
InstanceComponents subset of AuthoredComponents
AuthoredComponents subset of OwnedComponents
GeneratedComponents subset of OwnedComponents
AuthoredComponents disjoint from GeneratedComponents
```

The transient reflected `OwnedComponents` collection is the strong runtime
reference authority. Persistent `AuthoredComponents` is the package and
object-graph duplication authority. `GeneratedComponents` remains a keyed secondary index;
it does not independently define whether a component is owned.

### Names Follow UE Only Where Semantics Match

Adopt or retain these UE-compatible names:

- `OwnedComponents` for the all-live membership collection;
- `InstanceComponents`, `AddInstanceComponent()`, and
  `RemoveInstanceComponent()` for the per-instance authored subset;
- `AddOwnedComponent()` and `RemoveOwnedComponent()` for centralized runtime
  membership mutation;
- `GetComponents()` for a direct read-only view of the live collection;
- `GetComponents<T>(OutComponents)` for typed collection queries;
- `FindComponentByClass<T>() const` for the first polymorphic match;
- `EComponentCreationMethod::Native` and
  `EComponentCreationMethod::Instance` where Durin semantics match UE.

Retain Durin-specific names where adopting a UE name would claim nonexistent
semantics:

- `GetAuthoredComponents()` and `AuthoredComponents` describe Durin package
  authority;
- `GeneratedComponents`, `FActorGeneratedComponentKey`, and
  `EComponentCreationMethod::Generated` describe native keyed reconstruction,
  not Blueprint construction;
- `CreateDefaultComponent()` remains until Durin implements UE-compatible
  default-subobject/archetype behavior.

Rename the custom exact-class query from `FindComponentByStaticClass<T>()` to
`FindComponentByExactClass<T>()`; this is intentionally a Durin extension and
must not be confused with UE's polymorphic `FindComponentByClass<T>()`.

### Live Views And Frozen Snapshots Are Different Contracts

`GetComponents()` returns a const reference to `OwnedComponents` and performs no
allocation. As in UE, adding, removing, transferring, or destroying a component
invalidates affected iterators and references. Callers may use this view only
while they do not invoke code capable of mutating Actor component membership.

`GetComponentsSnapshot()` returns a value copy of the live handles. Engine
lifecycle paths use it when virtual callbacks, component registration, editor
notifications, or destruction may mutate membership. A snapshot freezes the
entry set but does not guarantee continued ownership; each candidate is
revalidated immediately before callback publication.

The allocating `GetOwnedComponents()` compatibility surface is deprecated and
migrated out of repository call sites. It may forward to
`GetComponentsSnapshot()` for one compatibility window, but new code must state
whether it needs a live view or a frozen snapshot.

### Typed Queries Preserve Their Template Type

`FindComponentByClass<T>()` and `FindComponentByExactClass<T>()` return `T*` and
are `const` Actor queries. `GetComponents<T>(OutComponents)` writes `T*` values.
If retained as a convenience wrapper, `FindComponentsByClass<T>()` returns
`std::vector<T*>`, never `std::vector<DActorComponent*>`.

The UE-style output overload clears the supplied output before filling it and
preserves `OwnedComponents` order. The first-match APIs return the first live
entry in that same order. Null entries are not published by typed query APIs.

### Ownership Mutation Is Centralized And Idempotent

All creation paths route runtime membership through `AddOwnedComponent()`.
All destruction paths route cleanup through `RemoveOwnedComponent()`.
Category-specific storage is updated in the same owner-controlled operation or
through a narrowly paired helper; `DActorComponent::DestroyComponent()` must
not independently coordinate multiple Actor collections.

The mutation helpers enforce or diagnose these invariants:

- the component is non-null and structurally belongs to the Actor;
- a component appears in `OwnedComponents` at most once;
- `SetOwnedByActor(true)` is published with successful membership insertion;
- removal clears owned, authored, instance, and generated membership exactly
  once where applicable;
- the root component is cleared through the existing root ownership path;
- repeated or re-entrant destruction does not publish a partially inconsistent
  collection state.

### Reconstruction Publishes One Atomic Desired Set

New generated candidates may have `Outer == Actor` during staging, but they are
not present in `OwnedComponents` and do not report owned membership until
`FActorConstructionContext::Commit()` succeeds.

Commit validates the complete desired set before changing public membership.
It then publishes an ordered all-live collection consisting of authored entries
followed by the generated desired set, publishes the corresponding generated
key registry, and performs existing creation, registration, BeginPlay, and
reverse retirement routing. Failure before publication preserves the preceding
owned collection and generated registry; rollback destroys only unpublished
candidates.

Generated components remain transient object-graph output. Save, load,
duplication, PIE, and undo/redo paths must never treat the transient
`OwnedComponents` property as authored input. After load or duplication,
runtime owned membership is rebuilt from persistent `AuthoredComponents` before native
reconstruction publishes generated membership.

## Current Foundations and Gaps

Implemented foundations:

- reflected `TObjectPtr` arrays retain authored native-default and instance
  components;
- `InstanceComponents` already represents a persistent authored subset;
- generated components use transient object flags, keyed desired-set
  reconstruction, explicit retention, and atomic registry replacement;
- Actor component BeginPlay and EndPlay already use forward and reverse frozen
  snapshots with membership revalidation;
- `EComponentCreationMethod` distinguishes native-default, instance, and
  generated origins;
- focused World, spline, viewport, duplication, and package round-trip tests
  exercise all three component origins.

Verified gaps:

- the field named `OwnedComponents` excludes generated components;
- `GetOwnedComponents()` allocates and concatenates two collections for every
  call;
- `OwnsComponent()` and the three templated lookup APIs allocate a full
  snapshot for read-only traversal;
- nested lifecycle revalidation can repeatedly rebuild snapshots;
- `FindComponentsByClass<T>()` erases its template type in the return value;
- query templates are not `const` even though they do not mutate the Actor;
- generic engine/editor call sites cannot express whether they need a live view
  or a mutation-safe snapshot;
- generated staging sets `SetOwnedByActor(true)` before the candidate enters an
  Actor ownership collection.

## Implementation Stages

### Stage 0: Freeze Ownership, Ordering, And Compatibility Contracts

- [x] Record the implementation baseline and inventory every direct read or
  mutation of `OwnedComponents`, `InstanceComponents`, and
  `GeneratedComponents`.
- [x] Add or identify focused regressions for authored-first/generated-desired
  order, exact versus polymorphic lookup, null filtering, duplicate insertion,
  and typed multi-component results.
- [x] Add ownership-state assertions covering staged generated candidates,
  successful reconstruction, failed reconstruction rollback, retirement, and
  repeated destruction.
- [x] Confirm transient reflected properties are excluded from package save and
  graph duplication while remaining visible to garbage reference traversal.
- [x] Define the compatibility window for `GetOwnedComponents()` and
  `FindComponentByStaticClass<T>()`; default to one deprecation window unless
  repository policy requires immediate removal.

#### Acceptance Gate

- Existing authored, instance, and generated behavior has deterministic tests
  before storage changes.
- Serialization, duplication, GC retention, ordering, and invalidation semantics
  have one selected interpretation with no unresolved alternative.
- The complete affected call-site inventory is recorded in the stage handoff.

### Stage 1: Introduce The Unified Runtime Ownership Authority

Dependencies: Stage 0.

- [x] Retain the former persistent `OwnedComponents` member through the initial
  refactor, then migrate the complete repository asset corpus and rename it to
  `AuthoredComponents`.
- [x] Add the transient reflected all-live collection and finalize its name as
  `OwnedComponents` after asset migration.
- [x] Add UE-named `AddOwnedComponent()` and centralize
  `RemoveOwnedComponent()` so membership and owned flags change together.
- [x] Route native default and instance creation through authored-subset
  insertion plus `AddOwnedComponent()`.
- [x] Make component destruction remove owned, instance, authored, and generated
  membership idempotently through the Actor-owned path.
- [x] Rebuild runtime membership from authored state after load and duplication,
  before native reconstruction.
- [x] Move generated-component GC retention to the unified transient reflected
  collection when reflection coverage proves equivalent; otherwise retain only
  the minimum explicit collector logic required by the generated key index.
- [x] Add debug invariant validation for subset membership, uniqueness,
  structural ownership, creation method, and authored/generated disjointness.

#### Acceptance Gate

- `OwnedComponents` is the only authority used to answer whether an Actor owns
  a live component.
- Native, instance, and generated components enter and leave it exactly once.
- Existing authored assets and duplicated/PIE Actors restore authored identity
  without serializing generated output.
- Focused ownership, destruction, load, duplication, and GC regressions pass.

### Stage 2: Publish Generated Membership At Reconstruction Commit

Dependencies: Stage 1.

- [x] Keep newly acquired generated candidates outside `OwnedComponents` and
  clear of the owned flag during staging.
- [x] Validate the complete desired set before publishing owned membership or
  replacing the generated key registry.
- [x] Publish `OwnedComponents` in authored-first and generated-desired order at
  commit.
- [x] Preserve reused generated component identity while reordering it to the
  current desired-set order.
- [x] Register and begin newly committed candidates using the existing active
  World and Actor play-state rules.
- [x] Retire unclaimed generated components in reverse prior-generation order
  and remove their unified membership exactly once.
- [x] Ensure failed acquisition, attachment, validation, or commit leaves the
  prior owned collection and generated registry observable without partial new
  membership.

#### Acceptance Gate

- `OwnsComponent()` is false for an unpublished candidate and true immediately
  after successful commit publication.
- Reconstruction reuse, reorder, add, remove, nested request, failure, and
  rollback tests preserve identity and deterministic order.
- Package dirty suppression and authored-state authority remain unchanged.

### Stage 3: Add UE-Style Component Query APIs

Dependencies: Stages 1 and 2.

- [x] Add allocation-free `GetComponents() const` returning a const reference to
  the live `OwnedComponents` collection with UE-style invalidation guidance.
- [x] Add typed `GetComponents<T>(OutComponents) const` overloads required by
  current pointer and `TObjectPtr` consumers; avoid a larger allocator framework
  until a concrete caller requires it.
- [x] Add explicit `GetComponentsSnapshot() const` for frozen component handle
  batches.
- [x] Make `FindComponentByClass<T>()` const and allocation-free.
- [x] Rename the exact query to `FindComponentByExactClass<T>() const` and keep a
  temporary forwarding alias only if Stage 0 selects compatibility.
- [x] Change `FindComponentsByClass<T>()` to return `std::vector<T*>` and
  implement it as a convenience wrapper over typed `GetComponents()`.
- [x] Make `OwnsComponent()` query only the authoritative collection without
  allocating.
- [x] Rename `EComponentCreationMethod::NativeDefault` to UE-compatible
  `EComponentCreationMethod::Native`; retain `Generated` and `Instance`.

#### Acceptance Gate

- Membership and first-match queries allocate no result container.
- Typed output and convenience queries preserve `T*` at compile time.
- Exact-class and subclass behavior remain distinct and deterministic.
- New API names match UE where semantics match and explicitly document every
  retained Durin-specific term.

### Stage 4: Migrate Enumeration Call Sites By Mutation Boundary

Dependencies: Stage 3.

- [x] Replace pure read-only engine and editor enumeration with
  `GetComponents()` or typed `GetComponents<T>()`.
- [x] Keep `GetComponentsSnapshot()` in Actor/Component BeginPlay, EndPlay,
  registration, visibility, destruction, and editor notification paths that can
  invoke membership-mutating callbacks.
- [x] Preserve per-candidate owner, membership, pending-kill, registration,
  destruction, and play-state revalidation at lifecycle publication points.
- [x] Remove redundant snapshots from `MakeUniqueComponentName()`,
  `OwnsComponent()`, lookup templates, collision reads, picking reads, and other
  non-callback queries.
- [x] Migrate explicitly typed `std::vector<DActorComponent*>` consumers of
  `FindComponentsByClass<T>()` to `std::vector<T*>` or `auto`.
- [x] Remove repository uses of deprecated `GetOwnedComponents()` and
  `FindComponentByStaticClass<T>()`, then remove or retain forwarding wrappers
  according to the Stage 0 compatibility decision.
- [x] Audit every live-view loop to prove it does not retain an iterator or
  element reference across a virtual or externally supplied callback.

#### Acceptance Gate

- Repository call sites use live views only in mutation-free scopes and frozen
  snapshots at callback boundaries.
- No generic caller manually combines authored and generated collections.
- Forward BeginPlay, reverse EndPlay, visibility, registration, collision,
  picking, and editor selection behavior remains unchanged.
- Search-based audit finds no unintended use of the compatibility APIs.

### Stage 5: Validate And Publish Lasting Contracts

Dependencies: Stages 1 through 4.

- [x] Discover the current focused native-test targets through DurinDevTool and
  run the smallest selections covering World/Actor ownership, native
  construction, spline generation, viewport selection, persistence, and PIE.
- [x] Run the bounded Engine runtime domain after focused tests pass because the
  public `AActor` query surface is shared across runtime and editor consumers.
- [x] Run the required final build for the affected Runtime Engine and editor
  dependents under the selected Agent Build Profile.
- [x] Update Native Actor Construction and Level System documentation with the
  unified runtime membership, authored persistence subset, query invalidation,
  snapshot, and reconstruction publication contracts.
- [x] Record test/build evidence in `Current Status`, complete every acceptance
  gate, and run changed-document plus all-plan validation.

#### Acceptance Gate

- Focused and bounded integration tests pass with no generated serialization,
  ownership, ordering, lifecycle, or editor-selection regression.
- Required build validation passes for all consumers of the changed exported
  `AActor` interface.
- Lasting contracts are authoritative in Runtime documentation rather than only
  in this plan.
- All required checklist items and acceptance gates are complete.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Runtime ownership | Native, instance, and generated insertion/removal; duplicate rejection; `OwnsComponent()` before and after transitions |
| Query API | Exact and subclass first match, typed all matches, null filtering, const access, stable order |
| Allocation contract | No snapshot construction in membership or first-match paths; output allocation only for explicit collection queries |
| Reconstruction | Reuse, reorder, add/remove, nested request, failed commit, rollback, reverse retirement |
| Lifecycle mutation | Add/destroy/self-destroy during component BeginPlay and EndPlay; frozen entry set and revalidation |
| Persistence | Existing level load, save round trip, duplicate, PIE clone, generated exclusion, authored identity |
| GC and retirement | All live owned components retained; retired generated candidates collect; no stale registry reference |
| Runtime integration | World activation, component registration, collision enumeration, visibility, primary camera and play routing |
| Editor integration | component tree, selection targeting, picking, details, rename/delete restrictions for generated components |
| Documentation | changed-document validation, all-plan validation, updated Runtime ownership contracts |

Build and test execution must follow [Agent Build And Run](../Agents/BuildAndRun.md)
and [Agent Testing Workflow](../Agents/Testing.md). Test target names must be
discovered from the configured registry rather than inferred from source paths.

## Definition of Done

- `AActor::OwnedComponents` contains every live component owned by the
  Actor and is the sole generic membership authority; the public API and final
  internal name match UE ownership semantics.
- Authored serialization and generated reconstruction remain disjoint, with no
  generated component serialized or duplicated as source state.
- UE-compatible `GetComponents()`, typed `GetComponents<T>()`,
  `FindComponentByClass<T>()`, `AddOwnedComponent()`, `RemoveOwnedComponent()`,
  and `InstanceComponents` semantics are implemented.
- Durin-specific generated and authored names remain only where UE has no
  semantically equivalent concept.
- Pure queries do not allocate; multi-result typed queries return `T*`; callback
  paths retain explicit frozen snapshots and membership revalidation.
- Ordering, lifecycle, construction rollback, persistence, PIE, GC, runtime,
  and editor acceptance evidence passes.
- Lasting Runtime contracts are updated and this plan is marked complete.

## Deferred Follow-ups

- Add a UE-like inline-capacity component result container only after a measured
  workload shows material heap-allocation cost in explicit multi-result queries.
- Add an O(1) owned-component membership index only after profiling justifies
  the extra state and mutation validation.
- Add child-Actor recursive query flags only with a real child-Actor component
  ownership feature.
- Revisit default-subobject and UE `CreateDefaultSubobject()` naming under a
  dedicated archetype/class-default-object plan.
- Add property-level `LegacyNames` only when compatibility with external asset
  corpora requires field aliases; the repository corpus no longer needs one for
  this rename.
- Add runtime-class `GetComponentsByClass(DClass*)` only when scripting or
  reflection callers require the non-template surface.

## Related Documentation

- [Native Actor Construction](../Runtime/World/NativeConstruction.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)
- [Agent Build And Run](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Actor Component System](Archive/2026-07/ActorComponentSystem.md)
- [Actor Lifecycle Mutation Safety](Archive/2026-07/ActorLifecycleMutationSafety.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Actor.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/ActorConstruction.h`
- `Engine/Source/Runtime/Engine/Private/Engine/ActorConstruction.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/ActorComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/Level.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/World.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/WorldCore.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/WorldCollision.cpp`
- `Engine/Source/Editor/LevelEditor/`
- `Engine/Tests/Native/EngineTests/Private/World/WorldActorTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldComponentTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/WorldPlayTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SplineMeshComponentTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SplineTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/`
