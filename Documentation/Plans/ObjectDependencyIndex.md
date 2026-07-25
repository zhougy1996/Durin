# Object Dependency Index Plan

Last reviewed: 2026-07-26

## Current Status

Design is selected and implementation has not started. The first consumer migration
will replace the material-parent reverse list and `RegisteredParent`; the same
facility will then replace the static-mesh and material component-binding mirrors.

## Goal

Provide one game-thread-owned, non-owning dependency index for loaded `DObject`
instances. Runtime systems explicitly publish typed semantic dependency edges and
can query either direction without storing consumer lists on providers or
registered-value mirrors on dependents.

The first completed vertical slice must allow a material or static mesh to find
exactly the loaded objects whose runtime state depends on it, while reflected
Edit, Cancel, Undo, Redo, setters, loading, and destruction all converge on the
same dependency-reconciliation path.

## Scope

- Add a process-local dependency index to `CoreDObject`.
- Represent an edge as a dependent object, a dependency object, and a named
  relation kind.
- Maintain forward and reverse indexes atomically from one published dependency
  set.
- Provide deterministic snapshot queries in both directions.
- Integrate edge removal with the central `DObject` destruction lifecycle.
- Migrate these current Engine-local relationships:
  - material instance to parent material interface;
  - static-mesh component to its assigned static mesh;
  - static-mesh component to each distinct resolved material.
- Preserve the existing material and static-mesh invalidation behavior.
- Preserve reflected-property transaction behavior without making
  `CoreDObject` depend on editor transaction types.
- Add focused CoreDObject and Engine tests and document the implemented runtime
  contract.

## Non-Goals

- Automatically treating every reflected `TObjectPtr` as a semantic dependency.
- Replacing garbage-collection reference tracing or changing object reachability.
- Replacing the `DObject` Outer index or making dependencies ownership edges.
- Indexing unloaded assets, package-on-disk references, source files, derived
  data, shader include graphs, or Cook dependencies.
- Loading an object merely because a dependency query names its asset.
- Providing cross-process persistence or serializing dependency edges.
- Building a general event bus or storing untyped invalidation callbacks in
  `CoreDObject`.
- Making the first version safe for concurrent mutation or render-thread access.
- Inferring dependency relation kinds from reflected property metadata.
- Migrating actor attachment, scene ownership, or unrelated cache relationships
  until they demonstrate the same reverse-query requirement.

## Design Decisions and Invariants

### Ownership and module boundary

- `CoreDObject` owns the index because handles, object registration, garbage
  filtering, and physical object removal already belong there.
- The index is process-local and contains only `FObjectHandle` values. It never
  roots objects and never participates in garbage-collector marking.
- Engine modules define named relation-kind constants for their semantic edges.
  Relation names are stable within the process but are not serialized.
- The index contains relationship identity only. Providers query dependents and
  invoke typed domain callbacks themselves, so `CoreDObject` does not depend on
  Engine classes or dirty-flag types.

### Edge direction and identity

For an edge:

```text
Dependent --RelationKind--> Dependency
```

the forward index answers which objects one dependent uses, and the reverse
index answers which loaded dependents use one dependency.

Edge identity is the tuple:

```text
(DependentHandle, DependencyHandle, RelationKind)
```

Publishing duplicates is idempotent. Different relation kinds between the same
two objects remain distinct.

Initial Engine relation kinds are:

```text
Engine.MaterialParent
Engine.MaterialRenderData
Engine.StaticMeshRenderData
```

Material instances publish one optional `Engine.MaterialParent` dependency.
Static-mesh components publish one optional `Engine.StaticMeshRenderData`
dependency and a set of distinct `Engine.MaterialRenderData` dependencies.

### Canonical values and derived indexes

- Reflected fields such as `DMaterialInstance::Parent`,
  `DStaticMeshComponent::StaticMesh`, and `MaterialOverrides` remain the
  persistent source of truth.
- The dependency index is derived runtime state and is never serialized.
- A dependent publishes the complete current dependency set for one relation
  kind. The index diffs it against its existing forward set and updates both
  directions as one operation.
- Complete-set publication is preferred over public caller-managed Add/Remove
  pairs. It allows the index to remember old edges and removes the need for
  `RegisteredParent`, `BoundStaticMesh`, and `BoundMaterials`.
- Domain validation remains next to canonical storage. For example, material
  parent-cycle rejection continues to inspect the proposed Parent chain rather
  than relying on load-order-sensitive index contents.

### Public API shape

The implementation should expose a small API equivalent to:

