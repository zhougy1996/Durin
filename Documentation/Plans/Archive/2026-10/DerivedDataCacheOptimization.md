# Derived Data Cache Optimization Plan

Summary: Harden cache-hit validation and inline dispatch, then simplify DDC ownership, eliminate repeated work, and add bounded diagnostics and offline local-storage maintenance.

Last reviewed: 2026-10-18

Status: Archived
Completed: 2026-10-18

## Current Status

Stages 0 through 5 are complete, including the reduced Stage 5 diagnostics and
offline maintenance scope. Shared backends, online collection, and further
execution/storage extensions are explicitly deferred, not remaining acceptance
gates. This local-cache optimization plan is complete as of 2026-10-18. The historical Shader dependency-manifest fixture exception recorded
below was fixed and its current tests pass in the 2026-10-18 follow-up. The 2026-10-13
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

Stage 3B evidence (Win64-Debug-DurinEditor, 2026-10-13):

- Texture, StaticMesh, and physics now share the private Engine
  FResourceBuildSessions holder for acquisition, synchronous submission, and
  close/drain. Producer-module shutdown drains Texture/Mesh sessions; Engine
  shutdown drains physics sessions. Registration remains at its existing family
  boundary and session snapshots remain immutable.
- Texture wrappers return typed admission errors; resource adapters preserve their
  diagnostic at business translation, and Shader admission failure retains its
  diagnostic. Shader keeps its independent compiler/service lifetime.
- Injected test services register functions before replacement and drain retired
  dependencies outside holder locks. Existing fixture owner-drain guards retain
  scoped restoration; no additional public test-guard API was introduced.
- DerivedDataCacheTests: 61 passed, one existing symlink platform skip. All three
  new holder cases also passed in isolated processes: nested inline completion and
  typed rejection across reopen, reentrant close followed by external drain, and
  service replacement with callback acquisition during drain.
- DerivedDataBuildTests: all 78 passed. TextureTests: all 142 passed.
  StaticMeshTests: all 169 passed, including collision cold/warm integration.
  AsyncTaskPilotLifecycleTests and RenderShaderCacheTests passed. Shader Builder
  retains only the recorded dependency-count fixture failure (22/23 passed).
- Workspace consumer audit found no affected Sandbox/RoadWeaver callers. The final
  all build passed; affected analysis expands the shared Engine integration changes
  to unrelated rendering/GPU targets, so validation uses the DDC module set plus
  TextureTests and StaticMeshTests rather than executing unrelated GPU coverage.
- Final build: `Build/.agent-state/logs/20261013-044942-875202-6028-cmake.log`.
  Module: `Build/.agent-state/logs/20261013-045008-365614-70052-ctest.log`.
  Isolated holder cases: `Build/.agent-state/logs/20261013-044927-294893-66284-ctest.log`.
  Texture: `Build/.agent-state/logs/20261013-044531-947087-67352-TextureTests.log`.
  Mesh: `Build/.agent-state/logs/20261013-044844-959776-78696-StaticMeshTests.log`.
  Reports remain in `Build/NativeTestResults/Win64-Debug-DurinEditor/`.

Shader fixture follow-up (MacOS-arm64-Debug-DurinEditor, 2026-10-18):

- The historical failure expected eight dependencies although the shader also
  imports `Material/SpecularAntialiasing`, yielding nine. Commit `891c7c3ae`
  replaced the stale count assertion with the explicit expected dependency paths.
- At revision `e6d758a854461c8853e3464bf5bfe06ede2c8609`,
  `FShaderBuilderTests.ErrorTerminalRecordsCompleteColdAndWarmBaseline` passed
  alone, recording nine dependencies and completing its cold/warm assertions.
  The full RenderShaderBuilderTests target then passed all 33 current cases.
- Reproduction: `./DevTool test RenderShaderBuilderTests
  FShaderBuilderTests.ErrorTerminalRecordsCompleteColdAndWarmBaseline --report`,
  followed by `./DevTool test RenderShaderBuilderTests --report`.
  Report: `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/RenderShaderBuilderTests.xml`.
  Isolated-case log: `Build/.agent-state/logs/20261018-162124-112993-70919-RenderShaderBuilderTests.log`.
  Full-target log: `Build/.agent-state/logs/20261018-162133-548310-71189-RenderShaderBuilderTests.log`.
  This follow-up does not rerun or replace the historical Win64 integration coverage.

