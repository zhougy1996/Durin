# Derived Data Cache Optimization Plan

Summary: Harden cache-hit validation and inline dispatch, then simplify DDC ownership, eliminate repeated work, and add maintainable storage and backend composition.

Last reviewed: 2026-10-13

Status: Active
Completed:

## Current Status

Stages 0 and 1 are complete with the recorded integration-validation exception
below; subsequent stages have not started. This plan continues the completed
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

- [ ] Replace default task-session-local cache queues with service-owned dispatch.
- [ ] Define service-wide concurrency, queue capacity, retained-byte admission,
  priority fairness, and cancellation ownership.
- [ ] Specify how inline and task callers share storage without changing inline
  completion or introducing cooperative sibling-asset execution.
- [ ] Keep capacity rejection distinct from backend failure.

Acceptance: multiple sessions obey one configured task-I/O limit and byte admission
limit; cross-session priority has a starvation bound; canceling one owner does not
cancel another. Overflow rejects atomically without callbacks. Single-worker waits,
shutdown, injected services, and inline compatibility pass deterministic tests.

### Stage 3: Consolidate resource session integration

Dependencies: Stage 2.

Boundary: module-owned integration holds sessions and registration lifetime;
Texture, StaticMesh, Physics, and Shader keep capture, recipes, semantic assembly,
reservations, and publication. Shader compiler scheduling/LRU remain ShaderBuild-owned.

- [ ] Introduce a small lifecycle holder with explicit registration, submission,
  and shutdown/drain ordering rather than duplicated global session glue.
- [ ] Reuse submission logic for synchronous wrappers through a wait adapter that
  preserves existing inline scheduling requirements.
- [ ] Preserve typed admission errors instead of collapsing them to bool/nullopt.
- [ ] Move test injection to scoped dependencies and drain outside holder locks.
- [ ] Decide and document Shader's isolated compiler/service lifetime explicitly.

Acceptance: producer services outlive callbacks and unload only after drain;
registration snapshots remain predictable. Reentrant callbacks and shutdown do not
deadlock. Texture2D/Cube/Volume, mesh render, collision, and Shader cold/warm results
remain equivalent. Validate affected consumers; any shared Engine API migration
requires an all build under repository rules.

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
