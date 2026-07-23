# Actor Component System Plan

Last reviewed: 2026-07-24

## Current Status

Planning baseline. The reflected actor/component type hierarchy exists, but ownership, lifecycle routing, world spawning, transform hierarchy, scene registration, and serialization still require implementation. Resolve the ownership decisions recorded below before work proceeds beyond the early lifecycle stages.

## Goal

Grow the current reflected actor/component skeleton into a coherent runtime model whose construction, ownership, destruction, transforms, rendering registration, and persistence all follow the engine object system.

## Scope

- Actor and component construction and ownership.
- Lifecycle callbacks and destruction.
- World spawn and ownership.
- Scene-component transforms and primitive scene registration.
- Serialization and editor-facing reflected metadata.

## Non-Goals

- A parallel object or RTTI system outside `DObject`.
- Advanced gameplay frameworks, networking, or replication.
- Renderer features unrelated to component registration.

This document describes how to grow the current Actor-Component System from the existing `DObject` reflection foundation into a usable runtime model.

## Current State

Durin already has the important reflection pieces needed for an Actor-Component System:

- `DCLASS()` and `GENERATED_BODY()` generate `DClass` registration.
- `NewObject<T>(Outer, Name)` constructs reflected `DObject` instances through `FObjectInitializer`.
- `DObject::IsA()` and `Cast<T>()` use the reflected `DClass` hierarchy.
- `AActor`, `DActorComponent`, `DSceneComponent`, `DPrimitiveComponent`, `DMeshComponent`, `DStaticMeshComponent`, and `AStaticMeshActor` are reflected by the `Engine` module.

The ACS layer is still mostly a skeleton:

- `DWorld` is empty and does not own or tick actors.
- `AActor` stores component arrays, but ownership and lifecycle routing are incomplete.
- `DActorComponent` has registration flags and callbacks, but it does not derive its owner from `Outer`.
- `CreateDefaultComponent()` uses raw `new`, while existing reflected objects should be created through `NewObject`.
- Component destruction currently removes array references but does not have a unified object-system destroy path.
- Scene and primitive registration are placeholders.

## Design Direction

Build ACS around `DObject` instead of introducing a parallel object model.

The minimum stable model should be:

```text
DWorld
  owns AActor instances
    owns DActorComponent instances through Outer == Actor
      DSceneComponent provides transform hierarchy
      DPrimitiveComponent registers renderable state into IScene
```

Reflection should be used for construction, type queries, and later editor/serialization hooks. Raw RTTI and raw allocation should be treated as temporary compatibility only.

## Phase 1: Stabilize DObject Ownership And Construction

Goal: make all Actor/Component instances follow one construction and ownership path.

Implementation steps:

1. Add `DObject::GetOuter()` and related small accessors if needed by `Engine`.
2. Decide and document object destruction semantics for `NewObject` instances.
3. Route component retirement through `MarkAsGarbage()` and keep physical destruction private to GC.
4. Replace `AActor::CreateDefaultComponent()` raw `new T(...)` usage with `NewObject<T>(this, Name)`.
5. Update component find helpers to use `Cast<T>()` / `IsA()` instead of `dynamic_cast` and `typeid`.
6. Add focused tests for `NewObject<AActor>`, `NewObject<DActorComponent>`, `Outer`, `Name`, `StaticClass`, and `Cast`.

Important current files:

- `Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectGlobals.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/DObjectGlobals.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Actor.cpp`

Exit criteria:

- Actors and components are never created with raw `new` in normal engine code.
- Components can reliably discover their owning actor through object ownership.
- Destroying an actor has one clear path for component teardown.

## Phase 2: Complete Actor And Component Lifecycle

Goal: make component callbacks happen in a predictable order.

Recommended lifecycle:

```text
Create object
-> OnComponentCreated
-> RegisterComponent
-> OnRegister
-> InitializeComponent
-> runtime use
-> UninitializeComponent
-> OnUnregister
-> OnComponentDestroyed
-> object destruction
```

Implementation steps:

1. Set `DActorComponent::OwnerActorPrivate` from `ObjectInitializer.Outer` when the outer is an `AActor`.
2. Add `AActor::AddOwnedComponent(DActorComponent*)` and route all ownership changes through it.
3. Split default components and runtime instance components only if both concepts are needed now; otherwise keep one owned list until editor archetypes/CDOs exist.
4. Make `RegisterComponent()` idempotent and have it call `ExecuteRegisterEvents()`.
5. Make `UnregisterComponent()` call `ExecuteUnregisterEvents()` instead of directly calling `OnUnregister()`.
6. Ensure `DestroyComponent()` unregisters and uninitializes before removing ownership.
7. Add `AActor` lifecycle methods such as `RegisterAllComponents()`, `UnregisterAllComponents()`, `InitializeComponents()`, and `DestroyActor()`.
8. Define whether actor constructors are allowed to create default components immediately. For now, keep the existing `AStaticMeshActor` pattern, but make it use a helper that also records ownership.

Exit criteria:

- A component cannot be initialized unless it is registered and created.
- Repeated register/unregister calls are safe.
- Actor destruction reliably tears down all owned components once.

## Phase 3: Implement World Ownership And Spawn

