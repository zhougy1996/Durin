# Garbage Collection

Summary: Define managed-object reachability, collection, rooting, and destruction contracts.

Modules: CoreDObject

Durin uses a synchronous, stop-the-world, non-moving mark-sweep collector for `DObject` instances. Collection runs on the game thread and does not scan the native stack. Object hierarchy and object lifetime are related in one direction only: a reachable child keeps its Outer chain alive, while a reachable Outer does not keep its children alive.

## Object Registry And Outer Index

`GDObjectArray` is the runtime object registry. It contains:

- stable index-and-generation slots used by object handles
- a dense array of live objects used for traversal
- a non-owning `Outer -> Direct Children` query index
- the current count of objects marked as garbage

`DObject::OuterPrivate` is the only source of truth for the hierarchy. Registration, `SetOuterPrivate(...)`, detachment, and physical removal update the Outer index in the same operation. Top-level objects are indexed under `nullptr`, and reparenting rejects any change that would create an Outer cycle.

`GetObjectsWithOuter(const DObject* Outer, bool bIncludeGarbage = false)` returns direct children only. Normal callers exclude garbage objects. Lifecycle and destruction code may pass `true` while it still needs to find objects awaiting physical removal.

The Outer index is a query accelerator. It does not own objects, create a GC strong reference, or replace `OuterPrivate` as hierarchy state.

## Lifecycle API

The public lifecycle operations are:

- `AddToRoot(DObject*)` and `RemoveFromRoot(DObject*)` maintain counted manual roots.
- `FScopedObjectRoot` provides a move-only scoped root reference.
- `MarkAsGarbage(DObject*)` requests destruction of one object and immediately makes it logically invalid without physically removing it.
- `MarkObjectHierarchyAsGarbage(DObject*)` iteratively applies the same request to the root and every current structural descendant found through the Outer index.
- `ReleaseClassDefaultObjects()` clears all class-default ownership derived-first before the host's shutdown collection.
- `ReleaseClassDefaultObjectsForModule(FName)` releases and synchronously drains one module's class-default batch before its native shutdown callback.
- `IsValid(...)` rejects garbage and begin-destroyed objects.
- `CollectGarbage()` performs a complete synchronous mark-sweep collection.
- `DObject::BeginDestroy()`, `IsReadyForFinishDestroy()`, and `FinishDestroy()` define the GC-controlled destruction phases.

`MarkAsGarbage()` only sets the garbage state and updates the garbage count. It does not invoke lifecycle callbacks, unregister the object, alter the Outer index, run a destructor, or release memory. The object therefore remains discoverable through registry APIs that explicitly include garbage, and its generation handle continues to resolve until physical removal. Physical removal invalidates the slot generation so an old handle cannot resolve to a later object that reuses the slot.

There is no public immediate-destruction API. Systems that retire an independent object remove it from their own active data structures and call `MarkAsGarbage()`. Systems whose lifecycle contract owns a complete structural tree use `MarkObjectHierarchyAsGarbage()` instead of relying on sweep to infer descendants. Package failure rollback and deletion, transient World and Level retirement, PIE teardown, duplication rollback, and engine exit follow this rule. Ordinary Package unload is different: it temporarily clears the Package's `Standalone` residency and lets reachability decide whether the graph can be collected, restoring residency when a live strong reference prevents unload.

Hierarchy marking is an explicit system request, not a GC reachability rule or ownership edge. It snapshots the current Outer tree at request time and remains iterative for deep trees. An object that must outlive a Package, World, PIE session, or other parent must be reparented before that hierarchy is marked; reparenting it afterward cannot reverse its garbage state.

Permanent reflected metadata objects are never swept. This includes intrinsic objects and registered reflected type metadata such as `DClass`, `DStruct`, and `DEnum` instances.

Class defaults and default subobjects remain registered but are templates, not
ordinary live instances. A ready `DClass` reports its default through
`AddReferencedObjects(...)`, so normal GC retains the complete reflected and
Outer-linked template graph. Templates cannot be manually rooted, renamed,
reparented, package-dirtied, saved, duplicated as ordinary graphs, or selected
by ordinary garbage requests. Only the class-default release transaction may
clear ownership and mark the template hierarchy.

