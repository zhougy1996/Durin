# Object Identity and Query Cache Plan

Summary: Separate object reference, weak reference, and identity-key responsibilities, then introduce a scoped material query cache with UE-style raw-pointer iteration.

Last reviewed: 2026-09-16

Status: Archived
Completed: 2026-09-16

## Current Status

Stages 0 through 5 are complete. Scoped discovery and identity migration are
implemented; measured first-query cost does not yet justify a persistent class index.
The selected order is contract definition, object identity types, consumer
migration, scoped queries, update-batch integration, and measurement-driven
class indexing. Changing the internal encoding of `FObjectHandle` is a separate,
deferred decision rather than a prerequisite for this cache.

Initial source inspection established:

- `FObjectPtr` and `FWeakObjectPtr` both store `FObjectHandle`, currently an
  eight-byte object-array slot index and generation pair.
- `FDObjectArray::Remove` advances the slot generation; `Resolve` checks slot
  occupancy and generation but does not filter pending destruction.
- `FWeakObjectPtr::Get` additionally filters `IsPendingKill` and enforces the
  game-thread access contract.
- Loaded material queries snapshot all live objects, select materials, and
  answer dependency questions using the current parent chain.
- Material code directly uses handle fields for identity comparisons, hashing,
  and sorting. Strong-reference tracking and object replacement also consume
  handles, so this is a shared API migration rather than a material-only rename.

## Goal

Give reference types explicit responsibilities consistent with UE's public
model, and reuse material dependency discovery within one synchronous update
batch. Preserve parameter, compilation, rendering, and editor notification
behavior while reducing repeated scans.

The cache returns a range/iterator of raw pointers, not an array of weak
pointers. Object lifetime and relationship freshness must be established by
the scope contract before those results can be consumed safely.

## Selected Design and Boundaries

- `FObjectPtr` / `TObjectPtr<T>` represent GC-visible references when traced by
  the owning object's reference traversal. A local wrapper is not automatically
  a GC root. Preserve existing barriers and collector behavior.
- `FWeakObjectPtr` / `TWeakObjectPtr<T>` provide non-owning access with identity
  and lifecycle validation. Keep index/generation identity independent of the
  public representation of `FObjectHandle`.
- Introduce `FObjectKey` / `TObjectKey<T>` for stable process-local identity,
  equality, and hashing. Stale keys retain their original identity; equality
  and hashing never depend on resolving a pointer. Do not persist keys or
  weak identities as asset paths or package identities.
- Reserve `FObjectHandle` for object-pointer representation after migrating
  external identity consumers. Its current encoding may remain initially;
  responsibility alignment does not require UE's complete internal encoding.
- Runtime Engine owns `FObjectCacheContext` and material dependency semantics;
  CoreDObject owns identity primitives and any eventual class lookup index.
- The first cache covers loaded material inheritance only. `Parent` remains
  authoritative. Build a lazy parent-to-direct-children table from one material
  snapshot, then traverse affected descendants with cycle protection.
- Use object keys for identity maps and raw pointers for scoped results.
  Results must not escape the synchronous batch or enter async callbacks.
  Long-lived or deferred consumers explicitly store weak references instead.
- Construct the cache after canonical relationship mutations. A cached table
  is a batch snapshot, not a live view; generation validation cannot detect a
  parent change. If an operation needs old relationships, capture them
  explicitly before mutation.
- Prefer completing internal updates before broadcasting external notifications.
  Reentrant edits must receive a fresh discovery batch; do not reuse stale
  tables. Stage 0 must establish whether queued follow-up batches preserve
  existing immediate-update semantics and define synchronous nesting if needed.
- Preserve current live-object admission rules, inclusion of the changed root,
  cycle/depth handling, deduplication, and deterministic notification ordering.
  Direct children and transitive dependents remain distinct queries.
- No persistent universal object dependency graph, automatic semantic edges
  for every GC reference, or AssetRegistry substitution for live relationships.
  Function callers and material-to-component maps are later, separate cache
  extensions, not requirements of the initial inheritance cache.

## Implementation Stages

### Stage 0: Establish contracts and baseline

Dependencies: none.