Goal: introduce the runtime container that owns actors and drives their lifecycle.

Implementation steps:

1. Turn `DWorld` into a `DObject` or explicitly decide it remains a non-reflected engine service.
2. Add actor storage to `DWorld`, initially as `std::vector<AActor*>`.
3. Add `SpawnActor<T>(FName Name)` using `NewObject<T>(WorldOrLevelOuter, Name)`.
4. Add `DestroyActor(AActor*)` with deferred destruction if destruction can happen during iteration.
5. Add simple world lifecycle methods: `InitializeWorld()`, `BeginPlay()`, `Tick(float DeltaSeconds)`, `EndPlay()`, and `CleanupWorld()`.
6. Add `AActor::Tick(float DeltaSeconds)` and optional component ticking behind explicit flags.
7. Wire the selected `DWorld` into `DGameEngine` first, then editor world ownership later.

Exit criteria:

- A standalone game world can spawn and destroy `AStaticMeshActor`.
- World cleanup leaves no registered actor components behind.
- Tick order is documented and covered by tests or logging assertions.

## Phase 4: Add Scene Component Transform Hierarchy

Goal: make components spatially meaningful before integrating rendering deeply.

Implementation steps:

1. Expand `DSceneComponent` with relative transform, world transform, parent pointer, and child list.
2. Add `SetupAttachment(DSceneComponent* Parent)` and `AttachToComponent(...)`.
3. Make `AActor::SetRootComponent()` validate ownership and maintain root invariants.
4. Add transform dirty propagation from parent to children.
5. Add `UpdateComponentToWorld()` and call it during registration and transform changes.
6. Decide whether actor transform is a proxy to root component transform or a separate property. Prefer root component as the source of truth for now.

Exit criteria:

- Root and child scene components produce stable world transforms.
- Changing a parent transform updates child world transforms.
- Reassigning the root component does not orphan owned components.

## Phase 5: Register Primitive Components With The Scene

Goal: connect ACS to the renderer-facing scene abstraction.

Implementation steps:

1. Give `DWorld` an `IScene*` or scene-owning render bridge.
2. Let `DPrimitiveComponent::OnRegister()` call `Scene->AddPrimitive(this)`.
3. Let `DPrimitiveComponent::OnUnregister()` call `Scene->RemovePrimitive(this)`.
4. Let transform changes call `Scene->UpdatePrimitiveTransform(this)`.
5. Move render-proxy creation ownership into `DPrimitiveComponent` or a dedicated render component layer.
6. Keep renderer module dependencies one-way: `Engine` should talk to `IScene`, not concrete renderer internals.

Exit criteria:

- Registering a primitive component adds it to the active scene.
- Unregistering or destroying it removes it from the active scene.
- Moving it updates the renderer-facing transform path.

## Phase 6: Add Serialization And Editor-Facing Metadata

Goal: prepare ACS for project content and editor workflows.

Implementation steps:

1. Mark important Actor/Component fields with `DPROPERTY()` after the runtime property system supports the needed types.
2. Reflect `RootComponent`, owned component references, scene transforms, and mesh/material references only after object reference lifetime is stable.
3. Add object path or package concepts before saving object graphs.
4. Add archetype/CDO/default-subobject concepts only after raw ACS lifecycle is proven.
5. Build editor panels on reflection metadata rather than direct engine-private arrays.

Exit criteria:

- A simple actor with a static mesh component can be serialized and restored.
- Editor UI can inspect reflected component properties.
- Runtime defaults and instance overrides have separate, documented ownership.

## Suggested Implementation Order

Use small, verifiable slices:

1. `DObject` accessors and destruction policy.
2. Component owner binding from `Outer`.
3. Actor component creation helper using `NewObject`.
4. Component lifecycle idempotency.
5. Actor lifecycle helpers.
6. Minimal `DWorld::SpawnActor` / `DestroyActor`.
7. Scene component transform hierarchy.
8. Primitive scene registration.
9. Serialization/editor metadata.

This order keeps each step useful on its own and avoids building renderer/editor behavior on top of unstable ownership.

## Verification Plan

For each phase, prefer one native test plus one runtime smoke check.

Recommended tests:

- `CoreDObjectTests`: object construction, outer/name/class/cast behavior.
- `Engine` or a new engine test target: actor spawn, component ownership, lifecycle callback order, destroy cleanup.
- Rendering smoke: spawn `AStaticMeshActor`, register components, run `DurinEditor` or `DurinGame` long enough to validate scene registration.

Use the root `BuildTool` workflow and native-test guidance rather than direct CMake commands. For UI or rendering-visible changes, complete the required full build and hidden-window `DurinEditor` smoke because component registration bugs often appear only when the scene and viewport are live.

## Open Decisions

Resolve these before implementing beyond Phase 2:

- Should `DWorld` inherit from `DObject` now, or stay a plain engine class until packages/GC exist?
- Should component arrays store raw pointers initially, or introduce a lightweight object handle before GC?
- Should actor destruction be immediate, deferred until end-of-frame, or both?
- Are default components needed before CDO/archetype support, or should the engine initially treat all components as owned runtime instances?
- Where should `IScene` ownership live: `DWorld`, engine instance, viewport, or a dedicated scene subsystem?