```cpp
auto SetObjectDependency(
    DObject* Dependent,
    FName RelationKind,
    DObject* Dependency
) -> bool;

auto SetObjectDependencies(
    DObject* Dependent,
    FName RelationKind,
    std::span<DObject* const> Dependencies
) -> bool;

auto ClearObjectDependencies(
    DObject* Dependent,
    FName RelationKind
) -> void;

auto GetObjectDependencies(
    const DObject* Dependent,
    FName RelationKind
) -> std::vector<FObjectHandle>;

auto GetObjectDependents(
    const DObject* Dependency,
    FName RelationKind
) -> std::vector<FObjectHandle>;
```

The exact names may change to match repository conventions, but the complete-set
mutation and handle-snapshot query semantics are required.

`SetObjectDependency(..., nullptr)` publishes an empty set for the relation.
The multi-value API removes nulls, deduplicates handles, rejects an unregistered
or garbage dependent, and rejects invalid or garbage non-null dependencies
without changing either index. An empty or `None` relation kind is invalid.

### Threading, ordering, and reentrancy

- All mutation and query APIs are game-thread-only. They follow the existing
  CoreDObject convention of calling `CheckGameThread()` when the game-thread ID
  has been initialized.
- Internal storage may use unordered containers, but returned snapshots are
  sorted by object-handle index and generation. Callback order is therefore
  deterministic and does not expose hash-table order.
- Queries return owned handle snapshots, not references or iterators into index
  storage. A callback may republish or clear dependencies without invalidating
  the current traversal.
- Callers resolve each handle immediately before use and skip objects that are
  null, garbage, pending destruction, or of the wrong domain type.
- No domain callback runs while the index is mutating internal maps.

### Lifecycle and failure behavior

- Object registration creates no implicit semantic edges.
- `PostLoad()` validates canonical data first and then publishes complete
  dependency sets before the loaded object becomes usable.
- Runtime setters update canonical storage and then call the same domain
  reconciliation helper used by reflected post-change hooks and `PostLoad()`.
- The central GC destruction path clears all incoming and outgoing edges before
  calling the object's `BeginDestroy()`. Cleanup must not depend on every
  subclass remembering a domain-specific unregister call.
- Physical slot removal asserts that no index entry remains for that exact
  handle. Generation reuse can therefore never inherit an old object's edges.
- Queries filter logically invalid objects even before physical removal.
- A failed dependency-set publication leaves both indexes unchanged. Debug
  validation checks forward/reverse agreement after mutations.

### Transactions and reflected editing

The editor transaction system remains unaware of the dependency index:

```text
transaction restores reflected canonical storage
    -> DObject::PostEditChangeProperty()
    -> domain reconciliation publishes the complete current dependency set
    -> dependency index atomically replaces old edges
    -> domain invalidation runs from reverse-query snapshots
```

Interactive Edit, Cancel, Undo, and Redo already pass through the same post hook.
The index's existing forward set supplies the old relationship, so neither the
transaction nor the edited object needs a second registered-value field.
Committed Edit remains notification-only after the final Interactive apply and
must not republish or invalidate a second time under the existing phase contract.

This integration also covers non-editor setters and loading. Dependency
correctness must not rely exclusively on an editor transaction being present.

### Invalidation propagation

- `DMaterialInterface::MarkRenderDataDirty()` queries:
  - `Engine.MaterialRenderData` dependents and notifies valid
    `DStaticMeshComponent` instances;
  - `Engine.MaterialParent` dependents and notifies valid
    `DMaterialInstance` instances.
- `DStaticMesh` render-data invalidation queries
  `Engine.StaticMeshRenderData` dependents and notifies valid
  `DStaticMeshComponent` instances.
- A component handling static-mesh changes recomputes and republishes its
  distinct resolved material dependency set before rebuilding render state.
- Recursive material-parent propagation preserves the existing dirty flags and
  cycle-rejection invariant. The index does not recursively dispatch by itself.

## Current Foundations and Gaps

Current foundations:

- `FObjectHandle` already supplies non-owning index-and-generation identity.
- `FDObjectArray` owns registration, resolution, garbage filtering, and physical
  slot removal.
- The Outer index demonstrates a central non-owning query accelerator whose
  canonical relationship lives on `DObject`.
- Reflected editing already validates detached proposals and routes Interactive,
  Cancel, Undo, and Redo through synchronous object post hooks.
- Material and static-mesh code already uses generation-safe handles in provider
  consumer lists.
- Existing reconciliation helpers already derive material bindings from
  canonical reflected component state.

Current gaps:

- `DMaterialInterface` and `DStaticMesh` each own ad hoc reverse consumer lists.
- `DMaterialInstance::RegisteredParent` duplicates the currently indexed Parent
  solely so the old reverse edge can be removed after generic property writes.
