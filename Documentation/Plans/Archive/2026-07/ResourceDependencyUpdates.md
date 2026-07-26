# Resource Dependency Updates Plan

Summary: Forward material dependency queries, batched loaded-object scans, and registration-free render invalidation.

Last reviewed: 2026-07-26

## Current Status

Completed and archived on 2026-07-26. Deterministic material and static-mesh
scan diagnostics are available, the 50 material dependency, batching,
transaction, rendering, asset, static-mesh scan, and lifetime tests pass, the
complete `Win64-Debug-DurinEditor-Tests` profile builds, and its hidden-window
editor smoke test passes. Material, reflected-property editing, and Asset
Registry architecture now record the lasting forward-dependency,
registration-free invalidation contracts.

This plan replaces the unimplemented centralized object dependency index design
recorded on 2026-07-26. The revised direction follows Unreal Engine's material
model more closely: canonical objects store only forward references, material
classes answer domain-specific dependency questions by traversing those
references, and an Engine-owned update context discovers loaded dependents on
demand for each change batch.

The change avoids introducing a persistent semantic dependency graph into
`CoreDObject`. It also separates loaded runtime invalidation from future
Asset Registry queries over unloaded assets.

## Goal

Remove material and static-mesh reverse registration state while preserving
correct, immediate render invalidation.

Loaded material inheritance is determined from the canonical `Parent` chain.
Resource changes are collected by a game-thread update context, which scans one
stable loaded-object snapshot, computes the affected material closure, and
updates matching components without requiring providers to own consumer arrays.

Reflected Edit, Cancel, Undo, Redo, ordinary setters, loading, duplication, and
destruction must all work without `RegisteredParent`, `DependentInstances`,
`BoundStaticMesh`, or `BoundMaterials`.

## Scope

- Add material-domain dependency queries:
  - test whether one material interface depends directly or transitively on
    another;
  - enumerate loaded direct child material instances;
  - enumerate loaded dependent material interfaces when needed by tools and
    tests.
- Add an Engine-owned material update context that batches changed roots,
  computes affected loaded materials, merges dirty flags, advances versions
  once, and updates matching static-mesh component slots.
- Discover loaded objects from `GDObjectArray` snapshots rather than maintaining
  a reverse semantic index.
- Replace recursive material child notification with one closure computation per
  update batch.
- Replace material-to-component registration with component-side dependency
  tests during a batched loaded-component scan.
- Replace static-mesh-to-component registration with an on-demand loaded
  component scan when mesh render data changes.
- Remove the registered-value mirrors and provider consumer collections used by
  those relationships.
- Preserve reflected-property validation, transaction behavior, render dirty
  flags, component revisions, and render-command ordering.
- Add performance counters and focused tests that make scan cost and batching
  behavior explicit.
- Update Architecture documentation after implementation.

## Non-Goals

- Adding a persistent forward or reverse semantic dependency graph to
  `CoreDObject`.
- Adding generic relation kinds or automatic dependency publication for
  reflected properties.
- Treating every `TObjectPtr` or garbage-collector reference as a render
  dependency.
- Replacing GC reference traversal, package dependency tracking, or the Outer
  index.
- Querying direct child material assets that are not loaded.
- Extending the current package header or Asset Registry with searchable
  property tags in this plan.
- Loading assets merely to answer a loaded-object dependency query.
- Making material or static-mesh mutation callable from the render thread.
- Preserving reverse-query complexity proportional only to the number of
  dependents; the selected tradeoff is a bounded loaded-object scan.
- Generalizing the first update context to textures, actor attachment, arbitrary
  resources, or editor-only reference analysis.
- Copying Unreal Engine's implementation details or API names where they do not
  fit Durin's current render-data snapshot model.

## Design Decisions and Invariants

### Selected model

The persistent relationship remains:

```text
DMaterialInstance --Parent--> DMaterialInterface
```

No matching Parent-to-children collection is stored. A material instance answers:

```cpp
Instance->IsDependent(TestMaterial)
```

by walking its current canonical Parent chain. A loaded direct-child query scans
loaded material instances and selects those whose `GetParent()` equals the
requested Parent.

Resource propagation uses:

```text
canonical mutation
    -> add changed root to FMaterialUpdateContext
    -> snapshot loaded objects
    -> evaluate IsDependent() against changed roots
    -> merge affected material dirty flags
    -> scan loaded components once
    -> queue required render updates
```

