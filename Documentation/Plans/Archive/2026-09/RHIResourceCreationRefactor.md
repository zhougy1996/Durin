# RHI Resource Creation Refactor Plan

Summary: Decouple resource factories from command replay, introduce bounded asynchronous PSO creation and shared requests, and migrate resource consumers with explicit readiness and lifetime contracts.

Last reviewed: 2026-09-09

Status: Archived
Completed: 2026-09-09

## Current Status

Stage 0 is complete under the user-approved measurement-policy revision below.
The audit, contracts, fixed-input measurement tools, raw samples, and hardware
receipts are retained. Timing variability is recorded as uncertainty, not a
prerequisite blocking functional implementation. Stage 1 is implemented and
validated with the unrelated property-editor failure recorded below.
Stages 2–6 are implemented and validated under the approved measurement policy.
Qualification reports observed timing and process-memory regressions as well as
zero creation replay round trips; it does not claim global performance acceptance.
The Stage 6 handoff distinguishes these outcomes and remaining attribution limits.
The user-requested 03:48 repeat on September 9 did not reproduce the cold/first-
frame median slowdown: those returned near baseline without code changes.
Following-frame p95 and process private-memory peaks remain elevated; see the
repeat receipt below. Earlier observations are retained as historical samples.
The original Balanced power scheme remains active.
The subsequent [qualification attribution investigation](../../../Investigations/RHICreationQualificationAttribution.md)
localizes the remaining memory increase to diagnostics-dependent retention and
frame tails to host completion waits; exact retaining allocations and host-wait
contributions remain open. Diagnostic controls do not replace the baseline.

The universal Vulkan creation/scheduling wrapper has been removed. The RHI
facade retains explicit scheduling only for context-owned operations; native error classification neither schedules
nor waits. Vertex declarations construct directly on the calling thread. PSO
Ready hits return directly; misses use the bounded device-owned Core channel
when the scheduler is running. Renderer uses asynchronous slots and an explicit first-consumer preparation boundary.

The recovery document's distinct-object description has been corrected to the
cache-reuse contract confirmed by code and tests. The Stage 0 handoff below
records execution domains, interface boundaries, measurements, and receipts.

## Goal

Assign requests, scheduling, and lifetime to the RHI facade and creation
service, and concrete object creation and native error translation to Vulkan
factories. Eliminate replay round trips for CPU-only resources and completed
PSO cache hits, move expensive PSO creation off replay, and make consumers
handle readiness dependencies explicitly.

Scope includes graphics/compute PSOs, vertex declarations, and execution-domain
audits and migration of shader, sampler, buffer, and texture factories. Complete
the PSO path first. Not every native operation must run in parallel. Model native
creation separately from uploads, layout transitions, and submission.

Do not rewrite the Core task scheduler, Render Graph, or material compiler;
introduce multiple GPU scheduling queues; copy UE's entire cache architecture;
or promise that first use never waits.

## Selected Design

### Ownership and execution

- The RHI creation service owns request caches, task groups, capacity, and
  concurrency budgets for one device lifetime. Reuse results, dependencies,
  cancellation, and admission from the
  [CPU Task System](../../../Runtime/Core/TaskSystem.md), without another generic
  future framework. Its API was qualified by the archived
  [Async Task Framework Refactor](AsyncTaskFrameworkRefactor.md);
  use the landed interfaces rather than assuming additional capabilities.
- Backends declare direct, background, and context-required operations. Vulkan
  factories must not access `GCommandListExecutor` or implicitly wait for replay.
  The facade explicitly schedules context operations without another universal
  synchronous creation wrapper.
- Construct immutable CPU data such as vertex declarations directly. PSOs use
  a bounded background channel. A concurrency limit of one is supported, but
  the channel remains independent of replay. Do not simulate capacity control
  by blocking many Core workers on semaphores.
- Uploads, resource transitions, command pools, and submission retain their
  designated contexts. Object existence, CPU creation completion, and GPU data
  readiness are separate conditions; upload dependencies remain explicit.

### Requests and cache publication

- Synchronous entry points return complete resources or failure. Async entry
  points return separate requests with Pending, Ready, Failed, and Canceled
  states and completion notification. Distinguish rejection from failure of an
  accepted request. Never publish partially initialized resource objects.
- Cache identity uses normalized descriptions and device generations, excluding
  diagnostic names. Ready hits return strong references directly; Pending hits
  share ongoing creation. Key equality compares full semantics, not just hashes.
- On a miss, register Pending and reserve capacity in a short critical section,
  create outside the lock, then publish under it. Task-admission failure rolls
  back reservations and terminalizes all observers, leaving no permanent Pending.
  Retry follows the resource's relevant generation policy.
- Bound request counts, description bytes, residency, and concurrent creation.
  Batches preserve each result and input order. The batch API does not promise
  atomic resource-set publication; owning Renderer slots publish aggregates
  transactionally.
- Canceling one observer must not cancel compilation needed by others. Device
  shutdown may cancel unstarted work; work inside a driver call must actually
  return before releasing dependencies.

### Concurrency and lifetime

- Audit PSO, render-pass, descriptor-layout, and pipeline-layout caches, LRU,
  statistics, debug naming, and release paths. Never hold a global cache-table
  lock across expensive driver compilation. Reservation, eviction, and strong
  reference acquisition must share a consistent concurrency protocol.
- Define driver-cache locking for creation, compilation access, merging,
  serialization, and destruction. Verify concurrency against actual Vulkan
  configuration and specification instead of mechanically copying UE's read lock.
- VMA configuration does not establish engine-wide allocator safety. Also audit
  engine pools, statistics, mapped writes, staging, reclamation, and state tracking.
- Requests own descriptions and strongly retain shaders, vertex declarations,
  and layouts. Remove implicit reliance on synchronous return from `&Result`,
  raw `this`, and borrowed-array captures.
- Ordinary candidate failures remain recoverable. Device loss, replay failures,
  and submission failures remain terminal; Core exception capture must not
  silently downgrade them to ordinary PSO failures.
- Shutdown closes request admission, cancels or joins creation, consumer
  dependencies, and publication tasks, then releases caches and the device.
  Old device/shader results cannot publish to new-generation owners. Ending CPU
  references does not replace GPU completion requirements for deferred destruction.

### Consumers and ordering

- Prewarm when shader/material and rendering configuration are known, without
  immediately waiting after each request. Renderer preparation checks results;
  Pending does not count as a failed generation or repeat failure logging.
- Retain old complete payloads only when identity and bindings remain compatible.
  Otherwise use the feature's defined fallback or skip dependent draws; never
  bind an incompatible PSO.
- Mandatory consumption of unfinished requests uses command-batch dependencies.
  Dispatch only after scheduling-layer readiness; never wait for creation on
  the sole replay thread.
- Preserve recording order, submission serials, fences, rejection/retry, and
  bounded queues. Later ordered work cannot arbitrarily bypass an unready batch.
  Accepted batches reach observable terminal states on shutdown or dependency
  failure, without leaving fences waiting forever.
- Creation tasks cannot wait back on replay batches that depend on them.
  Synchronous compatibility calls cannot block in threads/contexts that could
  form cycles. Inline mode has an explicit, testable policy.

## UE Reference Evidence

Reference source is under
`E:/Programs/Epic Games/UE_5.8/Engine/Source/Runtime/`, reviewed on 2026-09-07.
Paths and line numbers below are research locations, not repository links or
cross-version promises. Do not copy UE source into this repository.

| Source relative to UE Runtime | Observed behavior | Adoption boundary |
| --- | --- | --- |
| `RHI/Public/DynamicRHI.h:1125` | PSO/vertex-declaration wrappers call DynamicRHI directly | Separate factories from scheduling |
| `RHI/Private/PipelineStateCache.cpp:4138` | Capabilities/configuration control async task-graph or precache-pool compilation | Background creation, bounded concurrency, capability gates |
| `RHI/Private/PipelineStateCache.cpp:4831` | Consuming unfinished PSOs adds completion dependencies | Separate readiness from resource objects |
| `RHI/Private/RHICommandList.cpp:240` | Android/Mac defer dispatch; other platforms wait at command translation | Do not claim UE never waits; Durin selects scheduling-layer dependencies |
| `RHI/Private/PipelineStateCache.cpp:3808` | Compilation tasks retain shaders and vertex declarations | Requests own dependency lifetimes |
| `VulkanRHI/Private/VulkanVertexDeclaration.cpp:60` | Direct construction after local cache locking | CPU objects need no replay round trip |
| `VulkanRHI/Private/VulkanPipeline.cpp:2722` | Configurable serial compilation; table locking does not cover all creation | Control concurrency and lock granularity independently |
| `VulkanRHI/Private/VulkanPipeline.cpp:2841` | Recheck on publication and clean up duplicate candidates | UE does not guarantee one compilation per key; Durin selects shared Pending |
| `VulkanRHI/Private/VulkanBuffer.cpp:571` | Buffer creation is separate from upload-context initialization | Separate native creation from command initialization |

