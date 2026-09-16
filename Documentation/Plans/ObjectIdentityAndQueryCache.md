# Object Identity and Query Cache Plan

Summary: Separate object reference, weak reference, and identity-key responsibilities, then introduce a scoped material query cache with UE-style raw-pointer iteration.

Last reviewed: 2026-09-16

Status: Active
Completed:

## Current Status

Planning only; no implementation stages have started. Stage 0 is next.
The selected order is contract definition, object identity types, consumer
migration, scoped queries, update-batch integration, and measurement-driven
class indexing. Changing the internal encoding of `FObjectHandle` is a separate,
deferred decision rather than a prerequisite for this cache.

Current source inspection established:

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

- [ ] Inventory handle and pointer consumers across every source and test root
      declared by `Durin.dworkspace`; classify identity, deferred access,
      traced reference, and explicit strong ownership separately.
- [ ] Trace actual material setter, property edit, transaction, load, reload,
      compilation completion, and notification paths to select the update-batch
      owner and locate reentrant mutations.
- [ ] Define scope lifetime, borrowed versus owned result storage, nested scope
      behavior, and cache invalidation boundaries.
- [ ] Prove how physical destruction is excluded during pointer consumption;
      evaluate the existing GC deferral scope and any other teardown paths.
      Define handling for objects marked pending destruction within the scope.
- [ ] Define weak/key null, stale, equality, hash, ordering, and thread contracts;
      preserve generation wrap behavior or explicitly resolve its limitations.
- [ ] Record scan/build counts for representative single-root and multi-root
      update batches, separating existing snapshot reuse from repeated work.

Acceptance: a reviewed contract and concrete consumer inventory are recorded
here; no lifecycle safety assumption remains implicit. Baseline evidence does
not claim gains that the current batching already provides.

### Stage 1: Introduce object keys and separate weak identity

Dependencies: Stage 0.

- [ ] Add `FObjectKey` and typed keys with centralized identity hashing and
      comparison; define explicit conversion and resolution APIs.
- [ ] Move weak-reference identity storage behind the weak/key implementation
      boundary so it no longer depends on `FObjectHandle`'s public encoding.
      Continue using the same object array and generation mechanism.
- [ ] Preserve game-thread resolution, pending-destruction filtering, GC
      non-retention, forward-declared type support, and intended value sizes.
- [ ] Migrate directly affected CoreDObject interfaces without introducing
      implicit conversions that obscure strong versus weak ownership.
- [ ] Validate slot reuse, stale versus null identity, hash stability after
      destruction, weak access filtering, and non-retention through GC.

Acceptance: weak references and identity keys have independent contracts;
object-pointer behavior and lifetime semantics remain unchanged.

### Stage 2: Migrate identity and deferred-access consumers

Dependencies: Stage 1.

- [ ] Replace business-level handle keys, comparisons, deduplication, and
      sorting with object-key operations.
- [ ] Replace deferred object access with weak references where appropriate,
      including material retry queues and asynchronous result ownership checks.
- [ ] Audit strong-reference registration and graph replacement individually;
      preserve explicit retention counts and replacement validation.
- [ ] Remove obsolete public identity helpers and field access after migrating
      all consumers; do not mechanically replace every handle with a weak pointer.
- [ ] Keep loaded-query algorithms unchanged for this stage; verify stale async
      results, object replacement/reload, and strong retention behavior.

Acceptance: external identity consumers no longer depend on handle encoding;
all affected workspace projects compile and focused behavior checks pass.

### Stage 3: Implement the scoped material query cache

Dependencies: Stages 0 through 2.

- [ ] Add Engine-owned `FObjectCacheContext` with lazy loaded-material collection
      and a temporary parent-to-direct-children table.
- [ ] Add a result range/iterator exposing raw material pointers, with explicit
      storage lifetime and invalidation rules from Stage 0.
- [ ] Implement direct-child and affected-subtree queries, including self,
      overlapping roots, deterministic ordering, and malformed parent chains.
- [ ] Initially use the existing global snapshot for candidate discovery; make
      first-build cost and repeated-query reuse observable through counters.
- [ ] Validate result equivalence against the current query behavior and prove
      that repeated queries in one context build each required cache only once.

Acceptance: query results preserve semantics and lifetime safety; no claim is
made that the initial global object scan has been eliminated.

### Stage 4: Share the cache across material update batches

Dependencies: Stage 3.

- [ ] Have the outer update owner create the context and explicitly share it
      across parameter, compilation, rendering, and notification preparation.
- [ ] Preserve parameter snapshots, dirty flags, revisions, event ordering, and
      required immediate updates; discovery optimization cannot omit propagation.
- [ ] Implement the selected callback/reentrancy policy with fresh discovery for
      relationship changes; prevent raw results from escaping into queued work.
- [ ] Validate reparenting, Edit/Cancel, Undo/Redo, loading, duplication, reload,
      destruction, overlapping changed roots, and edits made by callbacks.
- [ ] Compare scan/build counts with Stage 0 and record observed savings;
      retire redundant scans only after equivalent behavior is established.

Acceptance: each synchronous batch shares discovery, callbacks cannot consume
stale relationships, and existing material behavior remains intact.

### Stage 5: Evaluate and optionally add class indexing

Dependencies: Stage 4 measurements.

- [ ] Measure remaining first-query cost with many unrelated objects and a
      controlled number of materials; record whether class indexing is justified.
- [ ] If justified, add CoreDObject class-based candidate lookup, including
      derived classes and existing publication/template/garbage admission rules.
      Integrate registration/removal without a second object lifetime authority.
- [ ] Switch cache candidate collection to the typed lookup and validate index
      consistency through construction, failure cleanup, destruction, and reload.
- [ ] Record before/after candidate counts and query costs, or explicitly record
      deferral when the measured gain does not justify an additional index.
- [ ] Publish lasting type/cache contracts in the appropriate Runtime and Editor
      documentation, then complete this plan with validation evidence.

Acceptance: the indexing decision has evidence; if implemented, first-query
candidate enumeration excludes unrelated classes. All required earlier stages
remain independently correct without this optional optimization.

## Validation and Handoff

Follow [agent testing](../Agents/Testing.md),
[agent build and run](../Agents/BuildAndRun.md), and
[documentation workflow](../Agents/Documentation.md).
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