There is no registration or reconciliation step when Parent, mesh assignment, or
material assignment changes.

### Module ownership

- Material dependency semantics and `FMaterialUpdateContext` belong to Runtime
  Engine.
- `CoreDObject` remains unchanged. Runtime Engine uses the existing
  `GDObjectArray.Snapshot()`, `FObjectHandle`, and `ResolveObjectHandle()` APIs.
- AssetCore keeps its existing package-level dependency registry. Package
  dependencies answer loading, unloading, move, and deletion questions; they do
  not identify which reflected property created a dependency.
- A future unloaded-child query belongs in AssetCore or an editor asset-query
  layer after searchable property metadata exists.

### Material dependency semantics

`DMaterialInterface::IsDependent(const DMaterialInterface* TestDependency)`
uses these rules:

- return false for null;
- return true when `this == TestDependency`;
- a base `DMaterial` has no other material dependency;
- a `DMaterialInstance` walks `Parent` until it finds `TestDependency` or reaches
  null;
- traversal uses an iterative recursion guard so corrupt loaded data cannot hang
  dependency queries even though setters and PostLoad reject Parent cycles.

`IsDirectChildOf(Parent)` is equivalent to `GetParent() == Parent` and is not a
second stored relationship.

Loaded query APIs return generation-safe handle snapshots:

```cpp
auto GetLoadedDirectMaterialChildren(
    const DMaterialInterface* Parent
) -> std::vector<FObjectHandle>;

auto GetLoadedMaterialDependents(
    const DMaterialInterface* Dependency
) -> std::vector<FObjectHandle>;
```

The direct query excludes the Parent itself. The transitive query includes the
dependency itself because `IsDependent(this)` is true; callers that need only
descendants filter identity explicitly.

### Loaded-object scanning

- Queries take an owned `GDObjectArray.Snapshot()` on the game thread and convert
  matching objects to handles before returning or dispatching callbacks.
- Handles are sorted by index and generation so update order does not depend on
  dense-array swap removal.
- Each handle is resolved immediately before use. Null, garbage,
  pending-destruction, and wrong-class objects are skipped.
- Domain callbacks do not run while iterating the live `GDObjectArray` storage.
- Garbage collection must not run reentrantly inside an update-context flush.
  Existing synchronous game-thread mutation and render-command paths satisfy
  this constraint; the implementation adds an assertion around flush reentry.

### Material update context

Runtime Engine adds an explicit game-thread `FMaterialUpdateContext`. It owns no
objects and retains only handles and dirty flags for the duration of one update
batch.

The required API is equivalent to:

```cpp
class FMaterialUpdateContext
{
public:
    auto AddMaterial(
        DMaterialInterface* Material,
        EMaterialRenderDirtyFlags DirtyFlags
    ) -> void;

    auto Flush() -> void;
};
```

The exact visibility and names may change to match repository conventions.

`AddMaterial()` is idempotent per material handle and ORs dirty flags. `Flush()`:

1. validates and deduplicates changed roots;
2. takes one loaded-object snapshot;
3. evaluates every loaded material interface against the changed roots;
4. computes one merged dirty mask per affected material;
5. adds `ParentChain` when a material is affected through an ancestor;
6. advances each affected material's render-state version exactly once;
7. scans loaded `DStaticMeshComponent` instances once;
8. emits updates for every current slot whose resolved material is in the
   affected set; and
9. clears the context.

Parameter-value flags propagate through the Parent chain. Multiple changed
ancestors in one batch merge their flags before versions or components update.

`Flush()` is explicit rather than destructor-driven so validation can observe
completion and shutdown cannot silently discard work. Calling it twice without
new roots is a no-op.

### Ordinary mutation and batching

`DMaterialInterface::MarkRenderDataDirty()` remains the common object-facing
entry. Its initial implementation creates a local context, adds `this`, and
flushes immediately, preserving current synchronous visibility.

An internal overload accepts an existing context so multi-property imports,
future compiled-material edits, or other bulk operations can batch roots and
perform one scan. The first migration does not change public setter signatures
unless a current caller already owns a natural batch boundary.

The update context computes the complete affected closure itself. It does not
recursively call `OnParentRenderDataDirty()`, preventing one loaded-object scan
per inheritance level.

### Component dependency tests

A static-mesh component depends on a material for render data when any current,
non-orphan mesh slot resolves to a material interface that is in the affected
material set.

The update context evaluates current canonical state:

```text
component override
    else mesh slot default
    else no material dependency
```

Duplicate use of one material across slots produces one component scan but one
render update per affected slot. Orphan overrides remain serialized but do not
participate.

The context does not rely solely on pointer equality with changed roots. A slot
assigned to a child instance must update when any ancestor of that assigned
instance changes.

### Static-mesh updates

Static-mesh render-data changes are less frequent and do not share material
inheritance closure semantics. The first implementation uses an Engine helper
that:

1. snapshots loaded objects;
2. finds valid `DStaticMeshComponent` objects whose current `StaticMesh` equals
   the changed mesh;
3. sorts their handles deterministically;
4. asks each component to rebuild the appropriate render state; and
5. performs no provider registration.

When mesh slot definitions or defaults change, the component resolves current
materials directly on its next render-state build. There is no
`ReconcileMaterialBindings()` step because no binding mirror remains.

A unified resource update context is deferred until another resource type needs
cross-resource batching with a demonstrable shared contract.

### Transactions and lifecycle

Reflected transactions remain storage transactions:

```text
transaction restores Parent or assignment storage
    -> PostEditChangeProperty()
    -> mark the edited material/component dirty
    -> material context or component render-state path reads current storage
```

Because no reverse relationship is registered, the post hook needs neither the
old Parent nor a registered-value mirror. Interactive Edit, Cancel, Undo, and
Redo already use the same hook path.

Committed Edit remains notification-only after the final Interactive apply and
must not trigger a duplicate scan under the existing phase contract.

`PostLoad()` validates Parent chains and overrides but registers nothing.
`BeginDestroy()` has no material, mesh, or component dependency cleanup. Normal
queries filter garbage and pending-destruction objects, while strong reflected
references continue to be governed only by GC.

### Ordering, failures, and diagnostics

- All queries and context mutations are game-thread-only and follow the existing
  `CheckGameThread()`-when-initialized convention.
- Invalid or garbage roots are ignored with a debug diagnostic and create no
  update.
- Invalid candidate objects discovered during a scan are skipped.
- Parent-cycle validation fails before canonical reflected storage changes.
- The update context never changes canonical material or component assignments,
  so it has no rollback responsibility.
- A context reports counters for roots, scanned objects, tested materials,
  affected materials, scanned components, and updated slots.
- Tests assert one material and component scan per batch rather than elapsed
  time. Performance thresholds based on wall-clock time are deferred until
  representative projects exist.

## Current Foundations and Gaps

Current foundations:

- `DMaterialInstance::Parent` is already the canonical reflected inheritance
  relationship, and Parent cycles are rejected by setters and detached edit
  validation.
- Parameter resolution already walks the current Parent chain.
- `GDObjectArray.Snapshot()` provides an owned loaded-object snapshot, while
  `FObjectHandle` prevents slot reuse from resolving to a retired object.
- Reflected transactions already route Interactive, Cancel, Undo, and Redo
  through domain post hooks.
- Components can resolve every current material slot from canonical mesh,
  default, and override storage.
- Material render updates already carry material versions, component revisions,
  slot indices, and dirty flags.
- AssetCore already records package-level dependencies and a registry revision,
  but not property-specific searchable tags.

Current gaps:

- Material interfaces have no `IsDependent()` or loaded-child query.
- `DMaterialInterface` stores both `DependentInstances` and `BoundComponents`.
- `DMaterialInstance::RegisteredParent` mirrors the reverse relationship
  currently installed for reflected `Parent`.
- `DStaticMesh` stores `BoundComponents`.
- `DStaticMeshComponent` stores `BoundStaticMesh` and `BoundMaterials`.
- Current propagation recursively walks provider consumer lists rather than
  batching an affected closure.
- There is no instrumentation showing the number of objects scanned or updated
  by one resource change.
- The Asset Registry cannot yet distinguish a material Parent reference from
  another package reference, so it cannot implement an unloaded direct-child
  query without a metadata extension.

## Implementation Stages

### Stage 0: Material dependency semantics and loaded queries

- [x] Add iterative, cycle-guarded `DMaterialInterface::IsDependent()`.
- [x] Implement the base-material and material-instance dependency behavior from
  current canonical Parent storage.
- [x] Add loaded direct-child and transitive-dependent query helpers using one
  `GDObjectArray.Snapshot()` and generation-safe result handles.