- [x] Inventory handle and pointer consumers across every source and test root
      declared by `Durin.dworkspace`; classify identity, deferred access,
      traced reference, and explicit strong ownership separately.
- [x] Trace actual material setter, property edit, transaction, load, reload,
      compilation completion, and notification paths to select the update-batch
      owner and locate reentrant mutations.
- [x] Define scope lifetime, borrowed versus owned result storage, nested scope
      behavior, and cache invalidation boundaries.
- [x] Prove how physical destruction is excluded during pointer consumption;
      evaluate the existing GC deferral scope and any other teardown paths.
      Define handling for objects marked pending destruction within the scope.
- [x] Define weak/key null, stale, equality, hash, ordering, and thread contracts;
      preserve generation wrap behavior or explicitly resolve its limitations.
- [x] Record scan/build counts for representative single-root and multi-root
      update batches, separating existing snapshot reuse from repeated work.

Acceptance: a reviewed contract and concrete consumer inventory are recorded
here; no lifecycle safety assumption remains implicit. Baseline evidence does
not claim gains that the current batching already provides.

### Stage 1: Introduce object keys and separate weak identity

Dependencies: Stage 0.

- [x] Add `FObjectKey` and typed keys with centralized identity hashing and
      comparison; define explicit conversion and resolution APIs.
- [x] Move weak-reference identity storage behind the weak/key implementation
      boundary so it no longer depends on `FObjectHandle`'s public encoding.
      Continue using the same object array and generation mechanism.
- [x] Preserve game-thread resolution, pending-destruction filtering, GC
      non-retention, forward-declared type support, and intended value sizes.
- [x] Migrate directly affected CoreDObject interfaces without introducing
      implicit conversions that obscure strong versus weak ownership.
- [x] Validate slot reuse, stale versus null identity, hash stability after
      destruction, weak access filtering, and non-retention through GC.

Acceptance: weak references and identity keys have independent contracts;
object-pointer behavior and lifetime semantics remain unchanged.

### Stage 2: Migrate identity and deferred-access consumers

Dependencies: Stage 1.

- [x] Replace business-level handle keys, comparisons, deduplication, and
      sorting with object-key operations.
- [x] Replace deferred object access with weak references where appropriate,
      including material retry queues and asynchronous result ownership checks.
- [x] Audit strong-reference registration and graph replacement individually;
      preserve explicit retention counts and replacement validation.
- [x] Remove obsolete public identity helpers and field access after migrating
      all consumers; do not mechanically replace every handle with a weak pointer.
- [x] Keep loaded-query algorithms unchanged for this stage; verify stale async
      results, object replacement/reload, and strong retention behavior.

Acceptance: external identity consumers no longer depend on handle encoding;
all affected workspace projects compile and focused behavior checks pass.

### Stage 3: Implement the scoped material query cache

Dependencies: Stages 0 through 2.

- [x] Add Engine-owned `FObjectCacheContext` with lazy loaded-material collection
      and a temporary parent-to-direct-children table.
- [x] Add a result range/iterator exposing raw material pointers, with explicit
      storage lifetime and invalidation rules from Stage 0.
- [x] Implement direct-child and affected-subtree queries, including self,
      overlapping roots, deterministic ordering, and malformed parent chains.
- [x] Initially use the existing global snapshot for candidate discovery; make
      first-build cost and repeated-query reuse observable through counters.
- [x] Validate result equivalence against the current query behavior and prove
      that repeated queries in one context build each required cache only once.

Acceptance: query results preserve semantics and lifetime safety; no claim is
made that the initial global object scan has been eliminated.

### Stage 4: Share the cache across material update batches

Dependencies: Stage 3.

- [x] Have the outer update owner create the context and explicitly share it
      across parameter, compilation, rendering, and notification preparation.
- [x] Preserve parameter snapshots, dirty flags, revisions, event ordering, and
      required immediate updates; discovery optimization cannot omit propagation.
- [x] Implement the selected callback/reentrancy policy with fresh discovery for
      relationship changes; prevent raw results from escaping into queued work.
- [x] Validate reparenting, Edit/Cancel, Undo/Redo, loading, duplication, reload,
      destruction, overlapping changed roots, and edits made by callbacks.
