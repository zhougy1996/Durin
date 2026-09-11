# RDG and RHI Multi-Queue Execution Plan

Summary: Introduce explicit GPU queue submission, completion, and resource retirement contracts across RDG, RHI, and Vulkan, then enable asynchronous compute, split barriers, and safe transient aliasing.

Last reviewed: 2026-09-11

Status: Active
Completed:

## Current Status

This plan records the selected architecture; implementation has not started.
Stage 0 is the next stage. No multi-queue behavior or acceptance gate is complete.

The logical barrier foundation landed in commit `7d5812720`:
`FRDGBufferTransition` and `FRDGTextureTransition` carry resource IDs and
transition descriptions together; `FRDGBarrierBatch` is shared by passes and
the final epilogue. `RecordBarrierBatch` resolves physical backings without
mutating the compiled plan. This foundation passed the default-profile `all`
build, 160 RenderContractTests, 54 RendererSceneContractTests, and the Vulkan
graph-transition replay case. Those results do not qualify the future work
in this plan.

The current executor's command-batch serial tracks CPU replay completion.
Vulkan separately tracks submitted GPU work through completion tokens and
submission fences. The existing graph and backend path still rely on a
single-queue execution model. Extending this requires coordinated scheduling,
submission, synchronization, and lifetime changes.

## Goal

Support explicit multi-queue execution while preserving deterministic RDG
compilation, correct resource lifetimes, and the existing single-queue path.
Keep logical scheduling independent of physical resource preparation and
backend synchronization objects. Establish completion and retirement semantics
before enabling asynchronous execution.

## Scope and Selected Decisions

- Cover RenderCore, RHI, VulkanRHI, Renderer allocation/reuse, and their native
  tests. Migrate consumers across every project in `Durin.dworkspace` whenever
  shared APIs change.
- Separate pass domain, pipeline stages, and physical queue identity. A compute
  pass may execute on a graphics queue. Async eligibility and scheduling policy
  must not imply hardware queue availability.
- Preserve the current barrier records as logical data. Extend them or add a
  separate resource-handoff record for queue synchronization; never restore
  parallel transition/resource arrays or place mutable physical pointers in
  the compiled plan.
- Represent a GPU completion point with device generation, queue identity, and
  a queue-local monotonically increasing value. CPU command-batch completion,
  logical graph positions, and GPU completion points must be distinct types.
- Represent retirement prerequisites as a set of queue completion points,
  retaining the maximum value only within the same queue and device generation.
  A larger value on another queue is not evidence of completion.
- Allow an explicit join to produce an aggregate completion point after waiting
  for all inputs. Do not impose a global join on every resource or submission.
  Any later removal of redundant prerequisites requires proven dependency
  coverage, not a numeric comparison across queues.
- Submission batches own all referenced resources until ownership transfers
  safely to backend in-flight work. Commands recorded but not yet submitted
  also retain their resources. Include views, descriptors, backing allocations,
  upload storage, and command allocators in the ownership audit.
- Resource destruction or CPU overwrite requires actual completion of relevant
  GPU uses. Planned transient aliasing instead requires a proven execution
  order and the necessary backend barriers; it does not require a CPU wait.
- Queue waits/signals, resource access/layout transitions, ownership transfers,
  and split-barrier begin/end are separate concepts with an explicit protocol.
  Backends may lower a logical handoff differently according to capabilities.
- Keep synchronous fallback available when independent compute queues or split
  barriers are unavailable. Supported logical behavior must not depend on a
  particular device's queue topology.
- Treat graph extraction as publication of a retained resource reference, not
  proof that its GPU contents are complete. Cross-graph consumers must inherit
  or explicitly consume the required readiness dependencies.

Multi-adapter scheduling and additional graphics backends are outside this
plan. Queue identities must accommodate multiple queues of the same class,
but initial production scheduling targets graphics and compute. A dedicated
copy queue remains disabled until its ownership and transfer cases are covered.
UE provides architectural references, not source or API compatibility targets.

## Implementation Stages

### Stage 0: Freeze Queue, Completion, and Ownership Contracts

Dependencies: none beyond the existing logical barrier foundation.

Outcome: a reviewed ownership and submission contract with an exact migration
inventory and named validation gates, before implementation changes behavior.

- [ ] Trace RDG preparation/recording, RHI replay, Vulkan submission, resource
  release, descriptor recycling, upload storage, and Renderer allocator reuse.
  Identify which lifetime decisions currently depend on implicit queue order.
- [ ] Specify queue capability discovery and physical queue identity, including
  graphics/compute sharing one underlying queue and distinct queue families.