- [x] Sort result handles by index and generation.
- [x] Exclude invalid, garbage, pending-destruction, and wrong-class candidates.
- [x] Add Engine tests for self, direct, transitive, unrelated, null, corrupt
  cycle guard, direct-child exclusion, deterministic ordering, and stale handle
  filtering.

#### Acceptance Gate

- Loaded material hierarchy questions are answered from `Parent` alone with no
  reverse registration state.
- Direct-child and transitive-dependent semantics are separately named and
  covered by tests.

### Stage 1: Batched material update context

- [x] Add `FMaterialUpdateContext` in Runtime Engine.
- [x] Implement root deduplication, dirty-flag merging, one loaded-object
  snapshot, and one affected-material closure computation per flush.
- [x] Mark indirect descendants with `ParentChain`.
- [x] Advance each affected material version exactly once per flush.
- [x] Scan loaded static-mesh components once and identify affected current
  material slots from canonical state.
- [x] Preserve per-slot `FMaterialRenderUpdate` emission, component revision
  ordering, and stale render-command rejection.
- [x] Add explicit flush reentry protection and update diagnostics counters.
- [x] Route `MarkRenderDataDirty()` through a local immediate context while
  supporting an internal existing-context batch path.
- [x] Add tests for multiple roots, shared descendants, overlapping dirty flags,
  duplicate slot assignments, one scan per batch, and no-op repeated Flush.

#### Acceptance Gate

- One material change batch updates every affected loaded material and component
  slot exactly once without traversing a stored reverse list.
- Context counters prove that inheritance depth and root count do not cause
  repeated global component scans within one flush.

### Stage 2: Remove material reverse registration

- [x] Remove `DMaterialInterface::DependentInstances`.
- [x] Remove `DMaterialInstance::RegisteredParent`.
- [x] Remove Add/RemoveDependentInstance, ReconcileParentDependency, and
  OnParentRenderDataDirty.
- [x] Remove material Parent registration from setters, PostLoad, post-edit
  hooks, and BeginDestroy.
- [x] Preserve Parent type checks, cycle rejection, override orphan behavior,
  package dirtying, and render invalidation.
- [x] Verify Parent changes through setter, PostLoad, Interactive Edit, Commit,
  Cancel, Undo, Redo, duplication, package unload, and GC.
- [x] Add tests proving transaction restoration updates rendering from current
  Parent storage without retaining the previous Parent.

#### Acceptance Gate

- Material inheritance and render propagation pass with only the reflected
  Parent relationship stored on instances.
- No material object owns a child-instance list or registered Parent mirror.

### Stage 3: Remove component binding registration

- [x] Replace material `BoundComponents` traversal with the material update
  context's loaded-component scan.
- [x] Remove material Add/RemoveBoundComponent methods.
- [x] Remove `DStaticMeshComponent::BoundMaterials`, BindMaterial,
  UnbindMaterial, and ReconcileMaterialBindings.
- [x] Replace static-mesh `BoundComponents` with the on-demand loaded-component
  scan helper.
- [x] Remove static-mesh Add/RemoveBoundComponent methods.
- [x] Remove `DStaticMeshComponent::BoundStaticMesh` and
  ReconcileStaticMeshBinding.
- [x] Remove dependency unregister work from component, material, and mesh
  BeginDestroy paths.
- [x] Preserve mesh replacement, slot default changes, override/reset/orphan
  behavior, reimport invalidation, and scene-proxy replacement rules.
- [x] Add tests for component assignment changes, material use across multiple
  slots, static-mesh replacement, default changes, override changes, orphan
  removal, garbage filtering, and object-handle slot reuse during later scans.

#### Acceptance Gate

- Current material and static-mesh render invalidation contains no provider
  consumer array and no component binding mirror.
- A provider change reaches exactly the valid components selected from current
  canonical state.

### Stage 4: Performance characterization and integrated validation

- [x] Add deterministic diagnostic access for the most recent material-context
  and static-mesh scan counters.
- [x] Create a focused test scene with multiple material inheritance depths,
  shared ancestors, components, and repeated slot assignments.
- [x] Verify that one batch takes one material/object snapshot and one component
  scan, independent of inheritance depth.
- [x] Compare update counts and render output before and after removing reverse
  registration.
- [x] Run focused Engine and reflected-property transaction tests through the
  root build driver.
- [x] Run the complete registered profile build using repository instructions.
- [x] Run the required hidden-window `DurinEditor` smoke test and exercise Parent
  editing, inherited parameters, assignments, Undo, and Redo.