## Implementation Stages

### Stage 0: Establish contracts and measurement baseline

- [x] Verify PSO identity in code, callers, and tests; distinguish names for
  independent native creation and cached requests, and record the resolution of
  the recovery-document discrepancy.
- [x] Inventory all Vulkan creation-wrapper callers, shared state, thread
  assertions, and release paths; classify direct/background/context execution.
- [x] Confirm reusable Core APIs, creation budgets, driver-cache locking, inline
  policy, and fence/dependency-failure handling; settle remaining design choices.
- [x] Establish comparable cold/hot PSO, duplicate-key, batch-resource, and material
  first-use baselines. Separately record request queuing, CPU preparation, native
  creation, consumer waits, frame time, and peak memory. Record hardware, driver,
  build configuration, concurrency, cache state, and sample counts.
- [x] Freeze comparisons, noise tolerance, and acceptable regression budgets
  before runtime changes rather than adjusting them afterward.

Completion: Execution domains and interface decisions are consistent, and fixed-input
hardware baselines are recorded with their uncertainty. Reproducibility affects
performance conclusions rather than admission to functional implementation.
Counter-based inference cannot replace measured durations.

#### User-approved measurement-policy revision — 2026-09-08

The user authorized recording the excessive strictness of the uniform 5%
stability requirement and continuing implementation. This decision supersedes
historical statements below that reject the baseline as a prerequisite for Stage 1.
Retain all raw samples, rejected comparisons, measurement settings, and uncertainty;
do not relabel them as stable or as proof of performance gains.

Stage 0 now requires a complete recorded baseline, which the existing fixtures
and receipts supply. The 5% drift indicator remains diagnostic. Microsecond-scale
comparisons must also report absolute differences and instrumentation/resolution
limits; relative drift alone is insufficient evidence of an engine regression.
Keep the original regression budgets and deterministic capacity, ownership,
ordering, recovery, and shutdown gates. Stage 6 still runs actual measurements
and reports per-metric outcomes as supported improvement, supported regression,
or inconclusive because of noise. Inconclusive timing is an explicit limitation,
not a functional implementation blocker or a claimed performance pass. Targeted
retesting replaces unbounded attempts to obtain a uniformly stable full suite.
No additional baseline-only qualification is required before Stage 1.

#### Stage 0 handoff — 2026-09-07

Audited source: `1b719d72232fb1145a03675b3b0445326c941c0c`. This change updates
only documentation, preserving the runtime path before refactoring. The decisions
below are implementation requirements, not claims that background creation landed.

**Identity and naming.**
`FVulkanPipelineManager::CreateGraphicsPipelineState` and
`CreateComputePipelineState` include key lookup, strong caches, and LRU
publication. Rename this layer to `GetOrCreate`/`Request` during implementation,
and name the independent candidate layer `Create`. Preserve the public
`RHICreate*PipelineState` synchronous complete-or-null compatibility contract
and existing reuse. Independent candidate factories neither query PSO tables
nor retain candidates. `RuntimeFactoriesReturnNullThenRecoverOnTheSameRHIThread`
in `VulkanFailureInjectionTests.cpp` asserts equal-description identity,
different objects for different state under the same name, and reuse after
signed-zero normalization. Its capacity and native-creation assertions also
depend on caching. `StaticMeshRenderer.cpp` publishes complete candidates by
slot identity without requiring a unique pointer from each creation call;
fixed Renderer callers also retain PSO references. The
[recovery contract](../../../Runtime/Rendering/RendererResourceRecovery.md) is corrected.

**Execution-domain inventory.** Production has nine common-wrapper calls, all
in `FVulkanDynamicRHI::RHICreate*` implementations; tests also call it directly
to verify error types. Selected domains require corresponding-stage validation
before activation.

| Entry and implementation file (VulkanRHI/Private) | Current shared state, threading, and release constraints | Selected execution domain |
| --- | --- | --- |
| VertexDeclaration — `VulkanResources.cpp` | Copies Elements; no native handle or constructor thread assertion; atomic failure injection; ordinary RHI reference release | Direct CPU construction; no new cache |
| GraphicsPipelineState — `VulkanPipeline.cpp` | Constructor, manager lookup, and destructor require RHI thread; PSO tables, AccessSerial, and statistics unlocked; render-pass/layout dependencies; destructor notifies contexts and defers pipeline/layout deletion | Bounded background candidates and request cache; context invalidation and destruction of used objects stay with replay owner |
| ComputePipelineState — `VulkanPipeline.cpp` | Same as graphics; shared layout tables, AccessSerial, and driver cache; deferred deletion | Same as graphics |
| Shader — `VulkanShader.cpp` | Reflects vertex inputs and creates shader module; constructor/destructor thread assertions; borrowed code/debug name; deferred module deletion | Independent native creation later; explicit facade context compatibility path first |
| Sampler — `VulkanTexture.cpp` | Constructor/destructor thread assertions; naming; deferred deletion | Independent native creation later; explicit context compatibility path first |
| Buffer — `VulkanBuffer.cpp` | VMA allocation, naming, memory statistics; constructor/destructor thread assertions; facade calls WriteBuffer after creation; borrowed InitialData | Separate native creation/uploads; mapping, staging, dynamic suballocation, state, and submission stay with context |
| Texture — `VulkanTexture.cpp` | VMA image, state tracking, naming; constructor/destructor thread assertions; storage creation records InitializeTexture; destruction may notify framebuffers | Separate native creation/initialization commands; state, framebuffer invalidation, and uploads stay with context |
| BufferView — `VulkanTexture.cpp` / `VulkanView.cpp` | Strong source-buffer reference; formatted views create native handles; constructor/destructor thread assertions; deferred deletion | Include this previously omitted category; explicit context path first, then direct native creation only |
| TextureView — `VulkanTexture.cpp` / `VulkanView.cpp` | Strong source-texture reference; image view and atomic DebugIdentity; constructor/destructor thread assertions; deferred deletion | Same as BufferView; automatic view caches retain separate publication/invalidation protocols |

Common constraints: `CheckVulkanRHIThread` checks ownership only when
`GRHIThread` exists. Inline mode skips it, which does not establish concurrent
safety. All nine lambdas retain local result references dependent on synchronous
return. Shader code, description debug names, view source pointers, and buffer
initial data each need request ownership; moving existing lambdas into tasks
is insufficient.

Dependency-cache audit: the render-pass table returns manager-owned raw pointers
without local locking; framebuffer lists and invalidation belong to replay.
`FindOrAddLayout` has unlocked tables, LRU, and statistics. Descriptor-set-layout
caching has a mutex covering native creation; device-resident handles cannot be
reclaimed directly with layout LRU eviction. Automatic view caches have local
mutexes, which do not move construction/destruction off the RHI owner. Naming
counters and internal IDs are atomic; test events have a separate lock. Naming
configuration and device destruction change only after joining creation tasks.

`FDeferredDeletionQueue` protects its queue with a mutex but reads the completion
tracker before enqueueing; release still asserts RHI ownership. This does not
prove arbitrary-thread destruction safe. PSO/image deletion also notifies
contexts. Unpublished, never-used native candidates may roll back directly in
the creation domain. Published objects require owner invalidation and GPU-token
deferred deletion. Cross-thread final release must transfer to a retirement
record with guaranteed capacity, without fresh task admission. VMA does not set
its externally-synchronized flag, and the memory-baseline tracker has a lock.
Dynamic uniform/storage, staging, mapped writes, and replay state retain their
owners; VMA or statistics safety does not cover the whole buffer/texture path.

**Implementation decisions and capacity.** Verified landed APIs in
`Threading/TaskComposition.h` include `FTaskGroup` construction/Close/JoinAsync,
`LaunchTask`, `Share`, `TCompletionSource::Create`, and `GetCompletion`.
Construction returns directly; device request/payload budgets belong to the
cache owner, separately from scheduler queueing. Use a device-owned counted group, shared
immutable results, and independent observer cancellation. Do not directly use
`TTaskOperationQueue`, whose Pump requires the owner thread, as a concurrent
cache. Batches return per-item admission/requests in input order; one failed
WhenAll must not discard other successes. Device/shader identity belongs in
request keys; manual retry generations belong to failure policy.

