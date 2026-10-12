# Derived Data Cache Optimization Plan

Summary: Harden cache-hit validation and inline dispatch, then simplify DDC ownership, eliminate repeated work, and add maintainable storage and backend composition.

Last reviewed: 2026-10-13

Status: Active
Completed:

## Current Status

Stages 0 through 2 are complete with the recorded integration-validation exception
below; Stage 3A is complete, Stage 3B is next, and subsequent stages have not
started. The 2026-10-13
architecture review revised Stage 3 to simplify dispatch policy and public
integration before adding further infrastructure. Stage 2 completion records
validated behavior, not a requirement to retain every implementation mechanism.
This plan continues the completed
[Modern Derived Data Cache plan](ModernDerivedDataCache.md) without reopening its
record/value format or asset-publication decisions.

Stage 1 evidence (Win64-Debug-DurinEditor, 2026-10-13):

- DerivedDataBuildTests and DerivedDataCacheTests passed in full, including wrong
  type/version positive and negative hits, cache-only rejection, 4096-item inline
  Get/Put dispatch depth, same-cache nested submission, and nested inline builds.
- The affected selection built all five targets. AsyncTaskPilotLifecycleTests and
  RenderShaderCacheTests passed; RenderShaderBuilderTests passed 22 of 23 cases.
- Validation exception: ShaderBuilderTests.cpp's
  ErrorTerminalRecordsCompleteColdAndWarmBaseline expects eight source dependencies
  but finds nine before reaching DDC build execution. The identical case failed
  with both changed DDC implementation files temporarily restored from HEAD;
  final sources were restored and the affected selection rerun with identical
  results. This existing fixture failure remains outside Stage 1 scope.