Stage 4 evidence (MacOS-arm64-Debug-DurinEditor, 2026-10-18):

- Request-local version/hash/raw-size reuse shares immutable decoded backing across
  IDs, including empty and selected values. Logical limits still count every output
  value; physical encoded/workspace limits count distinct content. Put verifies raw
  content before reuse and preserves atomic repair/publication on corruption or
  failure. Regression coverage includes a valid envelope with damaged raw bytes.
- Existing encoded-read coalescing now bounds retained flights and joined waiters,
  permits independent overflow, and separates caller cancellation from shared work.
  Canceling either a leader or a waiter does not cancel another caller's result.
- Opt-in early delivery preserves exactly-once callbacks, positive/negative output,
  inline behavior, reentrant close/drain, and single-worker progress. Pending Put
  stays owned after delivery; subsequent cancellation/failure cannot revoke output.
  Explicit callback extraction avoids relying on moved-from `std::function` state.
- Audited source/test roots across Engine, Sandbox, and RoadWeaver. No production
  whole-record codec consumer remains; five fixture source files use the private
  codec boundary. Public policy/request layout changes received an `all` build.
- The five-target `@module=derived-data-cache` set passed, including Build/Cache,
  Shader Builder/Cache, and AsyncTaskPilot lifecycle tests. Five focused Build cases
  and six Cache cases also passed in separate processes. An existing owner-dependency
  fixture declared its owner before events captured by cancellation callbacks;
  moving the owner after those captures fixes teardown access to destroyed mutexes.
- TextureTests passed all 150 cases; StaticMeshTests passed all 186. Reports are in
  `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/`; isolated receipts are
  `Build/Stage4/build-isolated.xml` and `Build/Stage4/cache-isolated.xml`.
- Final all-build log: `Build/.agent-state/logs/20261018-164718-807317-80841-cmake.log`.
  Final module log: `Build/.agent-state/logs/20261018-164642-847044-80780-ctest.log`.
  Texture log: `Build/.agent-state/logs/20261018-164416-323303-79693-TextureTests.log`.
  Mesh log: `Build/.agent-state/logs/20261018-164440-596652-79988-StaticMeshTests.log`.
  The tested source/test tree is base revision `149bffa4c` plus the Stage 4 changes
  committed with this evidence; subsequent evidence edits change documentation only.
  Validation is native macOS CPU coverage; Windows and GPU qualification were not run.

Stage 4 measurements use a fresh fixture with 64 IDs sharing one 8 KiB compressible
allocation. The before run restores `149bffa4c`'s record-cache implementation with
store-statistics instrumentation only; the after run uses Stage 4 reuse. The local
reproduction script is `Build/Stage4/measure_baseline.py`, and the case is
`FCacheTests.MeasuresRepeatedBlobWork`. These are work-count and accounted-memory
observations, not a qualified end-to-end throughput claim.

| Metric | Before | After |
| --- | ---: | ---: |
| Blob reads / raw bytes decoded | 64 / 524288 | 1 / 8192 |
| Total read bytes | 19713 | 14484 |
| Peak query payload/codec bytes | 634740 | 118644 |
| Cold per-blob encoding calls / publications | 64 / 64 | 1 / 1 |
| Warm per-blob encoding calls / publications | 64 / 64 | 0 / 0 |
| Warm total write bytes, including manifest | 19713 | 14401 |
| Warm post-manifest payload/codec peak bytes | 1334353 | 126836 |

Warm reuse adds one 83-byte verification read; it still validates and inflates that
content before skipping compression/publication. Store peaks exclude transient CB
assembly, which retains its separate bounded preflight; these metrics are not RSS.
Before/after receipts are `Build/Stage4/before.xml` and `Build/Stage4/after.xml`, with
raw logs `Build/.agent-state/logs/20261018-164035-672408-78160-DerivedDataCacheTests.log`
and `Build/.agent-state/logs/20261018-164057-852233-78500-DerivedDataCacheTests.log`.
Later edits affect cancellation-statistics accounting and the private fixture codec,
not the measured successful structured-record path, so these measurements apply.