Initial limits retain 2,048 resident PSOs each for graphics and compute. Across
both types, allow at most 256 unfinished unique requests, 4,096 active observers,
and 16 MiB of unfinished descriptions. Limit each description to 1 MiB and each
batch to 256 items; budget 64 MiB for cached CPU key/result metadata. Retain 256
structural layouts and add device-resident limits of 1,024 render passes and
4,096 descriptor-set layouts. Exhaustion rejects recoverably without destroying
entries referenced by raw handles. These budgets do not bound driver PSO
internals; measure process/device peaks separately. Check size arithmetic for
overflow and reserve before copying large payloads. Externally retained results
are not evictable cache entries; release observer/result budgets only after the
last owner exits.

Initial creation concurrency is **1**. One background drain task admitted by
Core takes work from the bounded device queue and exits when empty. Coordinate
enqueue/exit handoff under the same short lock. Do not schedule a semaphore-
waiting worker per item. Task-admission failure terminalizes registered requests
and releases capacity; cancellation/exceptional exit also terminalizes queued
observers. Higher concurrency requires separate protocol/budget validation.
Inline RHI mode still uses this channel. Without Core startup, reject async
admission instead of silently running expensive creation inline. Synchronous
compatibility calls may wait only outside replay and creation workers while
the device runs normally. Ready returns directly; cyclic Pending waits are forbidden.

PSO-table locks cover lookup, reservation, LRU, strong-reference acquisition,
and publication only; native compilation runs outside them. Dependency caches
have local locks. Obtain stable dependencies before compiling and do not enter
dependency tables under the PSO-table lock. Statistics reads/resets join the
same protocol; stop exposing mutable references to arbitrary threads.
Render-pass/descriptor handles remain device-resident until all creation tasks
and consumers exit. Framebuffer caches do not move to the creation thread.

The current driver cache uses default flags without the externally-synchronized
flag. Vulkan's requirement for that flag is documented in
[vkCreatePipelineCache](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreatePipelineCache.html).
This plan still selects a separate exclusive driver-cache lock covering creation
access, compilation, merging, data retrieval, and destruction for the initial
serial policy. Do not nest it with the PSO-table lock or claim Vulkan requires
all default-cache compilation to serialize. Save/destroy only after closing
admission and joining creation.

Reserve command-dependency waiting records before admission. Accepted batches
receive ordered serials; dispatch only a ready queue head without later batches
bypassing it. Add batch terminal states separate from GPU completion: dependency
failure/cancellation ends the batch and its fence with observable failure, not
GPU success. Inline `TryWaitForSerial` currently waits only for completion and
must also wake on failure. Candidate errors become ordinary Failed outcomes.
Device loss/invariant errors first publish terminal device state and wake batches
and fences, then let Core record the exception; task Failed alone is insufficient.

**Frozen measurement protocol (not executed).** Compare baseline and changed
versions in a quiet environment with identical hardware, driver, build profile,
validation layers, Tracy configuration, resolution, and resource inputs. Start
with `Win64-Debug-DurinEditor`. If Release is needed, establish its separate
baseline before runtime changes; never compare across configurations. Record
GPU vendor/device/driver, CPU, RAM, commit SHA, switches, and cache-seed SHA-256.
Do not clear the user's global driver cache. Cold means no application pipeline-
cache seed in the test's isolated directory; explicitly record that driver-private
disk caching is uncontrolled. Restore the same application seed each round so
one version cannot alter later inputs.

| Scenario | Fixed inputs and sampling method |
| --- | --- |
| Cold graphics/compute | 30 independent device lifetimes each; no application seed, fixed shader bytes/descriptions; measure first request and report initialization separately |
| Hot graphics/compute | Fixed seed and one successful warm-up; 30 groups of 100 same-key requests retaining strong references; vary DebugName to verify unchanged identity |
| Duplicate keys | 16 producers request one key simultaneously through a barrier; 30 fresh-device rounds, retaining each result and native-creation counts; separately measure 16 distinct keys |
| Resource batches | 64 distinct PSOs and 64 each of shader/sampler/buffer/texture/view/declaration; 30 rounds with fixed initial data/descriptions; baseline uses same-order synchronous loops |
| Material first use | Fixed static-mesh scene, material/output/camera/resolution; 30 independent runs recording first visible frame and next 120 frames; mark prewarm trigger and first consumer frame separately |

Add measurement points before runtime changes and keep definitions consistent
across versions: facade entry to admission, admission to creation-task start
(replay-operation start in baseline), normalization and CPU native-info
preparation, `createGraphicsPipeline/createComputePipeline` boundaries, actual
consumer waits, and complete frame boundaries. Split CPU preparation into
pre-admission and creation-body work. Native-call time excludes structural
layout creation and driver-cache lock waits. Per item, record request ID, key
hash (diagnostic only), device generation, timestamps, and terminal outcome.
Per round, record synchronous-operation/submission-serial/NativeCreations deltas,
peak process private bytes, device allocation peaks, and queue peaks. Executor
WaitDuration is globally cumulative and cannot reconstruct per-request queuing
or native durations. The Vulkan memory tracker excludes complete CPU/driver-
private memory and cannot replace process peaks. Keep logging/snapshots outside
measured intervals; report instrumentation overhead with identical empty-operation
sampling separately.

Report raw-sample median, nearest-rank p95, and maximum, not only averages.
Consecutive scenario medians differing by over 5% indicate instability; retain
results but do not use them for acceptance. Freeze regression budgets in advance:
cold native creation, first-consumer waits, and frame median/p95 may increase by
at most `max(5%, 0.1 ms)` over baseline; hot-request latency by `max(5%, 5 us)`;
process/device peaks by `max(5%, 16 MiB)`. All explicit capacities must hold.
Hot Ready hits and vertex declarations require zero replay round trips. One
key/generation permits only one successful native creation; independent replay
must advance during controlled slow creation. Stage 6 compares baseline against
serial background configuration 1. Do not accept regressions by changing samples,
enlarging tolerances, or redefining cold.

**Validation receipt and next steps.** At the audited baseline,
`VulkanRHIIntegrationTests` /
`FVulkanCreateFailureInjectionTests.RuntimeFactoriesReturnNullThenRecoverOnTheSameRHIThread`
passed under `Win64-Debug-DurinEditor` (1/1; reported 581 ms is total test time
only). Local receipt:
`Build/.agent-state/logs/20260907-152622-050464-18604-VulkanRHIIntegrationTests.log`.
Measurement fixtures/per-stage timestamps, a fixed material-scene manifest and
input digests, and raw samples/hardware receipts for every scenario remain
outstanding. Protocol preparation does not complete the baseline checklist item.
On resumption, implement those measurements, sample in a quiet environment, and
accept Stage 0 before starting Stage 1.

#### Stage 0 measurement implementation — 2026-09-08

`VulkanCreationTiming.h` adds a test-only, opt-in capture with a maximum of
65,536 records and explicit dropped-record reporting. Graphics/compute facade
records cover entry, pre-scheduling, replay-body start/end, native-call start/end,
and the return boundary before capture storage; failed requests retain the
boundaries they reached. Capture storage/locking overhead is not included in
that return timestamp and still needs a separate overhead receipt.
Replay uses a scoped thread-local record installed from the synchronous facade's
capture. Native-call scopes close on exceptions. Capture setup/drain require
joined producers; scopes cannot be copied. Runtime scheduling, caching, and
resource lifetime have not been refactored.

`VulkanCreationQualificationTests.ColdHotAndConcurrentRequests` runs graphics
and compute separately, each with 30 independent device lifetimes for cold,
hot, 16 same-key producers, 16 distinct-key producers, and 64-item synchronous
PSO batches. Hot rounds restore the same per-kind application-cache seed, warm
once, then retain 100 same-key results with different names. Duplicate and hot
results assert pointer identity; every scenario checks expected native-creation
counts and sample counts. Concurrent requests use a 16-producer barrier.
Unreferenced shader slots distinguish the fixed distinct-key descriptions.

The test-only cache-path override confines all application-cache reads/writes
and cold removal to the fixture sandbox. Shader compilation precedes sampling;
compiled SPIR-V, device/API/driver identification, device initialization times,
request CSV, group counters, and seeds are retained in the fixture output.
Use `DURIN_TEST_KEEP_WORK=1` to retain successful output. Set
`DURIN_VULKAN_CREATION_SEED_DIRECTORY` to an earlier `CreationTiming` directory
for subsequent comparisons so both versions restore identical seed bytes.
Without this setting a run creates fresh seeds and cannot establish identical
hot-cache inputs across runs. Driver-private caching remains uncontrolled.

Run the registered qualification target using the current testing workflow.
`Engine/Tests/Native/VulkanRHITests/Tools/SummarizeCreationTiming.py <output>`
validates fixed sample counts and timestamp order, then reports raw-sample
median, nearest-rank p95, and maximum in microseconds. `--compare <prior-output>`
reports seed/shader SHA-256 equality and median drift; its status deliberately
remains diagnostic because it does not implement all Stage 0 acceptance gates.
The CSV's round/scenario/pipeline-kind tuple identifies a device lifetime;
request IDs and key hashes are diagnostic identifiers, not persistent identity.