- Final affected report: `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.
  Baseline reproduction log:
  `Build/.agent-state/logs/20261013-033612-746999-35132-RenderShaderBuilderTests.log`.
- The restricted build encountered MSVC C1902; authorized elevated launcher
  execution compiled successfully. Documentation and all-plan validation passed.

Stage 2 evidence (Win64-Debug-DurinEditor, 2026-10-13):

- Migrated CreateSession and CreateAsyncCache consumers across Engine source/tests;
  the declared Sandbox and RoadWeaver source/test roots have no affected callers.
  The all build passed after the final shared API changes.
- Full DerivedDataBuildTests passed, including shared cross-session concurrency,
  priority/capacity, inline independence under task saturation, owner/session/service
  cancellation, retained-byte recovery, injected-cache lifetime, and single-worker
  cooperative waiting. All six new service cases also passed in isolated processes.
- DerivedDataCacheTests passed (56 passed, one existing platform skip), including
  byte/item atomic rejection, underlying allocation charging, per-record backing
  reuse, copied selection charges, callback retirement, cancellation, and priority
  starvation protection. Existing inline depth/nested and lifecycle tests remain green.
- TextureTests passed all 142 cases; StaticMeshTests passed all 169 cases.
  AsyncTaskPilotLifecycleTests and RenderShaderCacheTests passed. Shader Builder
  retains only the Stage 1 dependency-count baseline failure (22/23 passed).
- Bounded module selection was used after affected analysis expanded mechanical
  Engine call-site changes into unrelated engine/rendering targets. Reports are
  `Build/NativeTestResults/Win64-Debug-DurinEditor/module-derived-data-cache.xml`,
  `DerivedDataBuildTests.xml` (isolated new cases), `DerivedDataCacheTests.xml`,
  `TextureTests.xml`, and `StaticMeshTests.xml` in that same report directory.
- Build evidence: `Build/.agent-state/logs/20261013-040522-140247-70836-cmake.log`.
  Final module test log:
  `Build/.agent-state/logs/20261013-040544-608808-76332-ctest.log`.
  Documentation validation passed. The existing Shader fixture exception does not
  introduce a new DDC regression and remains outside this stage's scope.

Stage 3A evidence (Win64-Debug-DurinEditor, 2026-10-13):

- Removed service CacheScheduler overrides and the public priority bypass knob;
  scheduler test control uses an injected IAsyncCache. Kept explicit inline/task
  completion requirements and synchronous backend ownership behind service dispatch.
- Queued data accounting now omits request/table object sizes and container spare
  capacities, retaining copied selections, schema/messages, and distinct retained
  payload/metadata allocations. One admission calculation owns these charges.
- Admission timestamps replace per-item bypass updates. A maximum-capacity queue
  regression fills all 4096 slots, sustains high-priority arrivals, and confirms
  oldest progress after 32 waiting turns. For a 4096-item FIFO batch, the removed
  counter loop previously visited 8,386,560 pending items; the new policy updates
  one service turn per dispatch and checks one oldest timestamp. This is a
  deterministic bookkeeping comparison, not a wall-clock performance claim.
- DerivedDataCacheTests: 58 passed, one existing symlink platform skip.
  DerivedDataBuildTests: all 78 passed; three migrated service cases also passed
  in isolated processes. Added exact metadata/message budget and dispatch-rejection
  recovery coverage. Existing mutable-priority, callback-retirement, cancellation,
  schema, and nested inline regressions remain passing.
- The all build passed. The five-target DDC module selection passed except for the
  already recorded Shader dependency-count fixture (22/23 Shader Builder cases).
  Build log: `Build/.agent-state/logs/20261013-043730-135398-63944-cmake.log`.
  Module log: `Build/.agent-state/logs/20261013-043752-522377-27620-ctest.log`.
  Isolated service log: `Build/.agent-state/logs/20261013-043812-774228-72668-ctest.log`.
  Reports remain in `Build/NativeTestResults/Win64-Debug-DurinEditor/`.

## Goal

Make local DDC correct under rejected hits, bounded under batch dispatch, and
easy to integrate and maintain before adding distributed capabilities. Preserve
canonical action identity, immutable captured inputs, deferred resolution on a
miss, deterministic negative caching, and successful local results on store failure.

## Architecture Boundaries

- DerivedDataCache owns generic definition/action identity, registered function
  descriptors, request lifetime, output envelope validation, cache dispatch,
  record/value persistence, backend policy, and storage maintenance.
- Core owns task execution and cooperative waits. DDC borrows Core schedulers;
  it must not introduce another worker pool or inherit incidental task scopes.
- Engine and ShaderBuild own captured source state, resource admission and
  reservations, typed semantic validation, business-error translation, latest-wins
  checks, and publication. Pure recipe modules retain transformation ownership.
- Generic record validation and family semantic validation are distinct trust
  boundaries. Neither replaces the other. Performance improvements must preserve
  integrity checks when bytes enter through storage.
- Build completion, callback/capture retirement, and persistence retirement must
  have explicit lifetime rules. A result becoming usable does not release work
  still tracked by its owner.

Implemented contracts remain authoritative in
[build protocol](../Runtime/Assets/DerivedDataBuild.md) and
[cache storage](../Runtime/Assets/DerivedDataCacheStorage.md). Update those contracts
when each stage lands; this plan does not specify already implemented behavior.

## Implementation Stages

### Stage 0: Select scope and preserve contracts

Dependencies: initial architecture review.

Outcome: an ordered optimization path with bounded responsibilities.

- [x] Keep current action hashes and persisted namespaces for Stage 1.
- [x] Select schema matching and inline batch recursion as the first fixes.
- [x] Preserve synchronous nested inline submissions and exactly-once completion.
- [x] Keep asset publication and producer memory reservations outside DDC.
- [x] Defer remote execution, transitive build graphs, and range reads until a
  concrete consumer justifies them.

Acceptance: every implementation stage below identifies ownership, dependencies,
observable acceptance gates, and validation. No infrastructure rewrite is needed
to land the first fixes.

### Stage 1: Validate warm schemas and bound inline batch dispatch

Dependencies: Stage 0.

Boundary: Session compares cached output schema/type and version with the prepared
action's function descriptor. Cache codecs still validate bytes and generic tables;
family assemblers still validate product semantics. Inline queue advancement belongs
to the existing pump, while explicit nested submissions may pump synchronously.

- [x] Reject a structurally valid hit with the wrong output type or version before
  reporting CacheQueryHit; fall back once when local build is enabled.
- [x] Keep cache-only schema rejection an error without invoking the producer or Put.
- [x] Apply schema matching to deterministic negative hits as well as positive hits.
- [x] Remove completion-driven recursion for ordinary inline batches.
- [x] Add regression coverage and update implemented contracts.

Acceptance: valid positive and negative hits skip local production; wrong type and
version rebuild exactly once or fail cache-only; no rejected hit reports
CacheQueryHit. Maximum-capacity inline Get and Put batches have constant scheduler
nesting depth and exactly one callback per index. Same-cache nested submissions,
nested inline builds, cancellation, dispatch rejection, and owner retirement remain
covered. Run DerivedDataBuildTests and DerivedDataCacheTests, followed by affected
CPU integration coverage selected through the test registry.
Existing integration failures require reproduction against unchanged HEAD DDC
implementation and explicit evidence; this stage permits no newly introduced
failures. The recorded Shader manifest fixture failure is the sole exception.

### Stage 2: Centralize production cache dispatch and backpressure

Dependencies: Stage 1.

Boundary: the production cache service owns shared I/O admission and queues; sessions
retain their input resolver, registry snapshot, and producer scheduler. Explicit
injected caches and inline compatibility remain supported.

Implemented Stage 2 decisions (mechanisms may be simplified in Stage 3): cache scheduler and dispatch limits are service
configuration; sessions explicitly select Inline (default) or Task cache execution.
All task sessions share one dispatcher. Inline sessions share a separate bounded
dispatcher over the same backend and do not wait on task admission or execute
queued task work. Limits account for retained request structures, copied selection
IDs, and distinct Put payload/metadata backing capacities within each record;
cross-request shared allocations are charged conservatively for each request.
Opaque callable captures, allocator overhead, and active backend/codec workspace
are excluded; per-request workspace budgets remain separate. Queue priority uses
a configurable bypass threshold followed by oldest protected request selection,
bounding starvation by threshold plus configured capacity in dispatch turns.
Injected IAsyncCache remains caller-configured and is not closed globally by the
build service. Closing a session cancels only that session's requests.

- [x] Replace default task-session-local cache queues with service-owned dispatch.
- [x] Define service-wide concurrency, queue capacity, retained-byte admission,
  priority fairness, and cancellation ownership.
- [x] Specify how inline and task callers share storage without changing inline
  completion or introducing cooperative sibling-asset execution.
- [x] Keep capacity rejection distinct from backend failure.

Acceptance: multiple sessions obey one configured task-I/O limit and byte admission
limit; cross-session priority has a starvation bound; canceling one owner does not
cancel another. Overflow rejects atomically without callbacks. Single-worker waits,
shutdown, injected services, and inline compatibility pass deterministic tests.

### Stage 3: Simplify cache policy and resource session integration

Dependencies: Stage 2.

Boundary: the cache service owns dispatch, queue budgets, and fairness policy;
resource callers select their required synchronous or asynchronous completion
contract without configuring cache scheduling algorithms. Module-owned integration
holds sessions and registration lifetime;
Texture, StaticMesh, Physics, and Shader keep capture, recipes, semantic assembly,
reservations, and publication. Shader compiler scheduling/LRU remain ShaderBuild-owned.

Selected revision: adopt UE's concentrated cache/request ownership as a boundary
reference, not its full feature set. Preserve service-wide task admission and
correct request retirement. Treat inline dispatch as a compatibility adapter with
its existing immediate, nested completion contract; a generic asynchronous
submit-and-Wait wrapper is not an acceptable replacement. Do not expose fairness
algorithm parameters merely to make tests configurable. Keep one explicit cache
dependency injection seam and remove redundant production configuration only
after auditing consumers across the workspace.

Execute 3A before 3B; each step must be independently reviewable and validated.

3A: simplify Stage 2 mechanisms without weakening its behavioral guarantees.

- [x] Replace detailed object/container accounting with a documented coarse
  queued-request budget for retained payload/metadata backing and variable-size
  copied request data, alongside request-count admission. Preserve protection
  against oversized selections and overflow; do not describe this as total DDC
  memory accounting. Keep producer reservations and codec workspace limits under
  their existing owners. Centralize the accounting calculation and retain buffer
  lifetime coverage through callback retirement.
- [x] Make fairness an internal bounded policy; remove the public bypass tuning
  knob and the per-dispatch update of every waiting request's bypass counter.
  Preserve cross-session priority, live owner priority changes, and a documented
  starvation bound. Prefer the smallest policy that satisfies deterministic tests.
- [x] Audit AsyncCache injection, CacheScheduler, dispatch limits, and session
  execution selection; reduce overlapping configuration and keep test control at
  the cache/scheduler boundary. Keep service concurrency and admission limits
  explicit, with no new execution modes or generic scheduling framework.
- [x] Keep synchronous backend details and inline compatibility behind the cache
  integration boundary where practical. Avoid an unconditional public ICache API
  rewrite solely to resemble UE; any remaining split must have a stated owner.
- [x] Update runtime contracts and migrate every affected workspace consumer.

3B: consolidate resource integration only after 3A passes.

- [ ] Consolidate duplicated session registration, submission, and shutdown/drain
  glue using existing ownership facilities; introduce a small lifecycle holder
  only when concrete duplicated behavior requires it.
- [ ] Reuse submission logic while preserving immediate inline and nested
  synchronous completion; keep asynchronous task admission independent.
- [ ] Preserve typed admission errors instead of collapsing them to bool/nullopt.
- [ ] Move test injection to scoped dependencies and drain outside holder locks.
- [ ] Decide and document Shader's isolated compiler/service lifetime explicitly.

Acceptance for 3A: multiple task sessions still share one concurrency and admission
domain; inline completion is independent of task saturation and never executes
sibling asset work to make progress. Item and byte rejection remain atomic and
distinct from backend failure, with no callback or owner registration on rejection.
Budgets cover retained buffers through callbacks and recover after cancellation,
completion, and dispatch failure. Priority updates take effect and sustained high
priority arrivals cannot starve older work under the documented bound. Canceling
one session does not cancel another, injected caches retain caller ownership, and
single-worker shutdown/drain remains live. Existing Stage 1 schema and nested
inline regressions remain passing. Validate DerivedDataCacheTests,
DerivedDataBuildTests, and affected integration targets; measure admission and
dispatch behavior on a maximum-capacity queue to confirm reduced accounting and
bookkeeping rather than merely moving the same complexity behind another helper.

Acceptance for 3B: producer services outlive callbacks and unload only after drain;
registration snapshots remain predictable. Reentrant callbacks and shutdown do not
deadlock. Texture2D/Cube/Volume, mesh render, collision, and Shader cold/warm results
remain equivalent. Validate affected consumers; any shared Engine API migration
requires an all build under repository rules. Stage 3 completes only after both
3A and 3B pass; do not add DAGs, remote workers, new cache tiers, or storage-format
changes in this stage. The recorded Shader manifest fixture exception remains
the only permitted existing integration failure.

### Stage 4: Reuse blob work and separate result delivery from persistence

Dependencies: Stages 2 and 3.

Boundary: cache owns content reuse; resource owners control when usable results are
published. Persistence remains owned work through shutdown, including buffer lifetime.

- [ ] Deduplicate requested blobs by versioned identity and raw size, sharing
  immutable decoded backing across value IDs; keep physical/logical budgets explicit.
- [ ] Reuse valid stored content and retain atomic replacement for damaged blobs.
- [ ] Add bounded in-flight read sharing only after request-local reuse is measured;
  separate waiter cancellation from shared work cancellation.
- [ ] Define opt-in early result delivery and a separate persistence-retirement
  contract, retaining a mode that waits for persistence.
- [ ] Remove whole-record codecs from production API after auditing every workspace
  consumer; retain necessary fixture support privately.

Acceptance: repeated identities perform one physical read/decode per request and
share backing; valid repeated writes avoid redundant compression/publication.
Damaged content is repaired and failed writes preserve existing manifests. A delayed
Put does not delay opted-in result delivery; owner drain still joins persistence
and captures. Store failure cannot invalidate a delivered result. Cold/warm byte
equivalence, negative hits, budgets, and cancellation pass. Record before/after
physical I/O, codec work, delivery latency, and retained-memory measurements.

### Stage 5: Add observability and safe storage maintenance

Dependencies: Stage 4.

Boundary: diagnostics and maintenance are cache services, separate from persisted
recipe metadata and business completion status. Collection owns the complete
publication/read lifetime protocol.

- [ ] Add bounded per-bucket/backend hit, rejection, fallback, queue, codec, store
  failure, latency, and storage-usage counters without affecting keys or records.
- [ ] Provide offline inspect/verify/clear/reclaim tooling with exclusive ownership.
- [ ] Select publication epochs or leases before implementing online reclamation;
  include retained readers, blob-before-manifest writers, and legacy namespaces.
- [ ] Define quota/age policy, cancellation, crash recovery, and bounded maintenance I/O.

Acceptance: diagnostics distinguish normal miss, invalid record, backend failure,
and capacity rejection. Offline maintenance refuses active storage. Online collection
cannot delete blobs belonging to in-progress publications or retained readers;
fault-injection and concurrent reader/writer/collector tests prove the selected
protocol. Quotas converge after readers retire, and maintenance is restartable.

### Stage 6: Compose local and shared cache backends

Dependencies: Stages 2, 4, and 5.

Boundary: backend composition owns routing, query/store policy, promotion, health,
and maintenance configuration; build functions and asset families stay backend-neutral.

- [ ] Add explicit local/shared query and store controls with per-value overrides
  only where a consumer needs them; preserve full-output Build semantics.
- [ ] Implement fast-to-slow lookup and bounded promotion into faster stores.
- [ ] Version cryptographic blob identity before admitting untrusted shared storage;
  verify raw content and define action/namespace isolation independently.
- [ ] Add timeout/degraded-backend policy, authentication configuration, read-only
  seed/prewarm support, and local-only operation.

Acceptance: shared hits promote safely; shared outages fall back within a configured
deadline; policy prevents forbidden remote reads/writes. Local-only operation needs
no network service. Unsupported identity versions do not alias existing blobs;
tampered remote content is rejected. Concurrent writers and interrupted promotion
preserve readable records. End-to-end asset results remain backend-independent.

### Stage 7: Qualify consumer-driven extensions

Dependencies: Stage 6 for distributed work; earlier stages may supply measurements.

Boundary: graph execution and worker protocol belong to DDC only when selected;
source capture and publication stay with producers. Chunk reads belong to cache
storage and require a concrete partial-data consumer.

- [ ] Measure representative editor/cook workloads and record which remaining costs
  justify transitive build dependencies, remote workers, or value-range reads.
- [ ] For each selected extension, define a separate implementation plan with
  identity/versioning, cancellation, memory, failure, and deterministic-result gates.
- [ ] Explicitly defer extensions without measured consumers; do not introduce
  placeholder interfaces merely to mirror UE.

Acceptance: each extension has either a linked executable plan or an evidence-backed
deferral. This stage does not require implementing all UE capabilities.

## Validation and Handoff

Follow [agent testing](../Agents/Testing.md),
[build workflow](../Agents/BuildAndRun.md), and
[documentation validation](../Agents/Documentation.md). Run lifecycle plan validation
when adding this plan. Keep evidence and passed checklists current in each stage
commit, with exact Plan and Stage trailers. Subsequent stages remain open until
their acceptance gates are met; passing Stage 1 does not complete this plan.