#### Acceptance Gate

- Focused tests, full build, and hidden-window editor smoke test pass on the same
  build profile.
- Diagnostic counters demonstrate the selected scan cost and batching behavior
  without duplicate updates.

### Stage 5: Architecture handoff and plan archive

- [x] Update Material System with forward dependency semantics, loaded scan
  behavior, update-context batching, and the absence of reverse registration.
- [x] Update Reflected Property Editing to remove registered-value mirror
  language for materials and static-mesh components.
- [x] Document that Asset Registry package dependencies are not material
  hierarchy tags.
- [x] Remove any Architecture text that describes `RegisteredParent`,
  `DependentInstances`, BoundMaterials, BoundStaticMesh, or provider
  BoundComponents as current behavior.
- [x] Record completion evidence, archive this plan, and update active/archive
  indexes.

#### Acceptance Gate

- Architecture is the sole long-lived specification of implemented resource
  dependency updates, and every required implementation and validation gate is
  complete before archival.

## Validation Matrix

| Area | Automated validation | Integration validation |
| --- | --- | --- |
| Material hierarchy | Self/direct/transitive/null/unrelated/cycle-guard queries | Inspect hierarchy in Material Editor |
| Loaded queries | Direct child versus all dependent results, deterministic handles, garbage filtering | Query after load, unload, duplication, and Parent edits |
| Material batching | Root dedupe, flag merge, affected closure, version increments, one scan per batch | Change shared ancestor parameters across many visible instances |
| Reflected transactions | Interactive, Commit, Cancel, Undo, Redo without old-Parent storage | Parent picker and parameter controls |
| Material slots | Multiple slots, overrides, defaults, resets, and orphans | Observe in-place scene-proxy material updates |
| Static meshes | Equality-based component discovery, reimport, replacement, and defaults | Reimport or modify a mesh with multiple components |
| Lifetime | Garbage/pending-destroy filtering and handle generation reuse | Package unload and editor shutdown |
| Performance shape | Scan and update counters, no repeated scan by hierarchy depth | Representative multi-component test scene |
| Rendering | Existing revisions, dirty flags, and stale-command rejection | Hidden-window editor smoke after successful full build |

Build and test commands come from
`Documentation/Development/Build/BuildAndRun.md` and
`Documentation/Development/Build/NativeTests.md`; this plan does not duplicate them.

## Definition of Done

- Material dependency semantics are derived only from the canonical Parent
  chain.
- Loaded direct-child and transitive-dependent queries are explicit and tested.
- One Engine-owned material update context batches roots, computes the affected
  closure, scans components once, and preserves per-slot render updates.
- Static-mesh render changes discover loaded components from current mesh
  assignments without registration.
- `RegisteredParent`, `DependentInstances`, material and mesh
  `BoundComponents`, `BoundStaticMesh`, `BoundMaterials`, and their reconciliation
  methods are removed.
- Reflected transactions and runtime setters need no old relationship value.
- Package load/unload, duplication, garbage collection, and editor shutdown need
  no dependency unregister step.
- Scan cost is observable through deterministic counters.
- Focused tests, full build, and hidden-window editor smoke validation pass.
- Lasting behavior is documented in Architecture and the completed plan is
  archived.

## Deferred Follow-ups

- Searchable reflected property metadata in `.dasset` headers and `FAssetData`.
- An Asset Registry API equivalent to querying all unloaded direct material
  instance children by Parent path.
- A generic CoreDObject referencer finder for diagnostic object-reference
  analysis; it must remain distinct from render dependency semantics.
- A unified resource update context if textures, material functions, shader
  graphs, or other resources need the same batched loaded-consumer scan.
- Shared render proxies or renderer-owned material resources that could eliminate
  component scans for dynamic parameter-only changes.
- Incremental or cached loaded-object class views if measured scan cost becomes
  material at project scale.
- Concurrent material dependency queries if callers move off the game thread.

## Related Documentation

- `Documentation/Runtime/Assets/AssetPackages.md`
- `Documentation/Runtime/Core/GarbageCollection.md`
- `Documentation/Runtime/Rendering/MaterialSystem.md`
- `Documentation/Editor/Systems/ReflectedPropertyEditing.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectArray.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectHandle.h`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInterface.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInstance.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInstance.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/StaticMeshComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/ReflectedPropertyEditing.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/ReflectedPropertyEditing.cpp`
