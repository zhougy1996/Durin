# Actor Component System Plan

Summary: Close the remaining reflection-model debt in the implemented actor/component runtime and archive the completed foundation.

Last reviewed: 2026-07-27

## Current Status

Active, near completion. The original planning baseline is obsolete: actor/component ownership, lifecycle routing, world and level integration, transform hierarchies, primitive scene synchronization, persistence, Play In Editor isolation, and reflected editor workflows are implemented and covered by focused native tests.

The remaining plan work is intentionally narrow. Core ACS type queries still use C++ RTTI in several actor, level, world, and component paths even though these objects participate in the `DObject` class system. Replace that compatibility debt with `Cast<T>()`, `IsA()`, and reflected exact-class comparison, validate the existing behavior, then archive this plan. New gameplay-framework features do not belong in this plan.

## Goal

Finish the actor/component foundation as one coherent `DObject`-based runtime model, remove the last parallel type-query path from its core APIs, and preserve the implemented runtime contracts in their owning documentation.

## Scope

- Reflected actor/component type queries in core ACS code.
- Actor and component ownership invariants based on `Outer` plus reflected strong references.
- Regression validation for lifecycle, world/level ownership, transforms, scene registration, persistence, and editor-facing component operations.
- Plan completion and archival after the final acceptance gates pass.

## Non-Goals

- Archetypes, class default objects, or a general default-subobject system.
- Networking, replication, gameplay abilities, or a broader gameplay framework.
- Multi-level streaming or multiple simultaneously active levels.
- Replacing the current engine-owned main scene with a world scene subsystem.
- Redesigning asset serialization, reflection, garbage collection, rendering, physics, or editor transactions.

## Design Decisions and Invariants

- `DWorld` is a reflected runtime/editor session container; `DLevel` is the persistent scene asset and actor owner.
- A level owns actors through reflected `TObjectPtr` storage and `Outer == Level`. An actor owns components through reflected `TObjectPtr` storage and `Outer == Actor`.
- Default components are constructed by actor classes through `CreateDefaultComponent()` and `NewObject`. Runtime-added components are additionally tracked in `InstanceComponents`.
- `MarkAsGarbage()` performs logical retirement; garbage collection owns physical destruction. Retiring an owned hierarchy follows the `Outer` index.
- Component lifecycle preserves created → registered → initialized → play → uninitialized → unregistered → destroyed ordering; repeated register, unregister, and pending-kill routing remains safe.
- An actor's root scene component is the source of its transform. Scene attachments are acyclic, preserve the selected attachment rule, and cannot cross active level boundaries.
- `DPrimitiveComponent` owns stable primitive scene identity. The active engine scene receives proxy, transform, material, visibility, and removal updates.
- Persistent level data stores authored ownership and relative state. Attachment children and world transforms are derived and rebuilt after load.
- Core ACS type queries use the reflected class hierarchy. C++ RTTI is not a second actor/component type system.

## Current Foundations and Gaps

Implemented foundations:

- `DObject` construction, `Outer` access, reflected references, deferred physical destruction, hierarchy retirement, object duplication, and object-graph serialization.
- Actor default and instance component ownership, unique naming, root-component validation, visibility, attachment, play, tick, and teardown.
- Component owner binding, registration/initialization routing, play/tick routing, pending-kill cleanup, and garbage marking.
- `DWorld` and `DLevel` ownership, spawn/destroy, primary-camera selection, level switching, play lifecycle, pause/single-step behavior, and transient PIE duplication.
- Relative/world transforms, attachment rules, dirty propagation, cycle and cross-level rejection, post-load hierarchy repair, and destruction-safe detachment.
- Primitive scene proxy registration, replacement, transform/material updates, visibility removal, and unregister removal.
- Reflected level persistence, editor instance-component operations, property editing, hierarchy notifications, and PIE apply isolation.

Remaining gaps:

- `AActor` component lookup templates use `typeid` and `dynamic_cast`.
- Actor/component owner queries and several `DLevel`, `DWorld`, and scene-component paths use `dynamic_cast` despite reflected classes being available.
- The plan has not yet recorded one final focused regression run against the implemented foundation.

## Implementation Stages

### Stage 0: Lock The Implemented Ownership Model

