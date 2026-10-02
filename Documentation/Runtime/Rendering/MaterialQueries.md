# Scoped Material Queries

Summary: Define loaded-material discovery, batch cache ownership, raw-pointer results, and notification reentrancy.

Modules: Engine, CoreDObject, MaterialEditor

Last reviewed: 2026-10-03

## Identity and authority

`DMaterialInstance::Parent` is authoritative. Engine maintains a derived,
non-owning parent-to-direct-children index keyed by generation-bearing object
keys. Native parent assignment, dynamic construction, loading/duplication,
reflected edits and history replay refresh the edge before dependent discovery.
Destruction removes the retiring owner's incoming edge after dependent invalidation.
Index entries do not retain objects or become GC roots. CoreDObject owns identity
and lifetime primitives, not material dependency semantics.

Package replacement prepares an index candidate with rewritten parent identities,
validates the source index revision, and swaps it during the same non-failing
commit as reflected reference replacement. Failed preparation or validation leaves
live edges unchanged. Isolated package graphs may have indexed edges, but query
result admission excludes them until publication.

`FObjectCacheContext` keeps batch-local result and adjacency snapshots over this
index. Its interface follows UE's public
[FObjectCacheContext](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FObjectCacheContext)
query model; the persistent inheritance index is a Durin implementation decision.
Direct reflected writes that bypass editing, loading or replacement notifications
must explicitly refresh bindings before dependency queries.

## Query and result ownership

Create `FObjectCacheContext` after canonical relationship mutations. Queries lazily snapshot only indexed nodes reached from the requested roots,
applying live, non-template and published-package admission. Later queries reuse
captured nodes. Discovery never snapshots the global object array and does not
visit unrelated material families. Construction without a query performs no discovery.

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
snapshot, inspected-material and result counts. `SnapshotCount` counts contexts
that begin discovery; `ScannedMaterialCount` counts distinct material nodes
inspected within each context. The retained `ScannedObjectCount` and
`ParentTableBuildCount` fields stay zero for indexed inheritance queries. Separate
public edits remain separate batches; their discovery cost follows the affected
subtree rather than unrelated live objects.

`FMaterialDependencyTests` checks equivalence against an independent parent-walk
oracle, cycles, long chains, overlapping roots, reentrant edits and GC requests.
`FMaterialQualificationTests.ObjectQueryCacheCandidateCost` records diagnostic
CPU cold/warm timings with controlled materials and unrelated objects. Run it
through `DevTool` in qualification mode; it has no machine-dependent latency
threshold. It remains outside routine correctness selections.