- [x] Compare scan/build counts with Stage 0 and record observed savings;
      retire redundant scans only after equivalent behavior is established.

Acceptance: each synchronous batch shares discovery, callbacks cannot consume
stale relationships, and existing material behavior remains intact.

### Stage 5: Evaluate and optionally add class indexing

Dependencies: Stage 4 measurements.

- [x] Measure remaining first-query cost with many unrelated objects and a
      controlled number of materials; record whether class indexing is justified.
- [x] If justified, add CoreDObject class-based candidate lookup, including
      derived classes and existing publication/template/garbage admission rules.
      Integrate registration/removal without a second object lifetime authority.
      Not applicable: deferred by the measured indexing decision below.
- [x] Switch cache candidate collection to the typed lookup and validate index
      consistency through construction, failure cleanup, destruction, and reload.
      Not applicable: no persistent index was introduced.
- [x] Record before/after candidate counts and query costs, or explicitly record
      deferral when the measured gain does not justify an additional index.
- [x] Publish lasting type/cache contracts in the appropriate Runtime and Editor
      documentation, then complete this plan with validation evidence.

Acceptance: the indexing decision has evidence; if implemented, first-query
candidate enumeration excludes unrelated classes. All required earlier stages
remain independently correct without this optional optimization.

## Stage 0 Evidence and Selected Contracts

Audit scope: all registered module directories and native test roots in
`Engine/Engine.dproject`, `Sandbox/Sandbox.dproject`, and
`RoadWeaver/RoadWeaver.dproject`. Searches included `FObjectHandle`, its make,
resolve and null helpers, `GetHandle`, and strong/weak/traced pointer types.
Vulkan resource handles and delegate/timer handles are unrelated and excluded.

| Consumer | Responsibility and migration |
| --- | --- |
| CoreDObject `ObjectPtr`, object array | Traced reference representation and slot authority; preserve assignment barrier and collector traversal |
| CoreDObject `WeakObjectPtr` | Deferred access; separate identity storage, preserve game-thread and pending-kill checks |
| CoreDObject `StrongObjectPtr`, graph replacement | Explicit retention counts and exact-generation validation; key the registry without weakening ownership |
| CoreDObject `Archive`, property snapshots; DurinEd transaction records | Snapshot identity plus separately retained references; weak-value restoration must retain stale identity without rooting it |
| Engine material interface, function sources, compiler requests/results, retry queue, graph observers | Identity comparisons and deferred access; keys for identity, weak owners for queued access |
| Engine static-mesh build/compilation/recreation, texture compilation, cooked-mesh loading, asset compiling manager | Deferred owners and result identity; preserve strong task retention separately |
| Engine timer manager | Deferred owner; preserve weak cancellation and callback validation |
| DurinEd property editing/view, MaterialEditor, LevelEditor, StaticMeshEditor, AssetForgeBuiltins | Deferred UI selection/import owners and query results; preserve weak access and explicit import retention |
| Engine native CoreDObject/Engine tests | Slot reuse, weak snapshots, graph replacement, transactions, material/texture/mesh async owners, reload and timers need consumer migration |
| Sandbox | Reflected player-component `TObjectPtr` fields only; no handle identity consumers |
| RoadWeaver | Reflected road/mesh `TObjectPtr` fields and weak mutation-listener owner; no handle identity consumers |

`SetParentAndPropertyOverrides` mutates Parent before compilation invalidation
and render publication. `PostEditChangeProperty` likewise receives the committed
property value. Property editing and transaction replay deliver this hook;
load/duplicate run PostLoad, while reload publishes bindings after graph
replacement. Compilation completion applies the accepted program before calling
MarkRenderDataDirty. These synchronous owners are the scope boundaries; there
is no frame-global cache. Function-caller discovery remains separate.

Selected scope contract:

- Construct an Engine cache context after canonical relationship mutations;
  explicitly pass it through compilation invalidation and render/notification
  preparation. Convenience entrypoints create their own context.
- A context owns a GC deferral scope and lazy candidate/parent tables. Results
  own their pointer array; arrays remain stable across subsequent queries, but
  pointers are usable only while that context lives. No async capture or storage
  outside that batch is permitted. Deferred consumers copy weak references.