## Strong Reference Sources

The mark phase starts from root-set and permanent objects, then follows only these strong edges:

- reflected `TObjectPtr` properties
- `TObjectPtr` elements reached through supported arrays, maps, and nested reflected structs
- explicit native strong references reported by `DObject::AddReferencedObjects(...)`
- the current object's `Child -> Outer` reference
- the ready `DClass -> class default` native ownership edge

The base `DObject::AddReferencedObjects(...)` implementation executes the object's precompiled GC reference schema through `ForEachObjectReference(...)`. A derived type may override it, call the base implementation, and pass additional native object references to `FReferenceCollector`.

`ConstructDClass(...)` and `ConstructDStruct(...)` assemble immutable typed reference schemas after attaching all generated properties. Schemas contain only strong-reference operations: wrapped object pointers, reference-bearing arrays and maps, and nested structs whose own schemas contain references. Raw reflected `DObject*` properties and all non-reference properties emit no operation. Struct schemas are relative to a struct value and are reused from object fields and container elements; class schemas reuse the superclass operations before appending directly declared properties. If generated registration exposed provisional superclass metadata before its properties were finalized, superclass finalization rebuilds already assembled descendant schemas so the flattened inherited operations remain complete.

Schema execution continues to use property value accessors and generated container helpers. A reference-bearing container without its required helper, or a struct property without resolved `DStruct` metadata, fails during schema assembly instead of being silently omitted during collection. Reflection layouts are immutable after construction; the current system does not invalidate or rebuild schemas at runtime.

The collector does not follow:

- the Outer index from `Outer -> Child`
- reflected raw `DObject*` properties
- `TWeakObjectPtr`
- `TSoftObjectPtr`
- arbitrary pointers on the native stack

A local or otherwise unreflected `TObjectPtr` is a handle, not an automatically discovered root. It keeps an object alive only when it is stored in a reflected reachable field or explicitly reported to the reference collector.

## Outer Reachability Rules

Outer has UE-style one-way lifetime semantics:

| Situation | Result |
| --- | --- |
| Rooted Outer with an otherwise unreferenced child | The child is collected. |
| Rooted child | The complete Outer chain remains reachable. |
| Rooted child with an unreferenced sibling | The sibling is collected. |
| Rooted package with a `TObjectPtr` main asset | The explicit asset reference keeps the main asset alive; Outer alone does not retain arbitrary descendants. |

These rules keep structural containment separate from ownership. Systems that require an object to remain alive must store a real strong reference or report an explicit native reference.

## Mark Phase

Before marking, `CollectGarbage()` clears the transient `Reachable` flag from registered objects. It enqueues every non-garbage root-set or permanent object and drains an iterative worklist.

For each newly reached object, the marker:

1. sets `Reachable`;
2. enqueues the object's current Outer;
3. calls `AddReferencedObjects(...)` to execute the class reference schema and enqueue explicit native references.

The marker never queries direct children. Both ordinary reference graphs and deep Outer chains are therefore processed without recursive calls or native stack growth.

Objects already marked as garbage are not made reachable again, even if they still carry a root flag or are referenced by another object.

## Sweep And Destruction

Sweep selects every non-permanent object that is already garbage or was not marked reachable. It does not inspect reachable children to rescue an Outer; reachable children already marked their Outer chains during the mark phase.

Every selected object is first marked as garbage. GC then advances it through these phases:

1. Set `BeginDestroyed` and call `BeginDestroy()` exactly once.
2. Keep the object registered while `IsReadyForFinishDestroy()` returns false.
3. Call `FinishDestroy()` exactly once after readiness is reported.
4. Invoke the private GC-only `DestroyObject()` finalizer.

The private finalizer asserts that Begin, readiness, and Finish have already completed. It does not discover candidates, recurse into children, or synthesize missing callbacks. It removes the object from the Outer and registry indices, invalidates its handle generation, invokes the dynamic destructor, and releases the allocation synchronously. After it returns, no raw access to the object is valid.

