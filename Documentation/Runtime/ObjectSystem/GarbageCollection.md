# Garbage Collection

Summary: Define managed-object reachability, collection, rooting, and destruction contracts.

Modules: CoreDObject, Engine, Launch

Last reviewed: 2026-11-10

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

`EObjectQueryScope::IncludeUnpublished` is reserved for lifecycle and controlled
replacement traversal. It includes templates and private package graphs in the
same object array. Ordinary `LiveOnly` and `IncludeTemplates` queries exclude
private graphs. Exact handles continue to resolve their original generations;
visibility never redirects a handle. The array revision changes on registration,
removal, reparenting, and garbage marking so prepared operations can reject stale
membership.

## Controlled Graph Replacement

`ObjectGraphReplacement.h` supplies disk-independent package replacement on the
game thread. `DPackage::InitializePreparedAssetPackage` gives a candidate the
same package identity without claiming its registry entry. The caller must keep
the candidate alive during construction. A successful `FObjectGraphReplacement::Prepare`
accepts ownership, pins live objects needed for validation, maps package-relative
Outer/name paths, and prepares writable reference slots and detached container
copies. Failure leaves candidate ownership with the caller. Preparation and commit return
`std::expected<void, FObjectReplacementError>` with a typed code/reason, owned object/property routes,
reference budgets, ownership counts, participant indices and nested Map/property
copy/container-operation causes. `ToString` owns the prose;
participant callbacks return the same typed result. The caller must
quiesce editing and resource work; these primitives do not implement an editor
lease or asset-family admission.

`GetLiveAssetPackages` enumerates the existing package registry without retaining
packages, excluding private and garbage graphs. `GetAssetPackageRevision` changes
when live membership, identity or eligibility changes, including garbage marking
before physical destruction. AssetRegistry consumes this owner-thread revision to
publish value-only discovery metadata and lifecycle changes; CoreDObject has no
AssetRegistry dependency or second package owner.

A pair with null `Current` adds a new package. Prepare reserves its path without
making it visible through `FindPackage`; ordinary creation cannot claim that
reservation. Abort releases it. The nonthrowing commit publishes new and replaced
packages together. Graph application has no persistence callback; higher-level
owners request ordinary saving after the live graph is applied.