- `DStaticMeshComponent::BoundStaticMesh` and `BoundMaterials` are additional
  registered-value mirrors with the same synchronization burden.
- Reverse query behavior, ordering, lifecycle cleanup, and stale-edge handling
  are duplicated across Engine classes.
- Central object destruction does not currently remove semantic dependency
  edges because no central index exists.

## Implementation Stages

### Stage 0: Core index contract and focused tests

- [ ] Add the dependency relation and index API under `CoreDObject/Public/DObject`
  with implementation under `CoreDObject/Private/DObject`.
- [ ] Store matching forward and reverse indexes keyed by relation kind and
  generation-safe object handles.
- [ ] Implement atomic complete-set replacement, optional single-dependency
  replacement, relation clearing, and two-direction snapshot queries.
- [ ] Canonicalize nulls and duplicates according to the selected API contract.
- [ ] Sort query snapshots by handle index and generation.
- [ ] Add internal invariant validation for forward/reverse agreement.
- [ ] Enforce the selected game-thread contract without breaking bootstrap tests
  that run before the game-thread ID is initialized.
- [ ] Add `CoreDObjectTests` coverage for empty, single, multiple, duplicate, and
  multiple-kind edges; complete-set replacement; invalid input rollback;
  deterministic query order; and safe mutation after obtaining a snapshot.

#### Acceptance Gate

- Focused CoreDObject tests prove that one publication updates both indexes
  atomically and that relation kinds isolate otherwise identical object pairs.
- The index owns no `TObjectPtr`, raw owning pointer, editor type, or Engine
  domain callback.

### Stage 1: Object lifecycle integration

- [ ] Add central removal of all incoming and outgoing dependency edges when an
  object enters GC-controlled destruction.
- [ ] Assert before physical object-array removal that the retiring handle has
  no remaining edges.
- [ ] Make normal queries exclude garbage and pending-destruction objects.
- [ ] Define cleanup behavior for self-edges even though initial Engine
  consumers reject semantic cycles.
- [ ] Add tests for dependent destruction, dependency destruction, removal of
  mixed incoming/outgoing edges, delayed FinishDestroy, and object-slot
  generation reuse.
- [ ] Extend garbage-collection runtime documentation with the implemented
  non-owning dependency-index lifecycle invariant.

#### Acceptance Gate

- No edge survives logical destruction into slot reuse, and dependency edges do
  not keep otherwise unreachable objects alive.
- Destruction cleanup succeeds without any domain subclass unregistering itself.

### Stage 2: Material-parent migration

- [ ] Define the `Engine.MaterialParent` relation-kind constant in Runtime
  Engine.
- [ ] Change material-parent reconciliation to publish the current reflected
  `Parent` as the complete relation set.
- [ ] Replace `DMaterialInterface::DependentInstances` traversal with a reverse
  index query and typed handle resolution.
- [ ] Remove `AddDependentInstance()`, `RemoveDependentInstance()`,
  `RegisteredParent`, and their destruction cleanup.
- [ ] Preserve Parent type validation, cycle rejection, orphan overrides,
  dirty flags, and recursive parent-chain invalidation.
- [ ] Verify setter, PostLoad, Interactive Edit, Commit, Cancel, Undo, Redo,
  duplication, and destruction paths.
- [ ] Add tests proving that a reflected Parent replacement removes the old
  reverse result and adds the new result without a registered Parent mirror.

#### Acceptance Gate

- Material-parent propagation has identical observable render invalidation with
  no provider-owned child list and no dependent-owned registered Parent.
- Cancel, Undo, and Redo restore both the reflected Parent and reverse query
  result through the existing generic transaction path.

### Stage 3: Static-mesh component resource bindings

- [ ] Define `Engine.MaterialRenderData` and
  `Engine.StaticMeshRenderData` relation-kind constants.
- [ ] Publish the component's current static mesh as its complete static-mesh
  dependency set.
- [ ] Publish the distinct materials currently resolved from live mesh slots,
  component overrides, and mesh defaults as the component's complete material
  dependency set.
- [ ] Replace material and static-mesh provider consumer lists with reverse
  index queries.
- [ ] Remove `BoundStaticMesh`, `BoundMaterials`, Bind/Unbind helpers, and
  provider Add/RemoveBoundComponent methods.
- [ ] Preserve orphan override behavior: overrides absent from the current mesh
  publish no material dependency.
- [ ] Republish material dependencies before render-state invalidation whenever
  mesh slot definitions or defaults change.
- [ ] Add tests for duplicate material use across slots, mesh replacement,
  default-material changes, overrides, reset, orphan removal, transaction
  restoration, and component destruction.

#### Acceptance Gate