- [x] Select `DWorld` as the session container and `DLevel` as the persistent actor-owning asset.
- [x] Select reflected `TObjectPtr` fields for retention and `Outer` for structural containment and object paths.
- [x] Select garbage marking plus collection for destruction instead of direct deletion.
- [x] Separate constructor-created default components from editor/runtime instance components without introducing archetypes or CDOs.
- [x] Use the root scene component as actor transform authority.
- [x] Keep the active scene owned by the engine and synchronize primitives through stable scene IDs.
- [x] Persist authored level/component state and rebuild derived attachment state after load.

#### Acceptance Gate

- The selected contracts are implemented and described by the related runtime and editor documentation.

### Stage 1: Remove Core ACS C++ RTTI

Dependencies: Stage 0 and the existing `DClass` hierarchy.

- [ ] Replace exact component lookup via `typeid` with exact `DClass` comparison.
- [ ] Replace polymorphic component, actor, level, and world `dynamic_cast` queries in the core ACS path with `Cast<T>()` or `IsA()`.
- [ ] Preserve null handling, subclass matching, editor-only hierarchy notifications, primary-camera selection, and cross-world validation.
- [ ] Add or adjust focused tests only where the existing suite does not distinguish exact-class lookup from subclass lookup.

#### Acceptance Gate

- Core actor/component, level, and world implementation files contain no `dynamic_cast` or `typeid` type queries.
- Exact-class and subclass component lookup behavior remains distinct and covered.
- Existing world, component, lifecycle, persistence, and editor-facing tests pass.

### Stage 2: Validate And Archive

Dependencies: Stage 1.

- [ ] Run the focused `EngineTests` world/component test set through the repository `BuildTool`.
- [ ] Run the plan validator for active and archived plans.
- [ ] Record completion evidence in this section and `Current Status`.
- [ ] Confirm lasting behavior remains in the owning runtime/editor documents, then archive this file under the completion month.

#### Acceptance Gate

- Focused native tests and plan validation pass.
- No required implementation checklist remains open.
- The archived plan links to the authoritative long-lived contracts rather than competing with them.

## Validation Matrix

| Area | Evidence |
| --- | --- |
| Reflected type queries | Exact and subclass component lookup tests; runtime-class spawn tests |
| Ownership and destruction | `WorldLifetimeTests.cpp`, `WorldActorTests.cpp` |
| Lifecycle and ticking | `WorldPlayTests.cpp` |
| Transform hierarchy | `WorldComponentTests.cpp` |
| Scene synchronization | primitive/material/skybox component tests plus rendering smoke when rendering code changes |
| Persistence and duplication | level round-trip and PIE duplication tests in `WorldActorTests.cpp` and `WorldPlayTests.cpp` |
| Editor-facing properties | reflected property container, edit-session, and transaction tests |
| Plan integrity | `.\Documentation\DocTool.bat validate --scope all` |

Repository build and test commands must follow [Build And Run](../Development/Build/BuildAndRun.md) and [Native C++ Tests](../Development/Build/NativeTests.md).

## Definition of Done

- Core ACS uses the reflected class system for exact and polymorphic type queries.
- The focused ownership, lifecycle, transform, persistence, and editor-facing regression suite passes.
- Long-lived behavior is documented outside the plan in the owning runtime and editor domains.
- This plan is marked complete and moved to `Documentation/Plans/Archive/YYYY-MM/`.

## Deferred Follow-ups

- Introduce archetype/CDO/default-subobject semantics only when reusable authored actor classes require them.
- Add deferred actor destruction only when mutation during world iteration requires it.
- Add component tick prerequisites or scheduling only with a concrete gameplay or multithreading requirement.
- Treat sub-level streaming, networking, and replication as separate plans with their own ownership and lifecycle decisions.
- Revisit world-specific scene ownership only if multiple concurrently rendered worlds require it.

## Related Documentation

- [Reflection System](../Runtime/Core/ReflectionSystem.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Level System](../Runtime/World/LevelSystem.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Play In Editor Architecture](../Editor/Architecture/PlayInEditorArchitecture.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/Object.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectLifecycle.h`
- `Engine/Source/Runtime/Engine/Public/Engine/Actor.h`
- `Engine/Source/Runtime/Engine/Public/Engine/Level.h`
- `Engine/Source/Runtime/Engine/Public/Engine/World.h`
- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/SceneComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Tests/Native/EngineTests/Private/World/`