A controlled continuation fixture holds Put scheduling for at least 20 ms. Default
delivery took 21–25 ms; early delivery took 13–40 microseconds while owner/request
retirement still waited for persistence or its cancellation. Samples are in
`Build/.agent-state/logs/20261018-163927-014333-77560-DerivedDataBuildTests.log`.
This demonstrates the delivery boundary rather than production latency. The later
canceled-notification token latch does not change these delivered positive/negative
samples; final whole-target and isolated tests cover the final implementation.

Stage 5 evidence (MacOS-arm64-Debug-DurinEditor, 2026-10-18):

- Reduced scope was selected before implementation: process-local basic diagnostics
  and explicit-root offline inspect/verify/clear. No online reclamation, quota/age
  policy, orphan reclaim, shared backend, DAG, remote worker, or range-read API was
  implemented or required for completion; reopening criteria are recorded below.
- Added totals, 64 bucket slots plus overflow, backend outcome/I/O/codec/latency
  counters, async queue/admission observations, and rebuild/schema-rejection counts.
  Editor exit and the native asset/Cook host log drained `DDCStats` summaries.
  Telemetry observation/log failures cannot alter cache results or host shutdown.
- Shared process ownership spans record access/publication; exclusive maintenance
  refuses active roots and concurrent maintenance. Native tests and an independent
  CLI process prove conflict and termination recovery. Clear preflights containment,
  refuses links, removes records before values, preserves unrelated siblings and
  the lock file, and can resume after cancellation. Verify detects raw corruption
  even with a valid envelope, missing values, unsupported namespaces, and budgets.
- The final five-target `@module=derived-data-cache` set passed. Nine focused Cache
  cases and the Build fallback/schema case passed in isolated processes. TextureTests
  passed all 150 cases; StaticMeshTests passed all 186. DevTool cache/command contract
  tests passed all 64 cases. Ten explicit-root CLI checks passed, including native
  lock conflicts, killed-owner recovery, limit failures, and repeatable clear.
- The new Core/DDC-only DurinCacheTool and Editor-only summary hooks received a
  final workspace `all` build. Affected analysis selects all because Engine's CMake
  gains a program target and Launch gains an optional dependency; runtime execution
  stays bounded to DDC/asset consumers and offline tooling because these changes
  add no unrelated rendering/GPU behavior. Workspace API audit found no direct
  Sandbox/RoadWeaver consumers to migrate; real Sandbox Cook exercises Engine paths.
- Final build: `Build/.agent-state/logs/20261018-171805-976039-85778-cmake.log`.
  Final module: `Build/.agent-state/logs/20261018-171729-290774-85675-ctest.log`.
  Texture: `Build/.agent-state/logs/20261018-171426-896584-85321-TextureTests.log`.
  Mesh: `Build/.agent-state/logs/20261018-171526-199225-85501-StaticMeshTests.log`.
  Reports remain under `Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/`,
  with isolated reports in `Build/Stage5/cache-isolated.xml` and `build-isolated.xml`.
  CLI receipts/reproduction are `Build/Stage5/cli-smoke.json` and `cli_smoke.py`.
- Tested inputs are base revision `5eda544fd` plus this Stage 5 source/test tree.
  After the final tests/build only evidence/documentation changed. Native Windows
  execution and Editor application/GPU qualification were not run or required.

A real Sandbox Cook diagnostic baseline used two fresh processes with identical
project inputs, target and cache root:

```sh
./DevTool cook --output-dir Build/Stage5/SandboxCook --target macos --dry-run --no-incremental --format json
```

The first run used a fresh output/cache directory. Both runs captured the same seven
packages without publishing a Cook generation or changing authored packages. Before
and after inspection/verification ran only after the Cook processes had exited.

| Observation | Cold | Warm |
| --- | ---: | ---: |
| DDC queries / hits / misses | 40 / 0 / 40 | 40 / 39 / 1 |
| Structured Puts / store failures | 39 / 0 | 0 / 0 |
| Read / written bytes | 5470 / 2737743 | 2743213 / 0 |
| Blob encodes | 196 | 0 |
| Cook-reported elapsed seconds | 4.63 | 1.46 |
| Retained manifests / blobs | 39 / 196 | 39 / 196 |
| Encoded file-size total bytes | 2737743 | 2737743 |