- Internal updates complete before external parameter callbacks. Preserve root
  notification first, then slot/generation ordering of remaining admitted
  dependents. A callback edit completes synchronously in a fresh nested context;
  queuing it would change immediate setter semantics. An outer batch may finish
  its prepared notifications, but may not perform relationship discovery after
  callbacks using the old context. Re-querying after a mutation requires a new
  context, even if every generation still matches.
- Physical destruction of registered material objects is performed by the GC
  DestroyObject path in ObjectLifecycle.cpp. CollectGarbage queues requests
  while any FGarbageCollectionDeferralScope exists; the automatic scheduler also
  respects deferral. Nested scopes increment the same counter and exiting a
  scope does not run collection. Replacement and construction-failure cleanup
  mark graphs garbage; they do not synchronously delete published materials.
  Other reviewed deletes destroy reflection properties/registrants, not material
  instances. Direct deletion/removal of registered live objects is outside the
  object lifecycle contract. Query consumption filters pending-kill objects
  again before access; deferral does not prevent MarkAsGarbage.
- Candidate admission remains LiveOnly plus IsValid: exclude templates, private
  package graphs and pending-kill objects. Traversal must retain intermediate
  ancestors even when those ancestors are not admitted results, matching the
  existing parent walk. Include an admitted changed root, deduplicate overlapping
  roots, terminate cycles, and preserve the existing unbounded dependency walk
  (the separate property-resolution depth limit must not truncate dependency
  queries).
- Keys and weak identities are eight-byte slot/generation values. Null is a
  canonical invalid index and zero generation; stale is distinct from null.
  Equality, hashing and ordering use stored identity without resolving; order
  remains index then generation. Pointer capture and resolution are game-thread
  operations (allow bootstrap before game-thread initialization). Independent
  copies may be compared/hashed on workers; concurrent mutation of one value is
  unsupported. Neither type roots objects. Key resolution is explicit; weak
  access excludes pending destruction. Typed forms support forward declarations.
- Preserve existing generation rollover (skip zero). After 2^32-1 reuses of one
  slot an ancient identity can alias; this is an existing bounded-generation
  limitation, not a permanent or serializable identifier guarantee.

Baseline: `MaterialRuntimeTests FMaterialDependencyTests.*` passed its original
six cases on Win64-Debug-DurinEditor (2026-09-16). Existing assertions measure
one query/one snapshot for dynamic publication including notifications, and two
queries/two snapshots for a static/parent property edit (compilation plus
publication). Two independent dynamic roots perform two snapshots; there is no
existing multi-root discovery scope. Direct then dependent query performs two
snapshots. No reverse-table build exists yet. Candidate counters count all
admitted snapshot objects and valid materials, not only returned descendants.
These are operation counts, not wall-clock speedup claims.
## Stage 1 Evidence

Added eight-byte FObjectKey/TObjectKey with centralized identity hash/order and
explicit game-thread resolution. Weak storage now contains FObjectKey, with
temporary GetHandle/SetHandle bridges retained only for staged migration.
The object array supplies identity independently of FObjectHandle; traced object
pointer storage/barriers are unchanged. CoreObjectTests passed 90 cases on
Win64-Debug-DurinEditor, including new non-retention, stale/null, pending-kill
and stable-hash checks plus existing slot-reuse and worker-copy tests.

## Stage 2 Evidence

Migrated Engine, editor and native-test identity consumers to object keys,
including retained snapshot identities, transaction references, replacement
validation, texture/mesh owner qualifiers and timer-world identity. Strong
registration still counts each independent owner. Material compiler requests,
results and retry entries now carry weak owners; retry deduplication uses keys.
Timer callback owners use weak access. Removed weak/pointer GetHandle bridges;
only object-array/pointer representation internals and a deliberate low-level
slot-reuse test retain FObjectHandle. Existing loaded-query algorithms remain
unchanged. Snapshot weak identity serialization preserves stale values and does
not create retention.

