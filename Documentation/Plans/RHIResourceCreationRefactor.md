# RHI Resource Creation Refactor Plan

Summary: Decouple resource factories from command replay, introduce bounded asynchronous PSO creation and shared requests, and migrate resource consumers with explicit readiness and lifetime contracts.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

Stage 0 has completed the creation-entry audit, identity-contract correction,
interface and budget decisions, and measurement-protocol preparation. No
performance baseline exists; Stage 0 completion has not passed, and Stages 1–6
have not started. On 2026-09-07, the user confirmed that a quiet GPU environment
was unavailable and limited this work to audit and measurement preparation.
Existing Vulkan correctness coverage passed; test duration and counters cannot
substitute for a performance baseline.

`ExecuteFallibleVulkanCreationOperation` currently accesses the global executor.
With the RHI thread enabled, calls from other threads enqueue work and wait for
its serial. PSO cache lookup occurs inside that operation, so hits also take
the synchronous path. Vertex declarations mainly copy CPU data but use the same
path. Calls on the RHI thread execute directly; disabling it runs inline. This
CPU work wait is neither GPU idle nor an automatic submission of commands that
have been recorded but not submitted.

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
  [CPU Task System](../Runtime/Core/TaskSystem.md), without another generic
  future framework. Its API was qualified by the archived
  [Async Task Framework Refactor](Archive/2026-09/AsyncTaskFrameworkRefactor.md);
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
- [ ] Establish comparable cold/hot PSO, duplicate-key, batch-resource, and material
  first-use baselines. Separately record request queuing, CPU preparation, native
  creation, consumer waits, frame time, and peak memory. Record hardware, driver,
  build configuration, concurrency, cache state, and sample counts.
- [x] Freeze comparisons, noise tolerance, and acceptable regression budgets
  before runtime changes rather than adjusting them afterward.

Completion: Execution domains and interface decisions are consistent, and
measurements are reproducible. Items without hardware evidence stay open;
counter-based inference cannot replace measured durations.

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
[recovery contract](../Runtime/Rendering/RendererResourceRecovery.md) is corrected.

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
`Threading/TaskComposition.h` include `FTaskGroup::TryCreate/Close/JoinAsync`,
`TrySpawn`, `Share`, `TCompletionSource::TryCreate`, `GetCompletion`, and
capture/result byte estimates. Use a device-owned counted group, shared
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

### Stage 1: Separate factories and synchronous scheduling

Depends on Stage 0.

- [ ] Separate native error classification from executor scheduling; let the RHI
  facade explicitly select execution domains.
- [ ] Construct vertex declarations directly, protect necessary caches with
  local locks, and retain failure-injection semantics.
- [ ] Route unmigrated factories through facade compatibility paths, preserving
  complete-or-null behavior, error propagation, references, and execution order.
- [ ] Verify vertex-declaration creation outside the RHI thread increases neither
  synchronous operations nor queue submissions, consistently in threaded/inline modes.

Completion: Vulkan factories do not implicitly access the global executor;
CPU description objects incur zero replay round trips.

### Stage 2: Make PSO construction independent of replay

Depends on Stage 1.

- [ ] Separate immutable inputs, dependency acquisition, native creation, and
  cache publication.
- [ ] Implement concurrency protocols and capacity reservation for PSOs,
  dependency caches, statistics, LRU, and the driver cache.
- [ ] Replace construction/destruction thread assertions according to actual
  resource requirements. Failed/duplicate candidates must respect GPU lifetime,
  and retained references must prevent eviction and destruction.
- [ ] Validate graphics/compute PSOs through controlled background creation,
  including native failure, full caches, concurrent lookup/eviction, and
  independent replay progress during compilation.

Completion: Native PSO creation runs independently of replay, with shared state
and release paths validated.

### Stage 3: Add bounded asynchronous and batch PSO requests

Depends on Stage 2.

- [ ] Implement request states, shared Pending, batch results, completion
  notification, and synchronous compatibility entry points.
- [ ] Integrate Core groups, payload/result budgets, and bounded admission;
  implement observer cancellation, shutdown, stale-generation rejection, and
  separate candidate errors from device faults.
- [ ] Verify one native creation for concurrent same-key requests and zero replay
  round trips for Ready hits. Cover admission rejection, partial batch failure,
  cache pressure, and terminalization of all requests at shutdown.

Completion: Requests return independently and batches progress without dangling
captures, unbounded queuing, or permanent Pending states.

### Stage 4: Migrate Renderer readiness and command dependencies

Depends on Stage 3.

- [ ] Migrate static-mesh material PSOs and fixed graphics/compute Renderer
  creation callers. Establish prewarm triggers and compatible fallbacks while
  preserving complete transactional publication by each owning slot.
- [ ] Retain necessary request/result references in recorded commands and add
  scheduling-layer readiness dependencies and failure terminal states. Do not
  wait for future creation tasks on replay.
- [ ] Verify compute binding validation and shader/layout metadata availability
  during Pending; do not expose unfinished native handles to record commands.
- [ ] Validate recording order, submission serials, fences, rejection/retry,
  inline mode, cycles, late prewarming, shader updates, device invalidation,
  and late arrival of old candidates.

Completion: Migrated callers no longer submit and wait for each resource.
Unready resources have explicit consumption policies; dependency failure cannot
execute invalid draw/dispatch commands or prevent fence termination.

### Stage 5: Migrate remaining resource creation domains

Depends on Stage 4 and the Stage 0 resource inventory.

- [ ] Migrate shader/sampler/buffer/texture creation according to the inventory.
  Separate native creation from uploads/state transitions and define each
  class's batch and data-readiness contracts.
- [ ] Validate allocator wrappers, mapped ranges, staging, statistics, tracking,
  and reclamation. Retain explicit facade scheduling for context operations;
  do not claim all factories support arbitrary-thread calls.
- [ ] Remove the universal synchronous wrapper and unused bridges. Document each
  retained synchronous entry, its reason, and thread preconditions.
- [ ] Validate candidate cleanup after partial batch failure, upload visibility,
  device shutdown, and consumer lifetime, preserving Renderer/Render Graph
  transactional boundaries.

Completion: Every inventoried factory has a validated execution domain, and
remaining synchronization has explicit contracts.

### Stage 6: Qualify performance and publish contracts

Depends on Stages 0–5.

- [ ] Rerun baselines and compare median/p95, synchronous round trips, native
  creations, replay progress, first-consumer waits, and peak memory. Record
  serial background and selected concurrency configurations.
- [ ] Use controlled slow-creation fixtures to verify CPU preparation/native
  creation do not occupy replay. Deterministic coverage cannot substitute for
  actual driver performance measurements.
- [ ] Complete failure/concurrency/shutdown tests and real Vulkan scenarios
  within Stage 0 budgets; compile other affected backends and verify necessary
  contracts without regressions.
- [ ] Publish implemented contracts in owning Runtime documents and correct PSO
  identity descriptions. Update status, evidence, and open items here; mark
  Completed only after every gate passes.

Completion: Correctness and predetermined performance gates have evidence;
lasting contracts do not depend on this plan's body.

## Validation and Handoff

Read the [Build and Run workflow](../Agents/BuildAndRun.md) before building or
running, and the [Testing workflow](../Agents/Testing.md) before selecting native
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
- [Renderer resource recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Render resource lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