The warm miss is the StaticMesh bucket, with no Put observed; this is a measured
follow-up opportunity rather than a shared-cache requirement. Post-run verify
validated all 235 current files with no corruption, unsupported files, or missing
values, while retaining two unrelated root entries. This small single-pair sample
validates operational observations, not stable speedup or long-term cache growth.
The later best-effort logging exception guard does not change the sampled normal
path; the final source/build checks cover that guard.

Cook reports, parsed counters, inventories, and verification are retained as
`Build/Stage5/cook-{cold,warm}.json`, `cook-{cold,warm}-stats.json`,
`cook-{cold,warm}-inventory.json`, and `cook-cache-verify.json`.
Raw workload logs are
`Build/Stage5/SandboxCook/Logs/Durin-Durin-20261018-171011-84943.log` and
`Build/Stage5/SandboxCook/Logs/Durin-Durin-20261018-171200-85145.log`.
The [storage contract](../../../Runtime/Assets/DerivedDataCacheStorage.md#bounded-diagnostics)
owns implemented counters, ownership, maintenance limits and baseline procedure;
the [DevTool interface](../../../Development/Tooling/DurinDevTool.md#offline-local-cache-maintenance)
owns commands and exit/report behavior.

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
[build protocol](../../../Runtime/Assets/DerivedDataBuild.md) and
[cache storage](../../../Runtime/Assets/DerivedDataCacheStorage.md). Update those contracts
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

Selected integration decision: existing native fixture owner-drain guards already
scope cache overrides. Retain their low-level test setters instead of adding a
second public guard API; make the family holder own each injected service and
detach it before draining outside the acquisition lock. Shader retains its
isolated compiler service; do not introduce a cross-module lifecycle framework
solely to share its small synchronous wrapper.

- [x] Consolidate duplicated session registration, submission, and shutdown/drain
  glue using existing ownership facilities; introduce a small lifecycle holder
  only when concrete duplicated behavior requires it.
- [x] Reuse submission logic while preserving immediate inline and nested
  synchronous completion; keep asynchronous task admission independent.
- [x] Preserve typed admission errors instead of collapsing them to bool/nullopt.
- [x] Keep test overrides scoped to fixture owner-drain guards, retain service
  dependencies in the family holder, and drain outside holder locks.
- [x] Decide and document Shader's isolated compiler/service lifetime explicitly.

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
remain equivalent. Validate affected consumers using the repository's
[shared API coverage policy](../../../../AGENTS.md#task-routing), including an `all`
build when that policy requires it. Stage 3 completes only after both
3A and 3B pass; do not add DAGs, remote workers, new cache tiers, or storage-format
changes in this stage. The historical Shader manifest fixture exception was
resolved as recorded in the follow-up above.

### Stage 4: Reuse blob work and separate result delivery from persistence

Dependencies: Stages 2 and 3.

Boundary: cache owns content reuse; resource owners control when usable results are
published. Persistence remains owned work through shutdown, including buffer lifetime.

Selected Stage 4 decisions (2026-10-18):

- Request-local reuse keys include supported blob identity version, raw hash, and
  raw size. Encoded/workspace budgets count distinct requested content; logical
  output limits continue to count every descriptor.
- Retain the existing encoded-read coalescer, bounded to 64 flights and 64 joined
  waiters per flight, with independent overflow and per-caller cancellation.
  Request-local measurements confirm the primary read/decode reduction; cross-request
  decoded-buffer sharing remains deferred to avoid coupling request workspace budgets.
- `BeforePersistence` result delivery is opt-in; default behavior still waits for
  persistence. Request/owner waits and shutdown retain persistence ownership after
  notification, and later cancellation cannot revoke the delivered output.
- Whole-record codecs move to the private fixture boundary after the workspace
  consumer audit; manifest/value production APIs and persisted identities remain.

- [x] Deduplicate requested blobs by versioned identity and raw size, sharing
  immutable decoded backing across value IDs; keep physical/logical budgets explicit.
- [x] Reuse valid stored content and retain atomic replacement for damaged blobs.
- [x] Add bounded in-flight read sharing only after request-local reuse is measured;
  separate waiter cancellation from shared work cancellation.
- [x] Define opt-in early result delivery and a separate persistence-retirement
  contract, retaining a mode that waits for persistence.
- [x] Remove whole-record codecs from production API after auditing every workspace
  consumer; retain necessary fixture support privately.

Acceptance: repeated identities perform one physical read/decode per request and
share backing; valid repeated writes avoid redundant compression/publication.
Damaged content is repaired and failed writes preserve existing manifests. A delayed
Put does not delay opted-in result delivery; owner drain still joins persistence
and captures. Store failure cannot invalidate a delivered result. Cold/warm byte
equivalence, negative hits, budgets, and cancellation pass. Record before/after
physical I/O, codec work, delivery latency, and retained-memory measurements.

### Stage 5: Add basic diagnostics and offline maintenance

Dependencies: Stage 4.

Scope revision (2026-10-18): local cache correctness and reuse are implemented.
There is no measured requirement for shared storage or online collection. Complete
this plan with a small operational foundation rather than more cache architecture.

- [x] Add bounded process-local totals and per-bucket statistics for hit/miss,
  invalid records, backend/store failure, admission rejection, rebuild fallback,
  queue delay, request latency, and physical I/O/codec work. Keep these outside keys,
  persisted records, and completion status; provide a snapshot and shutdown summary.
- [x] Add explicit-root offline inspect/verify/clear commands. Inspect reports
  physical disk usage; verify applies existing manifest/blob integrity checks with
  file/read/workspace limits and cancellation. Clear removes recognized record/value
  storage only, preserving unrelated root contents.
- [x] Protect ordinary cache use with shared process ownership and maintenance with
  exclusive nonblocking OS ownership. Refuse active roots, unsafe paths/symlinks,
  and concurrent maintenance; release crashed owners through the OS lock lifetime.
- [x] Validate observations, bounded cardinality, ownership exclusion/recovery,
  integrity errors, cancellation, and contained repeatable clearing. Document how
  to collect editor/cook baselines when representative runs are available.

Acceptance: diagnostics distinguish a normal miss, rejected record, backend failure,
and admission rejection without changing build results. All offline operations
refuse an actively owned root; inspection/verification do not delete content, and
clear cannot traverse links or remove unknown siblings. Verification and scans have
explicit limits, return incomplete/error rather than a false success, and stop
cooperatively. Real editor/cook timings are follow-up measurements, not a synthetic
performance gate; do not claim end-to-end gains from fixture timings. Run focused
DDC/tool tests and affected consumer compilation/coverage under repository policy.

### Deferred work and reopening criteria

- **Former Stage 5 online collection and policy:** no online GC, publication
  epochs/leases, quota/age eviction, selective orphan reclaim, background scans, or
  automatic deletion in this stage. Reopen only when measured cache growth or
  disk-pressure incidents justify it, with a separate publication/read lifetime
  protocol and crash/concurrency acceptance plan.
- **Stage 6 shared/local backend composition:** deferred. Reopen only for an
  identified multi-machine/CI consumer, measured duplicate build cost and expected
  shared-hit benefit, and an operational owner. A separate plan must cover
  cryptographic identity/versioning, authentication, timeout/fallback, promotion,
  local-only behavior, and interrupted/concurrent publication.
- **Stage 7 execution/storage extensions:** DAGs, remote workers, and value-range
  reads are deferred. Start with representative editor/cook observations; reopen
  only for a named consumer and measured bottleneck, in a separate bounded plan.
  Existing selected-value queries are sufficient until a consumer needs partial
  bytes within a value. No placeholder extension interfaces are required.

These deferrals are deliberate scope decisions, not completed implementations.
Stage 5 acceptance completes the selected scope of this plan without implementing
Stages 6–7 or the deferred online-maintenance features.

## Validation and Handoff

Follow [agent testing](../../../Agents/Testing.md),
[build workflow](../../../Agents/BuildAndRun.md), and
[documentation validation](../../../Agents/Documentation.md). Run lifecycle plan validation
when adding this plan. Keep evidence and passed checklists current in each stage
commit, with exact Plan and Stage trailers. Subsequent stages remain open until
their acceptance gates are met; passing Stage 1 does not complete this plan.