Validation on Win64-Debug-DurinEditor: the workspace `all` build (including Sandbox and RoadWeaver), all six reflection-domain targets and all
ten material-domain targets passed. Additional passing targets: CoreObjectTests
(90), AssetPackageReloadTests (13), EditorOperationTests (45), WorldTests (161),
StaticMeshTests (102), TextureTests (110), CookedMeshLoadingTests (2). Lifecycle
tests now distinguish pending-kill access rejection from physical array removal;
the low-level slot-reuse test still checks index reuse and generation advance.

UE reference review (2026-09-16): the public
[FObjectCacheContextScope](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FObjectCacheContextScope)
requires a short interval without object creation or dependency changes and
shares its outer context with nested scopes. Durin instead passes contexts
explicitly and creates a fresh one for reentrant edits, because synchronous
callbacks may change Parent. The public FObjectCacheContext API supports lazy
reverse tables keyed by typed object keys and a multi-material affected query;
this supports the selected interface, not an inference about UE's internal
material inheritance implementation.

## Stage 3 Implementation Notes

Engine now owns FObjectCacheContext and an owning TObjectCacheIterator raw-pointer
range, following the UE public API shape. The context lazily builds one LiveOnly
snapshot and parent-to-children table; intermediate excluded ancestors remain
traversable without becoming admitted results. Query storage is independent of
subsequent queries. Iteration skips pending-kill objects and context lifetime
defers physical collection. Multi-root traversal deduplicates both roots and
cycles and sorts results using object-key ordering.

EndDiscovery seals the context before notifications. Any later query on that
context fails its contract check; prepared results remain protected until scope
exit. Reentrant edits require a fresh context. This formalizes the cache reuse
constraint without changing synchronous callback semantics.

Validation: all nine FMaterialDependencyTests and the workspace all build passed
on Win64-Debug-DurinEditor. New tests compare the independent legacy query
against scoped results, verify one snapshot/table for three queries, owned
result stability, overlapping roots, excluded ancestors, cycles and GC deferral.

## Stage 4 Evidence

Material setters, reflected property hooks, graph edits, immediate compilation
and asynchronous completion pumps explicitly pass the context through discovery
and publication. Compiler requests/results never store it. Standalone query
helpers produce owned key snapshots through a temporary context; production
synchronous loops consume raw-pointer ranges directly. Function-caller discovery
remains separate, while its internal updates now precede parameter callbacks.
Teardown also prepares internal retirement before notification.

Compared with Stage 0, a manual static/parent property edit still performs both
compilation and publication queries but takes one snapshot/table instead of two.
Dynamic parameter publication still needs one snapshot, so no scan reduction is
claimed there. Immediate CompileEdits across a root and two children performs
multiple queries with one snapshot/table. Two separate public dynamic edits are
still separate batches and still take two snapshots; there is no implicit global
cache. A multi-root query deduplicates overlapping roots within one context.

The independent parent-walk oracle remains in tests. Added coverage verifies
synchronous callback reparenting with a fresh batch, root-first prepared outer
notifications, suppression of notifications to pending-kill recipients, deferred
physical GC, and dependency chains beyond the property-resolution depth limit.
All ten material-domain targets passed, covering parent edits, transaction
Edit/Cancel and Undo/Redo, load/duplication, compilation completion and rendering
publication. EditorOperationTests passed 45 cases; AssetPackageReloadTests passed 13 cases and the final workspace all build passed.

## Stage 5 Measurements and Index Decision

The explicit CPU qualification case
`FMaterialQualificationTests.ObjectQueryCacheCandidateCost` passed on
Win64-Debug-DurinEditor. It uses a controlled 32-material chain, one warm-up,
16 recorded samples, and eight repeated queries per context. Times are
microseconds and diagnostic only: they are not production latency guarantees
or a new qualification threshold. No GPU timing is involved.

| Unrelated objects | Snapshot candidates | Materials | Legacy median | Cache cold median | Cache warm median | Cache cold p95 |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 260 | 32 | 573.3 | 374.6 | 132.550 | 1140.3 |
| 2,000 | 2,260 | 32 | 831.5 | 626.6 | 131.863 | 724.9 |
| 20,000 | 20,260 | 32 | 3327.8 | 3042.4 | 123.987 | 4154.3 |