The Outer index is used only to order independently selected candidates so children are physically removed before their Outer. If a forcibly garbage Outer has a reachable child, GC detaches that child instead of recursively destroying it. This preserves the rule that hierarchy is not ownership.

## Pointer And Handle Semantics

`TObjectPtr` and `TWeakObjectPtr` use stable index-and-generation handles. Handle resolution and registry removal are constant-time, and generation changes prevent stale handles from resolving after slot reuse.

- Reflected `TObjectPtr` fields are GC strong references.
- Raw `DObject*` fields are not automatically traversed and must not be retained across collection unless another strong reference guarantees lifetime.
- `TWeakObjectPtr` is non-owning and becomes invalid as soon as its target is pending kill.
- `TSoftObjectPtr` retains a canonical package-main-asset path plus a weak loaded-object cache. It never keeps the target alive; after collection or package unload, the path remains while the cache stops resolving.
- Worker threads may carry independent weak-handle copies but may only resolve or assign `DObject` references on the game thread.

Reflected `TWeakObjectPtr<T>` is supported only as explicit `Transient` runtime
state, including fixed arrays, Array values, Map values, and nested Structs.
GC schema compilation deliberately ignores every such leaf, so neither direct
nor container values retain a target. Property snapshots copy the generation
handle without rooting its target. Weak Map keys are prohibited. `TSoftObjectPtr`
serializes or snapshots only its path identity; its weak cache is runtime-only.

## Automatic Collection

The engine checks the garbage-collection scheduler after end-of-frame render synchronization. Collection may be triggered by:

- elapsed collection interval
- pending-kill object pressure
- live-object growth pressure

The elapsed interval starts at `IntervalSeconds`. A collection with no sweep candidates backs it off by `IntervalBackoffMultiplier`, capped by `MaxIntervalSeconds`; finding any candidate restores the base interval. The cap is the correctness fallback for garbage created only by removing a strong reference, because reference mutations do not currently have a write barrier. Pending-kill and object-growth pressure continue to trigger during backoff.

`TryCollectGarbage()` logs the selected automatic trigger immediately before
collection. `CollectGarbage()` remains available for explicit collection and
logs the completed mark-and-sweep statistics for every collection. Automatic
and explicit collection both execute synchronously on the game thread.

## Required Invariants

- `OuterPrivate` and the Outer index must always agree.
- The Outer hierarchy must remain acyclic.
- The Outer index must never act as ownership or a GC strong reference.
- Normal Outer queries must filter garbage; lifecycle internals may include it explicitly.
- Every reachable child must mark its complete Outer chain during mark, not through a sweep fallback.
- Marking and hierarchy destruction must remain iterative for deep chains.
- `MarkAsGarbage()` must not invoke callbacks or physically mutate the registry.
- `MarkObjectHierarchyAsGarbage()` must use the Outer index iteratively and mark only the tree that exists when the request is made.
- Objects crossing a hierarchy lifetime boundary must be explicitly reparented before the old hierarchy is marked.
- `BeginDestroy()` and `FinishDestroy()` must each run at most once.
- Only GC may invoke the private physical finalizer, and only after Finish has completed.

## Validation

Lifecycle and GC changes must cover:

- registration, rename, reparent, detachment, garbage filtering, and physical removal in the Outer index
- rooted Outer collecting an unreferenced child
- rooted child retaining the complete Outer chain
- rooted child not retaining a sibling
- reflected `TObjectPtr`, supported containers, and explicit native strong references
- garbage objects being collected even when rooted
- logical invalidation before physical removal
- hierarchy-wide logical invalidation and explicit reparent escape
- package unload, World retirement, PIE stop, and engine exit using hierarchy requests
- delayed finish readiness across multiple collections without repeated callbacks
- handle and Outer-index visibility between `MarkAsGarbage()` and physical removal
- automatic GC physically removing an already marked hierarchy
- deep Outer chains without recursive stack growth
- stable handle invalidation after removal and slot reuse

Run the focused tests through the repository build driver:

```powershell
.\DevTool.bat test CoreDObjectTests --plain
```

For changes that affect runtime behavior or public lifecycle contracts, also build the complete registered profile:

```powershell
.\DevTool.bat build --target all --plain
```