Initial receipts: `Win64-Debug-DurinEditor`, Tracy disabled, headless Vulkan,
threaded replay, Intel Core i7-12700, NVIDIA RTX 3090, driver raw value
`2480242688`. The focused existing factory-failure test passed (1/1), including
native, cache-hit, and failed-request timestamp boundaries. Its log is
`Build/.agent-state/logs/20260908-065706-779851-31516-VulkanRHIIntegrationTests.log`.
Two isolated PSO qualification runs passed (1/1 each):
`20260908-070311-665565-32020-ctest.log` and
`20260908-070505-356066-37008-ctest.log` under `Build/.agent-state/logs/`.
Their run directories are `run-p7372-380394c820ae46fa882ffc019f929055` and
`run-p19900-00a4c72d4995407cbb00635755674086`, under
`Engine/Binaries/Win64/Debug/Tests/DurinEditor/VulkanCreationQualificationTests/Work/Runs/`.
Compute batch request median changed by +16.38%, and distinct-key concurrent
request median by +12.90%. Retain these observations without accepting a
baseline or attributing them to an engine regression.

Final validation: `test affected` passed 79/79 target executions, including
bounded concurrent capture and exceptional native-call coverage. Receipt:
`Build/.agent-state/logs/20260908-070748-229782-31512-ctest.log`.
The corrected fixture then passed two consecutive qualification runs (1/1 each),
restoring the first run's exact seeds. Logs:
`20260908-070922-176063-32548-ctest.log` and
`20260908-071039-622776-29220-ctest.log` under `Build/.agent-state/logs/`.
Run directories: `run-p12364-347245910a6549deae54869ca7741dd3` and
`run-p34500-7c628f196d584ff1802abfa0320a8076` under the same `Work/Runs/` parent.
The summarizer validated all 11,820 samples per run and confirmed identical
shader and seed SHA-256 values. Cold-compute request medians differed by -5.92%,
still beyond the frozen noise tolerance; no performance gate is accepted.
Local comparison output: `Build/creation-timing-fixed-seed-comparison.json`.
Changed-document and all-plan validation passed.

#### Stage 0 complete measurement coverage — 2026-09-08

The timing seam now covers all nine inventoried creation entry points. PSO
normalization and native-info preparation have explicit CPU scopes; dependency
scopes exclude render-pass/layout lookup and native pipeline-layout creation
from native PSO compilation. Native spans also cover shader, sampler, VMA buffer/
image allocation, and view creation. Declarations have no native span. Request
body CPU time is reported separately as body wall time minus native/dependency
time. The baseline has no driver-cache mutex, so there is no driver-lock wait
to include; migrated locking must remain outside the native-call span.

Optional `FRHISynchronousOperationTiming` records queue admission under the RHI
queue lock and the caller's wait entry/exit. Admission storage is consumed before
work publication, and the synchronous caller reads after return. This separates
pre-admission delay, actual queued time, and actual synchronous consumer waits.
Inline execution records zero admission/wait timestamps. These diagnostics do
not change execution domains; the Vulkan request trace remains stack-owned only
because its current facade is synchronous. Async migration must own the trace
through completion rather than preserving this borrowed-pointer assumption.

`VulkanCreationQualificationTests.ResourceBatches` creates 64 each of shaders,
samplers, buffers, textures, formatted buffer views, sampled texture views, and
vertex declarations, retaining every result in each of 30 device lifetimes.
It records creation, upload recording, and submitted GPU-readiness waits
separately. Fixed 1,024-byte initial data and compiled shader bytes are retained.
`TimingInstrumentationOverhead` records 30 groups of 1,000 empty-operation
observations with capture disabled/enabled, including trace storage overhead.

`MaterialCreationQualificationTests` uses the production Renderer for a fixed
256x256 offscreen unlit opaque quad. Its generated `scene.txt` records geometry,
material, camera, output, prewarm trigger, and complete-frame boundaries.
Accepted material SPIR-V is frozen and checked across device lifetimes. Each of
30 lifetimes records its first visible frame and 120 following frames; first
and last pixels must agree, and their color ordering rejects an empty or error-
material image. Material/mesh readiness marks the baseline prewarm opportunity;
frame records distinguish producer, render, and GPU completion. No baseline PSO
prewarm is performed. Per-frame requests, counters, queue peaks, and memory
samples retain actual first-consumer costs.