- All three current Engine reverse resource relationships use the central
  index, and no task-local registered-value mirror or provider consumer vector
  remains.
- A changed material or static mesh reaches exactly the currently dependent
  components once per invalidation event.

### Stage 4: Integrated validation and architecture handoff

- [ ] Run focused `CoreDObjectTests` and Engine material/static-mesh tests
  through the root build driver.
- [ ] Run the complete registered profile build using the repository build
  instructions.
- [ ] Run the hidden-window `DurinEditor` smoke test required for rendering
  changes and verify parent editing, parameter propagation, material assignment,
  Undo, and Redo.
- [ ] Add an Architecture document for the implemented dependency-index
  contract and update Material System and Reflected Property Editing to describe
  complete-set publication instead of registered-value mirrors.
- [ ] Remove superseded Architecture statements that describe
  `RegisteredParent`, bound-material mirrors, or provider-owned consumer lists.
- [ ] Record completion evidence, archive this plan, and update the active and
  archive indexes.

#### Acceptance Gate

- Focused tests, full build, and hidden-window editor smoke test pass on the
  same build profile.
- Architecture is the sole long-lived specification of the implemented index;
  this plan is archived with every required checklist and gate complete.

## Validation Matrix

| Area | Automated validation | Integration validation |
| --- | --- | --- |
| Core edge semantics | Forward/reverse symmetry, relation isolation, replacement, deduplication, deterministic snapshots | Inspect invariant checks under all mutation APIs |
| Failure atomicity | Invalid dependent, dependency, or relation leaves both directions unchanged | Exercise rejected material Parent proposals without graph mutation |
| Lifetime | Garbage collection, delayed finish, incoming/outgoing cleanup, generation reuse | Destroy loaded materials, meshes, and components during editor shutdown |
| GC ownership | Unreferenced objects with dependency edges are still collected | Verify no resource remains loaded solely because of an index edge |
| Material Parent | Setter, load, edit, cancel, Undo, Redo, cycle rejection, recursive dirty propagation | Change instance parents and inherited parameters in Material Editor |
| Material bindings | Slot deduplication, override/reset/orphan behavior, default changes | Edit component assignments and observe in-place render updates |
| Static-mesh bindings | Mesh replacement and mesh render-data invalidation | Reimport or modify a mesh and verify dependent component refresh |
| Rendering boundary | Existing material render-update tests and revision ordering | Hidden-window editor smoke test after a successful full build |

Build and test commands come from
`Documentation/Development/Build/BuildAndRun.md` and
`Documentation/Development/Build/NativeTests.md`; this plan does not duplicate them.

## Definition of Done

- `CoreDObject` exposes one documented, game-thread-only, non-owning object
  dependency index with symmetric forward/reverse queries.
- Complete-set publication is atomic, deterministic to query, reentrancy-safe,
  and failure-atomic.
- Central object destruction removes every incoming and outgoing edge before
  handle-slot reuse.
- Material parent, material render-data, and static-mesh render-data reverse
  relationships use the index.
- `RegisteredParent`, `DependentInstances`, `BoundStaticMesh`,
  `BoundMaterials`, and the corresponding manual provider registration APIs are
  removed.
- Reflected Edit, Cancel, Undo, and Redo restore dependency results without
  editor-specific code in `CoreDObject`.
- Setters, PostLoad, duplication, garbage collection, and shutdown preserve the
  same invariants outside transactions.
- Focused tests, full build, and the required hidden-window editor smoke test
  pass.
- Lasting contracts are moved into the owning domain and the completed plan is
  archived.

## Deferred Follow-ups

- An unloaded-asset dependency database for Content Browser reference queries,
  rename/move impact analysis, Cook, and package scheduling.
- Automatic extraction of selected semantic edges from reflected metadata after
  explicit relation and lifecycle semantics exist.
- Batched invalidation contexts that coalesce repeated provider changes and
  dispatch topologically.
- Concurrent read snapshots or a read-copy-update representation if dependency
  queries move off the game thread.
- Cycle-detection helpers for relation kinds whose canonical domain cannot
  validate its own graph.
- Diagnostics UI for inspecting loaded-object dependencies and dependents.
- Metrics for edge count, query fan-out, stale-handle filtering, and propagation
  cost.

## Related Documentation

- `Documentation/Runtime/Core/GarbageCollection.md`
- `Documentation/Runtime/Rendering/MaterialSystem.md`
- `Documentation/Editor/Systems/ReflectedPropertyEditing.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`

## Related Code

- `Engine/Source/Runtime/CoreDObject/Public/DObject/DObjectArray.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectHandle.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/ObjectLifecycle.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/DObjectArray.cpp`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/ObjectLifecycle.cpp`
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