`TryCommit` rechecks identities, package edit revisions, object membership,
reference slots, container contents, and native participants before consuming
the plan in the same call. Its internal nonthrowing commit switches existing
package registry entries, writes references, swaps detached containers, and
invalidates soft caches. It does not notify observers. Map keys are rebuilt and
checked for collisions before commit; GC's temporary collector pointers are
never retained as writable slots. Container copying uses the shared reflected
value lifecycle described in [Property reflection](PropertyReflection.md#container-properties).

Native collector owners require an `IObjectReplacementParticipant`; additional
native strong handles require exact ownership claims. Participants prepare all
fallible work and retain their storage/module until retirement. Commit and abort
callbacks must not allocate, throw, broadcast, reenter GC/replacement, or perform
fallible resource work. Unenumerated native pointers cannot be discovered by
reflection and remain the owner's admission responsibility. Explicitly rooted
old objects, unhandled native ownership, unsupported container capabilities,
incompatible types, and referenced objects without a replacement are rejected.
Already retired members of the current package are excluded from its live source
graph, including construction components awaiting physical GC. Invalid members
of a candidate graph still reject preparation. This does not make expired exact
references remappable.

After commit, the operation keeps the old graph private and alive until `Retire`
accepts all participant receipts and verifies no external hard references remain.
Only then does it mark old objects as garbage and release its pins; physical
destruction follows normal deferred GC readiness. Keep the operation alive until
retirement succeeds. Before commit, `Abort` marks the accepted candidates instead.
Ordinary live weak handles are never rebound; they become invalid with their old
generation. Explicitly admitted serialized snapshots can prepare stored weak
keys through the same map and publication protocol, as described in
[runtime transaction contracts](Transactions.md#coredobject-boundary). This does
not redirect an already expired key or change ordinary weak-handle resolution.
The current primitive bounds package/object/reference counts; saved-file and
resource-memory budgets belong to the Engine preparation layer.

## Object Initialization

`StaticConstructObject` calls virtual `DObject::PostInitProperties` once after the
complete native constructor and identity/Outer registration, before returning the
object to its creator or loading graph values. Loaded and duplicated objects also
receive this hook at allocation; `PostLoad` remains the later graph-completion
notification. The default hook does nothing. The hook may request GC, but must not execute collection or reenter a host safe
point. Construction and its failure cleanup finish before the host services GC.

If a native constructor throws, no most-derived object lifetime remains and the
allocator releases its raw storage. If `PostInitProperties` throws, construction
has succeeded: mark the registered object hierarchy as garbage and rethrow, then
let normal GC perform virtual destruction. These failure paths must not share raw
allocation cleanup. This hook does not register render/physics state or begin play.

## Lifecycle API

The public lifecycle operations are:

- `AddToRoot(DObject*)` and `RemoveFromRoot(DObject*)` idempotently set and clear
  one UE-style manual root flag; repeated adds do not create independently owned
  root references.
- `TStrongObjectPtr<T>` owns one copyable, collector-visible native strong
  reference to an exact object generation.
- `MarkAsGarbage(DObject*)` requests destruction of one object and immediately makes it logically invalid without physically removing it.
- `MarkObjectHierarchyAsGarbage(DObject*)` iteratively applies the same request to the root and every current structural descendant found through the Outer index.
- `ReleaseClassDefaultObjects()` clears all class-default ownership derived-first before the host's shutdown collection.
- `ReleaseClassDefaultObjectsForModule(FName)` releases one module's class-default batch and returns false while destruction remains pending.
- `IsValid(...)` rejects garbage and begin-destroyed objects.
- `CollectGarbage(KeepFlags, bPerformFullPurge)` synchronously performs mark-sweep at an owner-established
  safe point. Collector reentry is diagnosed; arbitrary business-stack safety is
  a caller precondition, not dynamically detected.
- `TryCollectGarbage(KeepFlags, bPerformFullPurge)` refuses collector reentry and returns true
  after execution; only partial collection can leave pending destruction.
- `RequestGarbageCollection(bForcePurge)` is the Engine-free request extension; normal
  Engine callbacks use `DEngine::ForceGarbageCollection()`.
- `DObject::BeginDestroy()`, `IsReadyForFinishDestroy()`, and `FinishDestroy()` define the GC-controlled destruction phases.

`MarkAsGarbage()` only sets the garbage state and updates the garbage count. It does not invoke lifecycle callbacks, unregister the object, alter the Outer index, run a destructor, or release memory. The object therefore remains discoverable through registry APIs that explicitly include garbage, and its generation handle continues to resolve until physical removal. Physical removal invalidates the slot generation so an old handle cannot resolve to a later object that reuses the slot.

There is no public immediate-destruction API. Systems that retire an independent object remove it from their own active data structures and call `MarkAsGarbage()`. Systems whose lifecycle contract owns a complete structural tree use `MarkObjectHierarchyAsGarbage()` instead of relying on sweep to infer descendants. Package failure rollback and deletion, transient World and Level retirement, PIE teardown, duplication rollback, and engine exit follow this rule. Ordinary Package unload is different: it temporarily clears the Package's `Standalone` residency and lets reachability decide whether the graph can be collected, restoring residency when a live strong reference prevents unload.

Hierarchy marking is an explicit system request, not a GC reachability rule or ownership edge. It marks the current Outer tree, including unpublished and already-garbage descendants, using the stable Outer index and sibling back-pointers without allocating temporary storage or recursing. This keeps candidate rollback usable after allocation failure. Marking invokes no callbacks and does not mutate that index. An object that must outlive a Package, World, PIE session, or other parent must be reparented before that hierarchy is marked; reparenting it afterward cannot reverse its garbage state.

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
- live `TStrongObjectPtr` instances registered on the game thread
- explicit native strong references reported by `DObject::AddReferencedObjects(...)`
- exact live transaction identities reported by a reachable owner's native record traversal
- the current object's `Child -> Outer` reference
- the ready `DClass -> class default` native ownership edge

The base `DObject::AddReferencedObjects(...)` implementation executes the object's precompiled GC reference schema through `ForEachObjectReference(...)`. A derived type may override it, call the base implementation, and pass additional native object references to `FReferenceCollector`.

`ConstructDClass(...)` and `ConstructDStruct(...)` assemble immutable typed reference schemas after attaching all generated properties. Schemas contain only strong-reference operations: wrapped object pointers, reference-bearing arrays and maps, and nested structs whose own schemas contain references. Raw reflected `DObject*` properties and all non-reference properties emit no operation. Struct schemas are relative to a struct value and are reused from object fields and container elements; class schemas reuse the superclass operations before appending directly declared properties. If generated registration exposed provisional superclass metadata before its properties were finalized, superclass finalization rebuilds already assembled descendant schemas so the flattened inherited operations remain complete.

Schema execution continues to use property value accessors and generated container helpers. A reference-bearing container without its required helper, or a struct property without resolved `DStruct` metadata, fails during schema assembly instead of being silently omitted during collection. Reflection layouts are immutable after construction; the current system does not invalidate or rebuild schemas at runtime.

Owned function parameter buffers reuse a separate schema compiled at successful
`DFunction::Link`, covering only local parameter and return fields. Its raw object
slots, including container elements, are strong; weak and soft slots remain
non-owning. Cached struct operations use the current ordinary struct schema and
native collector, so later completion of provisional registration is visible.
The buffer root pass executes cached operations without compiling property trees.
See [Function Reflection](FunctionReflection.md#parameter-storage) for aggregate
lifetime, borrowed-call safe-point restrictions and parameter-specific reference policy.

The collector does not follow:

- the Outer index from `Outer -> Child`
- reflected raw `DObject*` properties
- `TWeakObjectPtr`
- `TSoftObjectPtr`
- arbitrary pointers on the native stack

A local or otherwise unreflected `TObjectPtr` is a handle, not an automatically discovered root. It keeps an object alive only when it is stored in a reflected reachable field or explicitly reported to the reference collector.

`TStrongObjectPtr` is the native-owner counterpart. Construction from a live
object or exact `FObjectKey` registers a collector root before the caller
continues; copying creates an independently releasable strong reference, moving
transfers one, and destruction or `Reset()` releases it. Construction and
release run on the game thread. A stale, removed, garbage, or begin-destroyed
generation produces an empty strong pointer. Explicit garbage marking still
wins over both strong pointers and manual roots.

## Collector-Enumerated Transaction Records

DurinEd's [transaction record foundation](../../Editor/Architecture/Transactions/TransactionRecords.md)
uses the same native-reference boundary without adding a new GC root source.
CoreDObject's `FPersistentObjectRef` reports references only when its reachable
owner explicitly traverses the non-reflected record. Ordinary references preserve
exact generations and never resurrect removed objects. CDO/default-subobject
references enumerate their stable root and cached leaf without name lookup in
the collector. Logical resolution outside collection follows the checked
[runtime transaction categories](Transactions.md#coredobject-boundary).

Focused transaction payloads store hard references as deduplicated exact
handles. A record enumerates its target and distinct hard values; weak handles
and soft paths remain outside collector traversal. Evicting the record removes
all of its strong edges. A referenced child still retains its Outer chain by
the ordinary mark rule, while a referenced Outer still does not retain children
or siblings.

The reachable editor `DTransBuffer` also enumerates pending and retained custom
participants, transaction package transitions, and tracked package-revision
state. Package forgetting, redo replacement, eviction, Reset, module drain, and
editor shutdown erase the corresponding native storage before its collector
edge disappears; none of these paths installs a transaction-specific manual
root.

`FPropertyValueSnapshot` is a retaining adapter over the shared
retention-neutral payload. Each copy owns `TStrongObjectPtr` references for its
hard object table; transaction records continue to use collector enumeration
instead of adding independent roots.

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

Before marking, `CollectGarbage(KeepFlags, bPerformFullPurge)` clears transient `Reachable` flags. It enqueues non-garbage root-set objects, permanent objects, objects matching KeepFlags, explicit package residency, and collector-visible native strong references, then drains an iterative worklist.

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

`FObjectPtr`/`TObjectPtr` retain their object-pointer representation and assignment
hook (currently no-op). `FWeakObjectPtr`/`TWeakObjectPtr` instead store object-key identity,
independently of `FObjectHandle` encoding. `FObjectKey`/`TObjectKey<T>` are
non-owning eight-byte process-local identity values for comparison, ordering and
hashing. Capture from a pointer and explicit `ResolveObjectPtr()` run on the game
thread; resolution rejects pending destruction. Null and stale identities remain
distinct, and equality/hash/order never resolve a pointer. Independent copies
may be compared or hashed on workers; concurrent mutation of one value is not
supported. Typed wrappers support forward-declared object types.

Keys retain slot/generation identity after destruction and never redirect to a
replacement graph. The object array remains the sole slot authority. Existing
32-bit generation wrap skips zero; an ancient identity can alias after
2^32-1 reuses of the same slot. Keys and weak identities must not be persisted
as asset/package identities. In-memory weak snapshots preserve stale identity
without rooting targets. Use `FWeakObjectPtr(Key)` explicitly for deferred access
and `FStrongObjectPtr(Key)` only when independent retention is intended.

- Reflected `TObjectPtr` fields are GC strong references.
- Raw `DObject*` fields are not automatically traversed and must not be retained across collection unless another strong reference guarantees lifetime.
- `TWeakObjectPtr` is non-owning and becomes invalid as soon as its target is pending kill.
- `TSoftObjectPtr` retains an exact authored `FSoftObjectPath` plus an epoch-qualified weak loaded-object cache. It never keeps the target alive; collection invalidates the weak handle, while unload, identity mutation, catalog changes, and runtime shutdown invalidate populated caches globally without changing authored identity.
- Worker threads may carry independent weak-handle copies but may only resolve or assign `DObject` references on the game thread.

Reflected `TWeakObjectPtr<T>` is supported only as explicit `Transient` runtime
state, including fixed arrays, Array values, Map values, and nested Structs.
GC schema compilation deliberately ignores every such leaf, so neither direct
nor container values retain a target. Property snapshots copy the generation
handle without rooting its target. Weak Map keys are prohibited. `TSoftObjectPtr`
serializes or snapshots only its authored path identity; its weak cache and
cache epoch are runtime-only. Its observable states are `Null`, `Pending`,
`Valid`, and `Stale`, and `Get()` never initiates lookup or loading.

## Automatic Collection

The engine checks the garbage-collection scheduler after end-of-frame render synchronization. Collection may be triggered by:

- elapsed collection interval
- pending-kill object pressure
- live-object growth pressure

The elapsed interval starts at `IntervalSeconds`. A collection with no sweep candidates backs it off by `IntervalBackoffMultiplier`, capped by `MaxIntervalSeconds`; finding any candidate restores the base interval. The cap is the correctness fallback for garbage created only by removing a strong reference, because reference removal alone does not otherwise trigger collection. Pending-kill and object-growth pressure continue to trigger during backoff.

`DEngine::ForceGarbageCollection(bForcePurge)` records a request. The Core-only
extension is `RequestGarbageCollection(bForcePurge)`. Requests share one process-wide
slot; full-purge intent is sticky until a full collection services it. Requests
never carry KeepFlags. Execution takes explicit `EObjectFlags KeepFlags` and
`bool bPerformFullPurge = true`, matching the UE argument shape. Tools and tests
choose their retention policy explicitly; the Engine uses Standalone in editor
mode and NoFlags otherwise. Explicit asset-package residency is a native root
independent of KeepFlags, and explicit garbage marking still overrides it.

`TryCollectGarbage(KeepFlags, bPerformFullPurge)` returns false on collector reentry.
It does not detect arbitrary business-stack safety. Both synchronous entries
consume only serviced request intent before running lifecycle callbacks; requests
created by those callbacks survive. Partial collection preserves a pending full
request. Purge-only work never consumes a request or scans newly allocated objects.

`DEngine::ConditionalCollectGarbage()` owns frame-end policy, including frames
without a World. Each Engine records its last checked frame even if no collection
was needed. Core independently records the actual collection serial, frame, time,
and resulting object count. Explicit collection changes Core telemetry, not the
Engine's checked frame. Engine also consults the actual frame to avoid redundant
collection across replacement hosts in one frame. Refused collector reentry does
not consume a check. Hosts advance `GFrameCounter` for each admitted frame.

Scheduling settings, adaptive intervals, pressure thresholds, and trigger logging
belong to Engine. `Engine/GarbageCollectionSettings.h` provides configuration;
policy types and evaluation live in Engine's private implementation. Engine reads
the existing `CoreDObject.GC` configuration key for compatibility. Core exposes
only collection telemetry and an internal full-request query; it does not call
Engine or own wall-clock scheduling. Engine observes completed collections before
policy evaluation. Standalone tools need no Engine scheduler or running loop.

### Pending Destruction

Reachability and BeginDestroy remain synchronous. A partial collection enqueues
unreachable objects and makes one bounded destruction pass. Later frames call
`IncrementalPurgeGarbage(true, TimeLimit)` even with automatic collection disabled.
The budget is cooperative between callbacks, not a deadline imposed on callbacks.
Objects waiting on readiness are retried without repeating BeginDestroy or
FinishDestroy. `IsIncrementalPurgePending()` describes outstanding destruction,
not whether another reachability pass was requested.

`IncrementalPurgeGarbage(false)` drains existing candidates fully and waits for
readiness. `CollectGarbage(KeepFlags, true)` also drains all candidates discovered
by its new scan. A new scan always finishes an older pending purge first; Engine
normally advances the pending purge instead of starting another scan. A full-purge
request instead drains the previous cycle and runs a full collection at that same
safe point. Full drains
require readiness to progress independently of the blocked caller. A caller-owned
fence must be released before entering a full drain. Shutdown therefore begins
with partial collection, flushes render work, drains pending destruction, then
collects any newly retired objects. Module-default release remains a nonblocking
readiness operation that can return false for a later retry.

The collector remains single-entry and game-thread-only. There is no incremental
reachability or concurrent GC. `ConditionallyMarkAsReachable` is currently a no-op
handle hook, not a functional write barrier; synchronous reachability does not
require one. A future incremental mark implementation must establish mutation
barriers and phase contracts before changing this guarantee.

## Safe-Point And Lifetime Contract

Business callbacks (construction, initialization, registration, serialization,
transaction replay, reflection, World, toolkit and material notifications) may
request GC. They must not call either synchronous collector, pump another host
frame or invoke `ConditionalCollectGarbage`. Operations and exception compensation
must finish, including native destructors and borrowed views, before the owner
services requests. Operation exit does not collect. No business deferral counter
or automatic refusal guarantee exists.

Synchronous collection remains a caller-owned boundary: normal frame end after
render synchronization, shutdown after consumer detachment, offline maintenance,
explicit package unload/removal, and standalone test hosts. Package unload/removal
and module-default drains are themselves collecting operations; callers must not
invoke them from callbacks with borrowed object storage. Initial default-building
failure cleanup marks objects and requests collection instead of collecting from
inside registration. GC callbacks can request a later pass; recursive Try returns
false and recursive Collect fails before consuming that request.

Objects retained across these boundaries require collector-visible ownership:
reflected references, `AddReferencedObjects`, or native strong handles. A raw
pointer, Outer relationship, borrowed parameter buffer, query view or operation
stack is not ownership. Subsystem collections and transaction history continue
reporting their references; editor sessions retain their existing strong owners.
Material query contexts own snapshots of keys, not the referenced objects; consume
borrowed results before collection and resolve retained keys again afterwards.

Logical retirement is independent of physical storage lifetime. Explicit garbage
marking is not overridden by a strong handle. After callbacks, retain existing
state/validity checks, frozen iteration sets, weak-handle resolution and deferred
host teardown. World, transaction and editor admission guards remain responsible
for business reentry; they are not GC locks. Game-thread checks, collector
non-reentrancy and finish-destroy readiness remain enforced. Durin does not emulate
UE's concurrent GC lock, incremental reachability or AutoRTFM transaction machinery.

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
- copied, moved, reset, stale, and explicitly garbage `TStrongObjectPtr` references
- idempotent repeated `AddToRoot` followed by one `RemoveFromRoot`
- garbage objects being collected even when rooted
- logical invalidation before physical removal
- hierarchy-wide logical invalidation and explicit reparent escape
- package unload, World retirement, PIE stop, and engine exit using hierarchy requests
- delayed finish readiness across purge passes without repeated callbacks
- handle and Outer-index visibility between `MarkAsGarbage()` and physical removal
- automatic GC physically removing an already marked hierarchy
- deep Outer chains without recursive stack growth
- stable handle invalidation after removal and slot reuse

CoreObjectTests validates collector execution and pending destruction independently.
EngineGarbageCollectionTests exercises the Engine Force/Conditional entries,
including request admission, private scheduling policy, automatic pressure, and frame accounting.
Run them through the repository build driver:

```powershell
.\DevTool.bat test CoreObjectTests
.\DevTool.bat test EngineGarbageCollectionTests
```

For changes that affect runtime behavior or public lifecycle contracts, also build the complete registered profile:

```powershell
.\DevTool.bat build all --color never
```
