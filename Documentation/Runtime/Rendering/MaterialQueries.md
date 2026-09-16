# Scoped Material Queries

Summary: Define loaded-material discovery, batch cache ownership, raw-pointer results, and notification reentrancy.

Modules: Engine, CoreDObject, MaterialEditor

Last reviewed: 2026-09-16

## Identity and authority

`DMaterialInstance::Parent` is authoritative. `FObjectCacheContext` in Engine
builds a temporary reverse table for one synchronous update batch; it is not a
second relationship registry. CoreDObject supplies process-local object keys
and lifetime primitives, not material dependency semantics. See
[object identity and lifetime](../Core/GarbageCollection.md#pointer-and-handle-semantics).

The interface follows the public UE
[FObjectCacheContext](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FObjectCacheContext)
model: lazy discovery, domain-specific reverse tables and raw-pointer iteration.
The chosen parent table is a Durin implementation decision. It does not imply
that UE maintains an identical inheritance index.

## Query and result ownership

Create `FObjectCacheContext` after canonical relationship mutations. Its first
query snapshots `GDObjectArray` with `LiveOnly` admission, selects valid
materials, and builds parent-to-direct-children edges. Later queries reuse both.
There is no persistent class index: the first query still scans unrelated live
objects. Construction without a query performs no discovery.

- `GetDirectMaterialChildren(Parent)` returns admitted material instances whose
  immediate parent is exactly Parent.
- `GetMaterialsAffectedByMaterials(Roots)` returns the admitted union of the
  roots and their descendants. `GetMaterialsAffectedByMaterial` is the
  single-root convenience form.
- Results exclude templates, private package graphs and pending destruction.
  Excluded intermediate ancestors remain traversable, preserving the canonical
  parent-walk semantics for valid descendants. Repeated roots and cycles are
  deduplicated; the separate property-resolution depth limit does not truncate
  dependency queries.
- Results use object-key ordering. `TObjectCacheIterator<T>` owns its pointer
  array; another query cannot invalidate that array. Range iteration yields
  `T*` and skips recipients marked pending destruction before they are visited.
  Its pointers borrow lifetime from the producing context. Neither the range
  nor its pointers may outlive that context or enter deferred work.

The legacy `GetLoadedDirectMaterialChildren` and `GetLoadedMaterialDependents`
helpers construct a temporary context and return owned object-key snapshots.
Use explicit weak references for deferred access. Synchronous production loops
use the context's raw-pointer ranges.

## Lifetime and mutation boundaries

All discovery and raw-pointer consumption occur on the game thread. A context
owns `FGarbageCollectionDeferralScope`; registered objects cannot be physically
collected while it lives. Nested contexts independently extend that deferral.
Collection requests remain pending after scope exit until the collector is
next invoked. This is temporary exclusion of collection, not a GC root.
Marking garbage is still allowed and invalidates access immediately.

Do not create new material relationships, reparent materials, or change result
admission while reusing a discovery context. It is a batch snapshot, not a live
view. A generation check cannot detect reparenting. If old relationships are
needed, capture them explicitly before the mutation and create a new context
for the resulting graph.

The outer update owner explicitly passes the context through compilation
invalidation, synchronous result admission, parameter-layer preparation and
render publication. An asynchronous completion pump shares its own context
across admitted results. Requests, worker results and retry queues never store
a context or its raw results; material async owners are weak references.

## External notifications and reentrancy

Complete internal updates before parameter notifications. Call `EndDiscovery()`
before invoking external listeners: subsequent queries on that context fail a
contract check, while prepared results retain lifetime protection until scope
exit. Sealing twice is harmless. Root notifications remain first, followed by
the prepared dependents in key order, excluding the root and retired recipients.

Callbacks may synchronously edit another material or reparent a descendant.
Such an edit creates a fresh context and finishes before its setter returns.
The outer batch finishes its already prepared notifications; it does not
rediscover or apply further internal updates using the old table. Thus a
reparented recipient can receive both its nested update notification and its
previously prepared outer notification. Subsequent batches see the new Parent.

This differs from UE's documented
[shared nested scope](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FObjectCacheContextScope),
which assumes no creation or dependency mutation during the scope. Durin has no
implicit thread-local cache and does not queue synchronous editor mutations.

## Diagnostics and validation

Per-context diagnostics and `GetMaterialLoadedQueryDiagnostics()` expose query,
snapshot, parent-table build, scanned-object/material and result counts. One
context builds at most one snapshot/table; separate public edits remain
separate batches. Dynamic publication already reused its notification snapshot
before this cache, so it does not gain a second eliminated scan.

`FMaterialDependencyTests` checks equivalence against an independent parent-walk
oracle, cycles, long chains, overlapping roots, reentrant edits and GC requests.
`FMaterialQualificationTests.ObjectQueryCacheCandidateCost` records diagnostic
CPU cold/warm timings with controlled materials and unrelated objects. Run it
through `DevTool` in qualification mode; it has no machine-dependent latency
threshold. It remains outside routine correctness selections.