- [ ] Specify GPU completion point creation, publication, polling, ownership,
  device-generation validation, and shutdown behavior. Determine how points
  reserved before submission distinguish pending, submitted, failed, and
  canceled work without advancing a completion watermark falsely.
- [ ] Specify transaction boundaries for submission rejection and partial graph
  submission: already-submitted work stays alive until completion; unsubmitted
  work releases references only when it can no longer execute. Device loss
  follows an explicit teardown path rather than normal completion.
- [ ] Select where retirement metadata lives: resource/allocation state,
  in-flight batch ownership, or a documented combination. Avoid competing
  authorities and per-resource hardware fences.
- [ ] Define cross-graph extraction/import readiness, including external work,
  and distinguish resource object reuse from backing-memory aliasing.
- [ ] Record concrete API signatures, backend capability requirements, test
  selections, and a representative performance baseline in this plan before
  beginning Stage 1. Freeze acceptable regression thresholds from measurements.

Completion: each lifetime-sensitive consumer has an identified owner and
completion authority; unresolved protocol decisions are closed and recorded.

### Stage 1: Introduce GPU Completion and Retirement on One Queue

Dependencies: Stage 0.

Outcome: explicit GPU completion and resource retirement contracts operate
through the existing single-queue execution path.

- [ ] Introduce typed queue identities, GPU completion points, and retirement
  prerequisite sets in RHI; keep CPU replay fences distinct.
- [ ] Adapt Vulkan completion tracking to queue-qualified points and preserve
  submission fence and payload ownership through GPU completion.
- [ ] Integrate pending-command retention, backend in-flight retention, resource
  deletion, descriptor/upload recycling, and allocation eviction with the
  selected ownership contract.
- [ ] Provide nonblocking completion polling and bounded-purpose waits. Keep
  ordinary reclamation free of device-idle calls and per-resource GPU waits.
- [ ] Test delayed and out-of-order completion observations, unsubmitted work,
  failed/canceled submissions, stale device generations, and shutdown.

Completion: early CPU replay completion cannot release GPU-used storage;
single-queue behavior remains equivalent, and affected tests plus the shared
Engine API `all` build pass.

### Stage 2: Compile and Submit an Explicit Execution Plan

Dependencies: Stage 1.

Outcome: RDG emits explicit submission batches and synchronization edges,
initially mapped to one physical queue.

- [ ] Introduce execution-plan records for pass assignment, submission batches,
  dependencies, resource handoffs, and prologue/epilogue barrier locations.
- [ ] Separate logical barrier planning, physical transition preparation, and
  submission. Preserve immutable plans and one resource-resolution boundary.
- [ ] Lower graph submission dependencies to RHI wait/signal operations without
  inserting CPU waits between passes.
- [ ] Extend captures with logical queue assignment, batch IDs, and dependency
  reasons. Keep dumps deterministic and exclude device addresses, native fence
  values, and measured timing from stable identity.
- [ ] Preserve culling, pass callbacks, extraction publication, failure outcomes,
  budgets, and exact range/access/discard behavior on the single-queue mapping.

Completion: single-queue replay oracles and production graph captures remain
equivalent; preparation failure emits no graph work, and partial submission
retains all already-submitted resources correctly.

### Stage 3: Enable Async Compute with Multi-Queue Lifetime Safety

Dependencies: Stage 2. Async execution remains disabled until all gates in this
stage pass together.

Outcome: eligible compute work executes on a separate queue with correct
dependencies, fallback behavior, and resource retirement.

- [ ] Add explicit async eligibility and deterministic queue-assignment policy,
  including graphics-queue fallback and a diagnostic override for comparisons.
- [ ] Implement Vulkan cross-queue waits/signals and required resource ownership
  transfers for both shared-family and distinct-family configurations.
- [ ] Track every using queue in retirement prerequisites. Cover concurrent
  reads, write/read handoffs, compute-only final uses, and graph boundaries.
- [ ] Integrate Renderer resource pools and reuse: require completed uses or
  explicitly scheduled dependencies before reuse; never assume frame number,
  pass index, or graphics completion covers outstanding compute work.
- [ ] Test fork/join, fan-in/fan-out, multiple readers, empty batches, culling,
  external resources, extraction/readiness, submission failure, and delayed
  compute completion. Validate that the submission graph is acyclic.
- [ ] Run Vulkan integration on an independent compute queue, inspect validation
  output, and compare rendered/read-back results with single-queue execution.
  Record unavailable queue-family coverage as outstanding, not passing.

Completion: cross-queue correctness and reclamation tests pass, single-queue
fallback passes, shared API builds pass, and enabled production wiring has an
identified integration test.