Each context performed nine queries with exactly one snapshot and one parent
build. Cold enumeration still includes unrelated classes; warm queries do not
scan them again. The retained receipt is
`Build/NativeTestResults/Win64-Debug-DurinEditor/ObjectQueryCacheCandidateCost.log`.
Reproduce with `DevTool.bat test MaterialQualificationTests
FMaterialQualificationTests.ObjectQueryCacheCandidateCost --mode qualification
--report`.

Decision: defer the optional CoreDObject class index. At 20,000 unrelated objects
the measured cold cost is approximately 3 ms in Debug, paid once per batch;
subsequent discovery is approximately 0.124 ms. This demonstrates the remaining
linear first-scan cost, but does not establish a production latency budget that
justifies another publication/removal index and its lifecycle consistency work.
Revisit when production traces identify first-query enumeration as a bottleneck.
No typed lookup, registration hook or persistent material relationship index was
added. The conditional indexing and switch-over tasks are therefore not
applicable to this implementation.

Lasting contracts now live in [scoped material queries](../../../Runtime/Rendering/MaterialQueries.md),
[object lifetime](../../../Runtime/Core/GarbageCollection.md#pointer-and-handle-semantics),
and [editor lifecycle](../../../Editor/Architecture/MaterialEditorLifecycle.md).
The material-system query section links to its new focused owner rather than
expanding an already long general specification. Transaction, timer and asset
compilation documents now distinguish object keys from deferred weak access.

Final cumulative affected-target validation also exposed a previously implicit
`TObjectPtr` include in Level.h after weak-pointer headers stopped importing
ObjectPtr.h. Level.h now includes its actual value-type dependency explicitly.

The cumulative affected run passed 61 of 62 targets. SceneImportVulkanTests
exposed a physical-lifetime assertion still using filtered key access and an
unrelated stale normal-output expectation (8 instead of the existing importer
channel 1). Both assertions were corrected; the complete target then passed.
The cumulative receipt is `Build/.agent-state/logs/20260916-160738-234756-24776-ctest.log`;
the successful follow-up is `Build/.agent-state/logs/20260916-161051-696031-21244-SceneImportVulkanTests.log`.
The final workspace `all` build passed after these fixes:
`Build/.agent-state/logs/20260916-161136-791965-32956-cmake.log`.

## Validation and Handoff

Follow [agent testing](../../../Agents/Testing.md),
[agent build and run](../../../Agents/BuildAndRun.md), and
[documentation workflow](../../../Agents/Documentation.md).
Shared API changes require workspace-wide consumer searches, affected project
validation, and an `all` build for shared Engine API migrations. Prefer focused
behavior tests over tests that merely reproduce container implementation.

Each stage must remain buildable and be committed independently with updated
status, checklists, and exact plan/stage trailers. Record relevant test/build
receipts and measured counters with the stage evidence. Planning alone does not
complete Stage 0 or imply any runtime validation has occurred.

## Deferred Object-Handle Encoding Work

After consumer migration, separately evaluate whether object pointers need a
resolved-pointer fast path, unresolved references, or access tracking. Select
required capabilities before changing representation. Such work needs a new
bounded stage or plan with GC, reflection, serialization, duplication, reload,
and lifetime qualification; it is not required to complete this query cache.

## UE Reference Boundary

The following public APIs inform responsibility and interface choices; they do
not prove the implementation of undocumented UE query functions:

- [FObjectCacheContext](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FObjectCacheContext):
  lazy query caches and domain-specific reverse lookup tables.
- [TObjectCacheIterator](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/TObjectCacheIterator):
  wraps raw-pointer arrays/views and exposes raw-pointer iteration.
- [Object pointers](https://dev.epicgames.com/documentation/unreal-engine/object-pointers-in-unreal-engine):
  traced, weak, and explicit strong reference responsibilities.
- [FObjectKey](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/CoreUObject/FObjectKey):
  object identity keys independent of ordinary pointer access.

Do not infer a permanent UE parent-to-child material index from the existence
of `GetMaterialsAffectedByMaterials`; Durin's temporary table is a selected
local implementation decision.