The Windows memory observer matches the Vulkan device LUID to DXGI and samples
process private bytes and local/nonlocal application video-memory usage at a
requested 1 ms interval. It reports sample count and maximum observed gap;
these are sampled window peaks, not a claim of exact instantaneous maxima.
Windows process-lifetime commit high water is a separate metric. Device-local
usage follows [DXGI's application-usage definition](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/ns-dxgi1_4-dxgi_query_video_memory_info),
not VMA's allocation-only estimate. The observer joins before device teardown.
Host receipts include RAM, CPU, build flags, requested switches, and actual
validation/debug-utils availability. Sampling is part of the frozen comparison
configuration and must be identical before/after the runtime refactor.

Use the bounded `@kind=qualification,domain=rhi-creation` selection for both
qualification targets; the shared GPU/lifecycle locks serialize them. Preserve
successful work with `DURIN_TEST_KEEP_WORK=1` and restore the same seed directory
for repeated runs. The summarizer accepts `--resource-root`, `--material-root`,
and `--overhead-root` alongside the PSO root; `--compare` also accepts a previous
summary JSON. It validates fixed counts, successful outcomes, timestamp ordering,
and non-overlapping native/dependency durations before summarizing. Raw logs,
CSV files, shader/seed bytes, and manifests are retained in test sandboxes.

Validation so far: the extended resource fixture passed all 30 lifetimes; the
material fixture passed all 3,630 frames; the three Vulkan qualification cases
passed together. `test affected` then passed 79/79 target executions, including
inline/threaded fallible-operation timing and concurrent capture coverage:
`Build/.agent-state/logs/20260908-074000-082913-25564-ctest.log`.
Early diagnostic summary `Build/creation-baseline-diagnostic.json` reports a
0.3 us median enabled empty trace (disabled median below timer resolution),
321.70 ms median first material frame and 2.91 ms median following frame.
These diagnostic durations precede the final repeated measurement receipt and
are not accepted baseline values. Still open: reproducible, same-input full-suite
baseline evidence under the original stability criterion. Stage 1 remains gated
on that evidence.

Two full-suite passes with the final instrumentation preserved 11,820 PSO,
13,440 other-resource, 3,630 frame, and 60,000 overhead observations per run.
Receipts under `Build/.agent-state/logs/`:
`20260908-074213-703525-29640-ctest.log` and
`20260908-074801-652015-7040-ctest.log`. Vulkan run identifiers are
`run-p35040-bf8106183a6a4abba32ac37c21076ab5` and
`run-p40748-7ef295afe2994a7fb626ceee421e8188`; material identifiers are
`run-p25712-4e74c56a5f714785912787176964d4bb` and
`run-p24860-b3079ef775764b2d98fa49b6886cc735`, under their respective target's
`Work/Runs/` directories. Summary files are
`Build/creation-baseline-complete-a.json` and
`Build/creation-baseline-complete-b.json`. Seed, shader, resource-input,
material-input hashes and scenario/metric sets match. Four primary medians still
exceed the frozen 5% stability bound: hot compute request +5.28%, buffer native
-5.08%, texture-view native -5.45%, and material-request native -5.85%.
These runs remain diagnostic; further consecutive qualification is in progress.

The third complete run also passed correctness, but did not stabilize the
baseline (`20260908-075013-597303-20500-ctest.log`, summary
`Build/creation-baseline-complete-c.json`): duplicate graphics request/native
medians changed +6.72%/+8.23%, and shader/buffer native +5.63%/+5.36%.
Windows CPU-set inspection reports logical processors 0–15 in efficiency class 1
and 16–19 in class 0 on this hybrid CPU. This is a possible scheduling variable,
not proof of the observed drift's cause. Before accepting any baseline, select
a separate process-local affinity lane using
`DURIN_CREATION_QUALIFICATION_AFFINITY=0xffff` (the 16 performance-core logical
processors). The fixtures validate and apply the mask before device setup and
record the actual process mask. Baseline and refactor comparisons must use this
same lane; never compare its durations against unrestricted runs. This changes
neither producer counts, sample counts, cache policy, nor frozen thresholds.
It does not alter machine power policy or other processes. The initial measured
runtime remains the synchronous implementation at `5721c81e8` plus this
qualification-only affinity setting.

The first affinity-lane suite passed both targets and complete sample validation:
`Build/.agent-state/logs/20260908-075348-100377-38560-ctest.log`, summary
`Build/creation-baseline-affinity-a.json`. Preserved Vulkan/material runs:
`run-p37792-cabc523a56a944e8b9cd1a861f6560a1` /
`run-p40736-6b04fbba8d7943118c12af4e46256540`. The actual recorded affinity is
65535. A second consecutive run is required before evaluating stability.

The second affinity-lane suite also passed both targets:
`Build/.agent-state/logs/20260908-075546-580406-33916-ctest.log`, comparison
`Build/creation-baseline-affinity-b.json`. Preserved Vulkan/material runs:
`run-p31512-18d61ee59abc4823b7ad1933eedd7e37` /
`run-p17072-99553e23c43948dc9c7dc414e5ee3f1a`. All input hashes and metric sets
match. The distinct-key graphics native median changed -5.64%; all other primary
request/native/frame medians were within 5%. Retain this rejected comparison
alongside the unrestricted diagnostics.

The third affinity-lane suite passed both targets:
`Build/.agent-state/logs/20260908-075756-412991-23132-ctest.log`, comparison
`Build/creation-baseline-affinity-c.json`. Preserved Vulkan/material runs:
`run-p28848-fb862fcede0b43098e6312deb795fa36` /
`run-p32188-2c7aa017a4d6494ba2b77690e71d2cec`. All input hashes and metric sets
again match. Batch graphics request/native medians changed -6.09%/-5.35%, so
this consecutive comparison also fails. Affinity alone did not resolve the
instability. Qualification source is `d08bd5f92`; no runtime refactor has begun.
Do not select nonconsecutive runs, discard unfavorable rounds, or relax the
frozen threshold to manufacture acceptance. The remaining gate requires a
demonstrably repeatable measurement environment; all current runs remain
diagnostic despite passing correctness. Further identical blind reruns are not
a substitute for diagnosing the remaining measurement variability.

Further raw-round analysis (`Build/creation-variability-rounds.json`) found that
batch graphics native and CPU preparation durations co-vary: the correlation
across round medians in affinity run B is 0.981. The active Windows power plan is
Balanced. This suggests a CPU execution/scheduling variable, but does not prove
its cause. Add an optional process-only
`DURIN_CREATION_QUALIFICATION_HIGH_QOS=1` lane alongside affinity `0xffff`, using
[Windows' explicit HighQoS API](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setprocessinformation).
Record the actual power-throttling control/state masks. This does not change
machine power policy or process priority; defaults remain system-managed.
Compare only consecutive runs with identical QoS and affinity settings, retaining
all earlier rejected observations and the original sampling/threshold rules.

Both initial HighQoS suites passed correctness and complete sample validation:
`20260908-080316-724004-37288-ctest.log` and
`20260908-080521-253521-38008-ctest.log` under `Build/.agent-state/logs/`.
Summaries: `Build/creation-baseline-highqos-a.json` and
`Build/creation-baseline-highqos-b.json`. Actual process control/state is 1/0,
and all input hashes/metric sets match. PSO and frame primary medians stabilize,
but buffer-view native changes +6.82% and declaration request +170.31%.
Declaration round medians cluster around 4.5 us or 17–18 us. Across the two
runs, aggregate queued time changes from 1.0 to 6.5 us and consumer wait from
3.2 to 11.9 us, while creation-body medians remain 0.4/0.6 us. This localizes
the large declaration variation to scheduling, not declaration copying.

The inspected CPU topology pairs logical processors 0/1, 2/3, ..., 14/15 on
the same physical performance cores. Next isolate SMT placement as a variable
with process affinity `0x5555` (one logical processor per performance core),
retaining explicit HighQoS. Keep 16 producers and all original sample counts;
baseline/refactor must use this identical configuration if it stabilizes.
The current data does not prove SMT caused the bimodality, so this remains a
diagnostic lane until consecutive comparisons pass.

The two one-logical-processor-per-core suites passed correctness, but remained
unstable (`20260908-080847-149299-4536-ctest.log` and
`20260908-081117-992275-39480-ctest.log`; summaries
`Build/creation-baseline-physical-a.json` / `Build/creation-baseline-physical-b.json`).
Six primary medians exceeded 5%, including hot compute request -9.96% and
following-frame time -5.73%. SMT exclusion alone is not a resolution.

The next bounded environment experiment retains affinity `0x5555` and HighQoS
and temporarily selects the existing Windows High performance scheme
`8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c` for two consecutive complete suites.
The local runner verifies the starting Balanced scheme
`381b4222-f694-41f0-9685-ff5bb260df2e`, records the active scheme and run paths,
and restores Balanced in a `finally` block. Verify restoration after completion.
No power-plan definitions are modified. Any accepted comparison requires this
same power scheme before and after refactoring; earlier configurations remain
diagnostic, with no cross-configuration performance comparison.

Both High performance suites passed correctness:
`20260908-081455-722082-22680-ctest.log` and
`20260908-081613-269648-33692-ctest.log` under `Build/.agent-state/logs/`.
Summaries and source/power-scheme/run-path receipts are
`Build/creation-baseline-power-a.json`, `Build/creation-baseline-power-b.json`,
and their `-receipt.json` companions. Measured source: `53671b1c4`.
Preserved Vulkan runs: `run-p34092-cd0fd431591a49028e00ac820f90183b` /
`run-p38240-012e2479bd054fc8a0470e3a834cdcc7`; material runs:
`run-p23204-986720c260e646058afb125789fcf87f` /
`run-p14792-b672efebf524477eb46dd2e4da72a0be`.
Inputs and metric sets match, but 21 primary request/native/frame medians exceed
5%, including compute batch request +11.26%, shader request +52.31%, declaration
request +54.21%, and first material frame +6.17%. This rejects the hypothesis that
these selected power/affinity settings alone provide a repeatable lane; it does
not establish a specific hardware, driver, or OS root cause.

The runner restored Balanced in `finally`, and an independent
`powercfg /getactivescheme` after terminal completion confirmed GUID
`381b4222-f694-41f0-9685-ff5bb260df2e`. No measurement process remains running.
The prerequisite has persisted across goal turns despite completed measurement
implementation and these bounded environment investigations. Stage 0 is blocked
on reproducible hardware evidence. No subsequent implementation stage or
performance acceptance gate is complete.

### Stage 1: Separate factories and synchronous scheduling

Depends on Stage 0.

- [x] Separate native error classification from executor scheduling; let the RHI
  facade explicitly select execution domains.
- [x] Construct vertex declarations directly, protect necessary caches with
  local locks, and retain failure-injection semantics.
- [x] Route unmigrated factories through facade compatibility paths, preserving
  complete-or-null behavior, error propagation, references, and execution order.
- [x] Verify vertex-declaration creation outside the RHI thread increases neither
  synchronous operations nor queue submissions, consistently in threaded/inline modes.

Completion: Vulkan factories do not implicitly access the global executor;
CPU description objects incur zero replay round trips.

#### Stage 1 implementation — 2026-09-08

`MakeVulkanCreationOperation` only adapts recoverable Vulkan native errors and
propagates opt-in timing into the invoked body. It neither executes at callback
construction nor selects a thread. `ExecuteFallibleRHICreationOperation` is a
caller-thread RHI error boundary reused by the synchronous executor; terminal
errors retain their original types. There is no replacement universal scheduling
wrapper. The backend-specific `FVulkanDynamicRHI::RHI*` facade explicitly selects
the existing context path for PSOs, shaders, samplers, buffers, textures, and views.
Inline execution, already-on-replay execution, payload accounting, initial buffer
uploads, texture initialization, and complete-or-null results retain their contracts.

Vertex declarations copy immutable elements directly, with no device/cache mutation
or new cache lock. Failure injection remains recoverable. The focused deterministic
test holds the sole replay thread behind a gate while another thread creates a
declaration; it also checks copied element ownership, failure/recovery, and unchanged
synchronous-operation/submission counts in threaded and inline modes. Native-error
type tests also pass (2/2):
`Build/.agent-state/logs/20260908-212953-666055-8212-VulkanRHIIntegrationTests.log`.
Affected-suite validation passed 80/81 targets, including all RHI/Vulkan/Renderer
targets (`Build/.agent-state/logs/20260908-213244-124364-10968-ctest.log`). The
untouched CPU-only `FReflectedPropertyViewTests.GenericStructRendersEditableFields`
expects row 3 but receives 36; a focused rerun reproduces it independently
(`Build/.agent-state/logs/20260908-213608-692560-24136-EditorPropertyTests.log`).
The existing `FBodyInstance` now exposes nested collision-response fields and
`FPropertyView::EditStructProperty` expands editable fields; neither path invokes
the changed resource factories, and neither file was modified by this stage.
The aggregate is therefore not reported as fully passing. No elapsed-time
performance claim is made. Only whitespace formatting changed after validation.

### Stage 2: Make PSO construction independent of replay

Depends on Stage 1.

- [x] Separate immutable inputs, dependency acquisition, native creation, and
  cache publication.
- [x] Implement concurrency protocols and capacity reservation for PSOs,
  dependency caches, statistics, LRU, and the driver cache.
- [x] Replace construction/destruction thread assertions according to actual
  resource requirements. Failed/duplicate candidates must respect GPU lifetime,
  and retained references must prevent eviction and destruction.
- [x] Validate graphics/compute PSOs through controlled background creation,
  including native failure, full caches, concurrent lookup/eviction, and
  independent replay progress during compilation.

Completion: Native PSO creation runs independently of replay, with shared state
and release paths validated.

#### Stage 2 implementation — 2026-09-08

Graphics and compute candidates now receive copied immutable inputs with strong
shader/declaration references and explicitly acquired render-pass/layout
dependencies. Cache managers own lookup and publication. One creator domain
serializes misses; ready lookup, reference acquisition, LRU, reservation, and
publication share the statistics transaction lock. Full-cache admission extracts
a cache-only victim before compilation and retains its node for rollback. No
table lock is held while waiting for the creator or compiling a native PSO.

Driver-cache creation, graphics/compute compilation, serialization, and
destruction use a separate exclusive mutex. The current Vulkan cache has default
flags; Vulkan allows internally synchronized compilation in that configuration.
The explicit exclusive domain is a conservative engine policy for the selected
single-creator design, rather than a claim that all Vulkan cache compilation
requires external synchronization. See the
[Vulkan pipeline-cache contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreatePipelineCache.html).
Render-pass lookup/publication has a dedicated mutex and a 1,024-entry resident
limit; descriptor-set layouts retain their mutex and now have a 4,096-entry limit.
Statistics return a locked copy, with scoped mutable access for cache accounting.

Structural-layout construction has its own mutex and releases the statistics
transaction before entering descriptor-set-layout dependencies. Driver and table
locks never nest. Native PSO construction no longer asserts replay ownership;
published PSO destruction still does, because it invalidates context state and
enqueues GPU-token retirement. Final RHI reference release uses an intrusive
pending-delete node reserved in each resource. Gathering reserves destination
storage before removing queue ownership. Render-pass and descriptor-layout
publication failures destroy never-published native candidates directly, and
render-pass errors retain their original native error type.

`BackgroundPipelinesPreserveReplayProgressAndCapacityReservations` passes for
graphics and compute on controlled background threads. It holds compilation at
the driver boundary, verifies concurrent ready hits and replay progress, checks
same-key sharing, saturates a reduced two-entry cache while results are retained,
injects native layout failure after victim reservation, confirms restoration,
and checks successful eviction and exact counters. A separate CPU lifetime test
releases 256 children on four threads and verifies all 512 child/parent objects
are destroyed only on the deletion owner, across two queue drains.

The affected run exposed a pre-existing borrowed-temporary declaration in
`GeometrySubmissionTestSupport.h`: the helper discarded the factory's only
strong reference before PSO creation. It now retains that reference through
construction. The static-mesh integration target then passed all five cases.

Validation receipts:

- Full Vulkan integration: 71/71 cases in
  `Build/.agent-state/logs/20260908-220457-446949-13432-VulkanRHIIntegrationTests.log`.
- Final affected run: 80/81 targets in
  `Build/.agent-state/logs/20260908-220548-669832-11896-ctest.log`;
  only the unchanged property-editor failure documented in Stage 1 remains.
- After separating the structural-layout lock, both the controlled background
  and recoverable-factory regression cases passed in
  `Build/.agent-state/logs/20260908-220854-222904-38360-VulkanRHIIntegrationTests.log`.

Public synchronous PSO facades retain compatibility scheduling until the
request-service stage. Device-owned service admission and joining remain Stage 3
work; the controlled tests join every creator before device shutdown. No timing
improvement is inferred from these correctness tests.

### Stage 3: Add bounded asynchronous and batch PSO requests

Depends on Stage 2.

Implementation policy: Core task construction returns directly and treats
construction/lifetime violations as terminal assertions; ordinary scheduler
saturation queues work. Recoverable request rejection therefore happens at the
device budget/description/admission boundary before Core construction. The
service owns a worker-only `FTaskScope` and borrows its token for its task group;
the scope owner provides explicit close/join authority. Core rejects creation
of roots/completion sources from a foreign executing task scope. A small
read-only context query exposes that existing rule so requests reject as
Unsupported before construction, without changing scheduler policy. Requests
originate on ordinary owner threads; scoped workers consume issued handles.
The service closes admission before closing its group.
Pending synchronous waits reject Core workers as well as replay and creation
workers, preventing a one-worker scheduler from being blocked by its own caller.
Domain waits on other caller threads observe Core completion under the service
notification lock; no creation operation requires game-thread or replay work.
Old request handles retain completion metadata after close, while their shared
publication records relinquish RHI resources before device destruction.

- [x] Implement request states, shared Pending, batch results, completion
  notification, and synchronous compatibility entry points.
- [x] Integrate Core groups, payload/result budgets, and bounded admission;
  implement observer cancellation, shutdown, stale-generation rejection, and
  separate candidate errors from device faults.
- [x] Verify one native creation for concurrent same-key requests and zero replay
  round trips for Ready hits. Cover admission rejection, partial batch failure,
  cache pressure, and terminalization of all requests at shutdown.

Completion: Requests return independently and batches progress without dangling
captures, unbounded queuing, or permanent Pending states.

#### Stage 3 handoff — 2026-09-08

Added device-owned asynchronous graphics/compute requests, per-item batches,
independent cancellation, shared Pending creation, and startup rejection without
Core. Synchronous Ready lookup makes no replay submission; misses wait only on
permitted callers. Owned payloads, count limits, 16 MiB unfinished storage, and
64 MiB Vulkan cache metadata reservations precede publication. The metadata
reservation follows retained PSO/layout lifetime, including owner retirement.
Device close cancels queued observations and joins native work before shutdown;
old handles cannot publish resources into another device lifetime.

Core task construction retains its existing scope/lifetime policy. The only
Core addition is a read-only scope-context compatibility query; foreign scoped
callers receive Unsupported at the RHI boundary. Completion handles carry no
native references. Request owners share a separate publication record retired
at close. Native terminal exceptions notify the executor/serial waiters before
Core captures them and are rethrown by result access. Ordinary candidate errors
remain per-request Failed outcomes. Command dependency dispatch follows in Stage 4.

Validation (`Win64-Debug-DurinEditor`): Vulkan integration passed 72/72,
including 16 concurrent graphics/compute observers, one worker, threaded/inline
replay progress, zero Ready-hit submission deltas, metadata pressure, and old
handles after shutdown. Request-service tests passed 7/7 and cover partial batches, failure
retry, cancellation, count/payload saturation, foreign scopes, and close/join.
RHI thread tests passed 12/12; command-list tests passed 68/68 after preserving
ordinary failed-thread rejection/retry separately from terminal device failure.
The final affected selection passed 81/82 targets; the sole failure remains the
pre-existing property-editor row-index assertion recorded in Stage 1.

Receipts:
- `Build/.agent-state/logs/20260908-230815-879831-41456-RHIPipelineCreationTests.log`
- `Build/.agent-state/logs/20260908-230213-629894-10060-VulkanRHIIntegrationTests.log`
- `Build/.agent-state/logs/20260908-225503-891017-30368-RHIThreadTests.log`
- `Build/.agent-state/logs/20260908-230133-455943-39420-RHICommandListTests.log`
- `Build/.agent-state/logs/20260908-230427-149920-16444-ctest.log`

### Stage 4: Migrate Renderer readiness and command dependencies

Depends on Stage 3.

- [x] Migrate static-mesh material PSOs and fixed graphics/compute Renderer
  creation callers. Establish prewarm triggers and compatible fallbacks while
  preserving complete transactional publication by each owning slot.
- [x] Retain necessary request/result references in recorded commands and add
  scheduling-layer readiness dependencies and failure terminal states. Do not
  wait for future creation tasks on replay.
- [x] Verify compute binding validation and shader/layout metadata availability
  during Pending; do not expose unfinished native handles to record commands.
- [x] Validate recording order, submission serials, fences, rejection/retry,
  inline mode, cycles, late prewarming, shader updates, device invalidation,
  and late arrival of old candidates.

Completion: Migrated callers no longer submit and wait for each resource.
Unready resources have explicit consumption policies; dependency failure cannot
execute invalid draw/dispatch commands or prevent fence termination.

#### Stage 4 handoff — 2026-09-08

Migrated fixed Renderer and static-mesh PSOs to retained asynchronous slot
requests. Pending attempts preserve compatible payloads, retry without false
failure diagnostics, and discard obsolete device/generation observations.
Bare preparation is nonblocking. Scene submission collects required first-use
requests and joins them on the render owner before graph authoring; complete
single-shot outputs remain supported. Batches retain at most 4,096 observations
and allow 64 preparation rounds. Compatible refreshes do not join. GBuffer,
GTAO, deferred/debug, and cloud temporal/composite creation moved before graph
execution. This is an explicit first-consumer wait, not a zero-latency claim.

Recorded requests retain immutable layout metadata and counted observations.
Bounded scheduling dependencies preserve FIFO, skip failed/canceled batches,
and expose durable fence outcomes without waiting on replay. Old-serial lookup
uses 4,096 bounded receipts; overwritten history returns Expired. Render fences
capture receipts before later submissions. Inline waits reject replay/worker
cycles; rejection keeps command ownership. Device close joins creation before
replay drains, then retires Ready publications before native teardown. Metadata
leases live in RHI so old layout aliases survive backend unload. Vulkan shaders
now own entry-point strings; broad asynchronous tests exposed the old borrowed
string lifetime, which was corrected rather than suppressing validation.

Validation: command-list tests passed 70/70, resource-slot tests 11/11, real
Vulkan async compute recording passed threaded/inline, and actual render-thread
prewarm passed pixel readback. The final affected selection passed 81/82 targets,
including Vulkan, service/thread contracts, material/texture single-shot renders,
static-mesh preparation, sky and cloud integration. The sole failure remains
the unchanged property-editor assertion recorded in Stage 1. Earlier broad
failures from missing first-consumer preparation and borrowed entry points were
fixed; they are not accepted limitations.

Receipts:
- `Build/.agent-state/logs/20260908-233759-516057-31944-RenderContractTests.log`
- `Build/.agent-state/logs/20260908-234543-318627-11972-RHICommandListTests.log`
- `Build/.agent-state/logs/20260908-234155-840281-38056-RendererResourceReloadVulkanTests.log`
- `Build/.agent-state/logs/20260908-235416-892436-3492-VolumetricCloudSceneVulkanTests.log`
- `Build/.agent-state/logs/20260908-235513-202287-37532-ctest.log`

### Stage 5: Migrate remaining resource creation domains

Depends on Stage 4 and the Stage 0 resource inventory.

- [x] Migrate shader/sampler/buffer/texture creation according to the inventory.
  Separate native creation from uploads/state transitions and define each
  class's batch and data-readiness contracts.
- [x] Validate allocator wrappers, mapped ranges, staging, statistics, tracking,
  and reclamation. Retain explicit facade scheduling for context operations;
  do not claim all factories support arbitrary-thread calls.
- [x] Remove the universal synchronous wrapper and unused bridges. Document each
  retained synchronous entry, its reason, and thread preconditions.
- [x] Validate candidate cleanup after partial batch failure, upload visibility,
  device shutdown, and consumer lifetime, preserving Renderer/Render Graph
  transactional boundaries.

Completion: Every inventoried factory has a validated execution domain, and
remaining synchronization has explicit contracts.

#### Stage 5 handoff — 2026-09-09

Shader/sampler/buffer/local-texture and stable-view native creation now execute
on the caller. Upload bytes remain copied into recorded buffer/texture commands;
initial storage-texture transitions and mapped/state/transfer-arena operations
retain their owners. VMA internal synchronization, object-local allocation state,
mutex-protected memory statistics and view caches, and atomic naming counters
were audited. External image views retain explicit replay scheduling because
swapchain backing can change. Other retained context entries and each category's
ordered batch/data-readiness contract are published in RendererResourceRecovery.

Added post-native publication failure injection and immediate rollback for all
six native categories. Published resources still retire on the owner after GPU
completion. Six factory types ran from 16 concurrent callers while replay was
held, in threaded and inline modes, with zero synchronous-operation/submission
deltas. Failure injection verified complete-or-null results and allocation/byte
counts returning to baseline after normal retirement. Automatic view-cache tests
now assert zero round trips for first creation as well as reuse.

Focused Vulkan tests passed 2/2; the final affected selection passed 81/82
including all Vulkan upload/readback, allocator/mapped-range, view lifetime,
Renderer/RDG transaction and shutdown coverage. The sole aggregate failure is
the unchanged property-editor assertion recorded in Stage 1.

Receipts:
- `Build/.agent-state/logs/20260909-000414-490059-9588-VulkanRHIIntegrationTests.log`
- `Build/.agent-state/logs/20260909-000444-201530-9092-ctest.log`

### Stage 6: Qualify performance and publish contracts

Depends on Stages 0–5.

- [x] Rerun baselines and compare median/p95, synchronous round trips, native
  creations, replay progress, first-consumer waits, and peak memory. Record
  serial background and selected concurrency configurations.
- [x] Use controlled slow-creation fixtures to verify CPU preparation/native
  creation do not occupy replay. Deterministic coverage cannot substitute for
  actual driver performance measurements.
- [x] Complete failure/concurrency/shutdown tests and real Vulkan scenarios
  within Stage 0 budgets; compile other affected backends and verify necessary
  contracts without regressions.
- [x] Publish implemented contracts in owning Runtime documents and correct PSO
  identity descriptions. Update status, evidence, and open items here; mark
  Completed only after every gate passes.

Completion: Correctness gates pass and actual performance comparisons report
supported outcomes or explicit uncertainty under the approved measurement policy;
lasting contracts do not depend on this plan's body.

#### Stage 6 handoff — 2026-09-09

The final Windows Debug qualification used the i7-12700, RTX 3090, driver
2480242688, Vulkan API 4211013, active validation/debug utils, original Balanced
power scheme, default affinity (1048575) and default process power controls.
The background configuration uses one Core worker and one native creator;
producer configurations are serial admission and 16 simultaneous callers.
Higher creator concurrency is not implemented or qualified. Application caches
are cold per device lifetime except the specified hot cases; driver-private
caches remain uncontrolled. No other RHI backend exists in this checkout;
Windows Vulkan is compiled and tested, with no cross-platform qualification claim.

Measurements include 11,820 legacy PSO calls, 13,440 resource creations,
3,630 material frames, 60,000 empty instrumentation observations, and a separate
300-round async suite containing 11,820 observers and 4,920 native creations.
The summarizer validates counts, phase ordering, native/observer key correlation,
memory sampling availability, and median/p95 comparisons. Every async native
record belongs to its creator thread; Ready observation is a sequential caller
timestamp, not publication time. Creator queue fields exclude service queuing.
Async admission and whole-batch first-consumer waits have separate boundaries
and must not be compared as if they were legacy synchronous call durations.

All 300 legacy PSO rounds recorded zero synchronous operations and zero creation
submission serials. Hot rounds created no native pipelines; duplicate-16 rounds
created exactly one per key, distinct-16 exactly 16, and batch-64 exactly 64.
All async rounds recorded zero synchronous operations and exactly one independent
replay marker. Resource rounds retain one explicit upload-completion wait;
Stage 5's blocked-replay factory fixture independently proves zero factory round
trips, including declarations. The controlled compilation hook fixtures
`BackgroundPipelinesPreserveReplayProgressAndCapacityReservations` and
`PublicAsyncPipelinesShareCreationAndKeepReplayAvailable` passed again in the
final affected run. They prove independence while native work is deliberately
held; actual driver measurements below are separate evidence.

**Comparable legacy observations.** Values are microseconds, median / p95.
The baseline is `Build/creation-baseline-complete-a.json`; the final summary is
`Build/creation-refactor-final-comparison.json`. PSO seeds, all three shaders,
resource data and resource shader hashes match. Material shaders also match.
The material scene receipt hash differs only because two host-observation lines
(`process_affinity` and `process_power_control`/`process_power_state`) were added;
the complete scene specification and other host/device lines are identical.
The raw hash mismatch is retained rather than rewritten as equality.

| Metric | Baseline | Refactor | Observed outcome |
| --- | ---: | ---: | --- |
| Hot graphics request | 36.40 / 49.10 | 8.80 / 11.00 | Lower by 27.60 / 38.10 us; zero round trips is independently verified |
| Hot compute request | 26.50 / 31.60 | 2.60 / 7.90 | Lower by 23.90 / 23.70 us; zero round trips is independently verified |
| Cold graphics native | 5666.15 / 6766.10 | 7448.55 / 8095.10 | Higher by 1782.40 / 1329.00 us; exceeds budget |
| Cold compute native | 2382.45 / 3023.10 | 2849.65 / 3641.60 | Higher by 467.20 / 618.50 us; exceeds budget |
| Buffer request | 30.60 / 61.90 | 9.20 / 26.10 | Lower by 21.40 / 35.80 us |
| Declaration request | 18.30 / 20.20 | 1.00 / 2.70 | Lower by 17.30 / 17.50 us |
| Material first frame | 322030.95 / 347337.20 | 403605.90 / 422119.00 | Higher by 81574.95 / 74781.80 us; exceeds budget |
| Material following 120 frames | 2999.90 / 3895.90 | 3787.60 / 5264.10 | Higher by 787.70 / 1368.20 us; exceeds budget |

The two post-refactor PSO runs reproduce the direction of the cold slowdown,
but the historical baseline was not stable. These are observed regressions and
budget exceedances, not evidence that the refactor alone caused the entire
driver/frame difference. No cold/frame non-regression or universal speedup is
claimed. Instrumentation remains opt-in; final raw overhead samples and timer
resolution are retained. The approved uncertainty policy does not increase
budgets or transform a budget exceedance into a performance pass.

**Async first use and replay.** In the selected serial-creator run, batch-64
graphics observer admission was 63.75 / 185.60 us per request; the whole-batch
first-consumer wait was 420850.20 / 430849.50 us. Compute values were
48.20 / 172.60 us and 170914.95 / 180757.40 us. Independent marker latency was
69.90 / 86.20 us for graphics and 59.10 / 88.80 us for compute. Cold single-request
waits were 7141.20 / 8412.00 us and 3118.45 / 4087.00 us respectively. These
results demonstrate the real first-use cost; there is no pre-refactor async API
baseline for claiming an async admission speedup. Hot async observations also
pay service bookkeeping, unlike the direct synchronous Ready fast path.

**Memory outcome.** Device-local and nonlocal sampled peaks match the baseline
in the comparable PSO, resource and material scenarios. Process private memory
does not pass the original budget. After preserving the original test order,
resource-batch maximum is 860,286,976 bytes versus 446,980,096; cold-compute
maximum is 603,271,168 versus 388,673,536; material first-frame maximum is
485,253,120 versus 436,174,848. These are recorded process-memory regressions;
attribution among engine, allocator, validation and driver retention across
repeated device lifetimes remains unresolved. Passing bounded metadata and
resource-retirement tests does not explain or waive these process peaks.
Async cache metadata peaked at 494,356 bytes, below 64 MiB. Counts, payload,
observer admission, cancellation, rollback, retirement and shutdown capacities
passed deterministic coverage. The async suite runs after the legacy groups so
its additional 300 device lifetimes cannot contaminate their process peaks.
The initial mixed-order run is retained as diagnostic evidence, not selected
as the comparable resource-memory result.

**Validation and disposition.** Vulkan qualification passed 4/4, the isolated
legacy-order confirmation passed 3/3, and the material qualifier passed its
3,630-frame correctness/readback checks. The final affected selection passed
81/82 targets. Its only failure remains the unchanged
`FReflectedPropertyViewTests.GenericStructRendersEditableFields` assertion
recorded in Stage 1. There are no newly failing functional targets. Failure,
concurrency, fence outcomes, owner waits, inline execution and shutdown coverage
all passed. Implementation and the required qualification/reporting work are
complete under the revised policy; cold/frame timing and process-memory
non-regression remain unqualified, with the concrete exceedances above retained
for further performance attribution. Lasting contracts are in
RendererResourceRecovery, including identity reuse and measurement boundaries.

Receipts:
- `Build/.agent-state/logs/20260909-001140-175301-42384-ctest.log` (material passed; Vulkan setup rejected empty affinity before sampling)
- `Build/.agent-state/logs/20260909-001310-925193-31600-ctest.log` (Vulkan 4/4)
- `Build/.agent-state/logs/20260909-002205-942075-27576-ctest.log` (original three-group order)
- `Build/.agent-state/logs/20260909-001825-202653-37724-ctest.log` (affected 81/82)
- `Build/creation-refactor-comparison.json` (initial mixed-order diagnostics)
- `Build/creation-refactor-final-comparison.json` (final median/p95 and async summary)

Raw work directories below are under
`Engine/Binaries/Win64/Debug/Tests/DurinEditor/`:
- `VulkanCreationQualificationTests/Work/Runs/run-p33232-204524737ca14ab588b19f9e79a07ba1` (final legacy/resource/overhead)
- `VulkanCreationQualificationTests/Work/Runs/run-p42580-d3fd927bd8844b6c9da4aeef686b3411` (async and retained initial measurements)
- `MaterialCreationQualificationTests/Work/Runs/run-p42452-1bd2157c082f4e70b722895a9dc17b68` (material)

#### Stage 6 repeat qualification — 2026-09-09 03:48

At the user's request, reran the bounded RHI-creation qualification selection
without code changes, at commit `17f0ffc3c`. Both targets passed (Vulkan 4/4 and
material 1/1), in 167.02 seconds of test execution. The same Windows Debug,
Balanced power, default affinity/QoS, GPU/driver, validation and cache settings
were retained. Seed, PSO shader, resource input and complete material receipt
hashes match the previous post-refactor run. Legacy Vulkan groups run before
the added async group; GPU-owning targets share the CTest resource lock.
The summarizer accepted all fixed sample counts and async identity/round-trip
checks. This repeats qualification only; no unrelated functional suite was rerun.

Values below are microseconds, median / p95; the previous column is the final
Stage 6 legacy-order measurement, not the contaminated mixed-order resource run.

| Metric | Pre-refactor | Previous refactor | Repeat |
| --- | ---: | ---: | ---: |
| Hot graphics request | 36.40 / 49.10 | 8.80 / 11.00 | 8.60 / 9.00 |
| Hot compute request | 26.50 / 31.60 | 2.60 / 7.90 | 2.60 / 3.70 |
| Cold graphics native | 5666.15 / 6766.10 | 7448.55 / 8095.10 | 5674.35 / 6988.30 |
| Cold compute native | 2382.45 / 3023.10 | 2849.65 / 3641.60 | 2341.85 / 2586.70 |
| Buffer request | 30.60 / 61.90 | 9.20 / 26.10 | 8.70 / 17.40 |
| Declaration request | 18.30 / 20.20 | 1.00 / 2.70 | 1.00 / 1.80 |
| Material first frame | 322030.95 / 347337.20 | 403605.90 / 422119.00 | 321285.55 / 327830.00 |
| Material following 120 frames | 2999.90 / 3895.90 | 3787.60 / 5264.10 | 3084.95 / 4960.00 |

Cold graphics median is now +0.14% versus baseline, cold compute -1.70%, and
first material frame -0.23%. The previous 20–31% cold/first-frame slowdown did
not reproduce; it cannot be treated as a demonstrated persistent code regression.
Following-frame median is +2.84%, but p95 remains +27.32% (+1.0641 ms), exceeding
the unchanged budget. The two post-refactor runs themselves differ materially;
this one improved run does not establish universal stability or acceptance.

Process memory remains elevated. Resource-batch private maximum is now
888,217,600 bytes, versus previous 860,286,976 and baseline 446,980,096
(approximately 847 / 820 / 426 MiB). Material first-frame maximum is
518,545,408 bytes versus previous 485,253,120 and baseline 436,174,848.
The memory excess therefore reproduced; its cause remains unassigned.
Async batch-64 whole-consumer waits were 290145.20 / 296980.20 us for graphics
and 118368.15 / 127101.90 us for compute; independent replay markers were
40.60 / 59.10 and 38.80 / 92.40 us. Metadata maxima remain 494,356 and 376,448
bytes respectively. No request/native boundary was redefined for comparison.

Receipts:
- `Build/.agent-state/logs/20260909-034806-424244-7532-ctest.log`
- `Build/creation-refactor-repeat-comparison.json`
- `Engine/Binaries/Win64/Debug/Tests/DurinEditor/VulkanCreationQualificationTests/Work/Runs/run-p21592-57be3efbb4bc4ae6ae23814345354726`
- `Engine/Binaries/Win64/Debug/Tests/DurinEditor/MaterialCreationQualificationTests/Work/Runs/run-p11144-afa09bec2a6c4c88b79ca6332eb06a62`

## Validation and Handoff

Read the [Build and Run workflow](../../../Agents/BuildAndRun.md) before building or
running, and the [Testing workflow](../../../Agents/Testing.md) before selecting native
tests. Do not freeze build commands here that may become stale. Documentation-
only changes run document and applicable plan validation without claiming
runtime validation.

Each stage records actual changes, execution configuration, test/measurement
receipts, and limitations. After successful validation, commit according to
repository rules with this plan's path and exact stage title as Plan/Stage trailers.

## Related Code

- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRHIPrivate.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanShader.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- [Renderer resource recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [Render resource lifecycle](../../../Runtime/Rendering/RenderResourceLifecycle.md)