### Stage 4: Add Split-Barrier Scheduling and Transition Preparation

Dependencies: Stage 3.

Outcome: supported handoffs can begin after producers and complete before
consumers without changing their synchronization or lifetime guarantees.

- [ ] Introduce begin/end batch relationships and explicit transition-object
  ownership. Define legal placement around render passes and merged passes.
- [ ] Specify backend split-barrier support independently from queue support;
  preserve a full-barrier lowering when the optimization is unavailable.
- [ ] Validate pairing, exact resource/range agreement, no intervening illegal
  accesses, and transition-object retention through its final backend use.
- [ ] Centralize transition creation after required backings are prepared;
  add a creation queue only where batching or parallel creation needs it.
- [ ] Compare full and split lowering with CPU plan tests and Vulkan validation
  and output-equivalence tests on supported hardware.

Completion: split lowering preserves results and synchronization; forced full
barriers remain a tested fallback, and performance stays within the Stage 0
baseline or has an explicitly reviewed adjustment.

### Stage 5: Add Dependency-Aware Transient Aliasing and Qualify the Model

Dependencies: Stage 4.

Outcome: transient memory may be reused across non-overlapping GPU lifetimes
without conflating logical lifetime positions with observed GPU completion.

- [ ] Model allocation acquire/discard positions using submission dependencies
  and queue intervals. Prove ordering across all prior users before aliasing;
  a scalar first/last pass interval is insufficient across independent queues.
- [ ] Generate aliasing handoffs through the same execution-plan machinery and
  reject reuse when lifetimes can overlap on any participating queue.
- [ ] Keep backing heap destruction and CPU-side recycling governed by actual
  GPU completion even when graph-local aliasing was planned in advance.
- [ ] Test overlapping async lifetimes, safe ordered reuse, multiple terminal
  readers, extracted resources, and independent graphs sharing allocator pools.
- [ ] Measure CPU compile/prepare/submit cost, GPU overlap, peak transient memory,
  retirement backlog, and waits on the representative workloads selected in
  Stage 0. Adopt dependency compression or joins only with measured benefit.
- [ ] Run final affected native tests, multi-queue Vulkan integration gates,
  single-queue fallback gates, and an `all` build across the workspace targets.
- [ ] Update the owning runtime contracts and record reproducible completion
  evidence before completing the plan.

Completion: correctness gates and frozen performance budgets pass; diagnostics
explain queue assignment, handoffs, aliasing decisions, and retirement waits.

## Validation and Handoff

Use [agent build guidance](../Agents/BuildAndRun.md) and
[agent testing guidance](../Agents/Testing.md) for execution and target selection.
Use registry discovery for new target selections rather than inferring target
names. CPU fixtures must model independent queue progress; GPU integration
must exercise actual supported queue configurations. Unsupported hardware is
an explicit coverage gap for the relevant multi-queue gate.

Each stage must update this plan's status and evidence in its implementation
commit. Use the exact plan path and stage title for the repository's `Plan`
and `Stage` commit trailers. Do not carry a passing result across changes to
its relevant code, configuration, or environment.

## Owning Code and References

- [RDG public records](../../Engine/Source/Runtime/RenderCore/Public/RDG.h)
  and [compiler/recording](../../Engine/Source/Runtime/RenderCore/Private/RDG.cpp).
- [RHI command submission](../../Engine/Source/Runtime/RHI/Public/RHICommandList.h)
  and [implementation](../../Engine/Source/Runtime/RHI/Private/RHICommandList.cpp).
- [Vulkan completion](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanCompletion.h)
  and [submission payloads](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanSubmission.h).
- [Render Graph contract](../Runtime/Rendering/RenderGraph.md),
  [Renderer frame preparation](../Runtime/Rendering/RendererFramePreparation.md),
  and [render resource lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md).
- [UE barrier batches](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RenderCore/FRDGBarrierBatchBegin)
  provide reference boundaries for transition creation and submission.
- [UE transient allocation fences](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/FRHITransientAllocationFences)
  describe graphics/async-compute lifetime overlap and acquire/discard ordering;
  these are not interchangeable with polled GPU-completion objects.
- [UE GPU fences](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/FRHIGPUFence)
  and [RHI resource deletion](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHIResource?lang=en-US)
  document separate completion and object-lifetime interfaces. Backend deletion
  details must be verified against a specific UE version before copying a design.
- [D3D12 multi-engine synchronization](https://learn.microsoft.com/en-us/windows/win32/direct3d12/user-mode-heap-synchronization)
  illustrates explicit cross-queue ordering and fence timelines.
