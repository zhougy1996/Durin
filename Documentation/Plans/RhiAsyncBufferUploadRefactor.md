# RHI Asynchronous Buffer Upload Refactor Plan

Summary: Decouple uniform and storage uploads from render-thread frame-slot waits, introduce deferred logical buffer operations, and remove unconditional frame-start RHI synchronization while preserving completion-based reclamation.

Last reviewed: 2026-09-23

Status: Active
Completed:

## Current Status

Stage 0 inventory, interface decisions, and pre-refactor runtime baseline are
recorded below. RHI replay and Vulkan frame-slot, queue-poll,
descriptor-preparation, storage-reset, upload-overflow, and presentation scopes
have distinct CPU profile zones. Stage 1 implementation has begun with an
owned, graph-specific RHI buffer upload and RDG queued upload passes. Logical
deferred buffers, backend version allocation, consumer migration, and removal
of the frame-start wait remain pending. No asynchronous frame-start path or
performance improvement is claimed yet.
The prior checkpoint passed a Release `all` build, Debug
`VulkanRHIIntegrationTests` (103/103), `RHICommandListTests` (95/95), and
changed-document validation. Current validation appears under Stage 1.

Source inspection establishes the following baseline:

- `FDynamicRHI::RHIBeginFrame_RenderThread` submits `BeginFrame` using
  `FlushRHIThread`, blocking until preceding RHI work and this boundary complete.
- Vulkan `RHIBeginFrame` selects `FrameNumber % FrameInFlight` and calls
  `FVulkanFrame::Prepare`, which waits its GPU retirement prerequisites.
  `FrameInFlight` is currently two.
- Vulkan then performs a separate synchronous `PrepareForProducer` operation
  for dynamic uniforms, followed by render-thread storage producer reset.
  These operations are outside the `RHI.BeginFrame.FlushRHIThread` profile zone.
- The executor frame number advances during successful `EndFrame` replay;
  front-end storage allocation currently depends on that executed frame state.
- Command buffers and submission fences already use completion-tracker
  retirement. They do not require a new frame-age-based reclamation design.

The screenshot motivating this work identifies a long synchronization scope,
but does not establish whether RHI backlog, GPU slot waiting, or other backend
preparation dominates it. Instrumentation must separate those causes.

### Stage 0 Migration Inventory (2026-09-23)

The search covered source and test roots in all three `Durin.dworkspace`
projects. Sandbox and RoadWeaver have no direct references to
`AllocateDynamicUniformBuffer`, `AllocateDynamicStorageBuffer`, or the two
physical range types; their dependencies on Engine still require target builds
after a shared API change.

| Surface | Current owner and migration consequence |
| --- | --- |
| Dynamic allocations | `FRHICommandListImmediate` delegates to `FDynamicRHI`. Vulkan fast paths return mapped physical ranges; overflow calls a synchronous RHI operation. Other backends use synchronous context allocation. |
| Uniform producer | `FVulkanDynamicUniformBufferAllocator` has two producer states, each starting with a 4 MiB page, and up to eight chunks per producer. `RHIBeginFrame_RenderThread` synchronously prepares one, and `RHIEndFrame` attaches every reserved queue use. |
| Storage producer | `FVulkanDynamicStorageBufferAllocator` uses the executor frame number modulo two, with a 64 MiB and 16-chunk limit per frame. Render-thread `BeginFrameProducer` resets offsets after the frame-start flush. |
| Recorded uploads | `FRHICommandListBase::WriteBuffer` already copies source bytes, retains its destination, and replays in command order. `UpdateUniformBuffer` is an aligned wrapper over it. Native buffer creation and automatic view cache misses still return synchronous, fallible results. |
| Binding | `FRHIUniformBufferRange` and `FRHIStorageBufferRange` carry a physical `FRHIBuffer*`, offset, and size. RenderCore shader reflection reads those structs directly. RHI parameter commands retain resolved resources, while Vulkan descriptor snapshots cache native backing. A logical range migration must change reflection and command recording together. |
| RDG | `FRDGBuilder::CreateBuffer` declares a logical graph buffer, but there is no queued initial-data upload helper. Graph allocation and extraction follow the existing compile/execute and terminal-consumer contracts. |
| Frame slots | `FVulkanFrame::Prepare` waits retirement prerequisites for `FrameNumber % FrameInFlight`; context begin/end, swapchain minimum image count, descriptor pool capacity, dynamic upload allocators, and legacy test diagnostics reference the two-slot policy. Command buffers and fences already retire from completion tracking. |
| Queue pressure | `FRHIThreadLimits` currently bound accepted work to 8 entries, 16 batches, and 32 MiB of payload. These limits omit the producer's unsubmitted command storage and do not alone bound queued frame count or backend upload pages. |

The baseline capture must use one fixed scene and resolution on a recorded
hardware/driver/build revision, with a quiet GPU lane and the same workload
after migration. Capture the render-thread `RHI.BeginFrame.FlushRHIThread` and
`Vulkan.BeginFrame.PrepareUniformBufferSync` scopes, RHI-thread
`RHI.BeginFrame.Replay`, `Vulkan.BeginFrame.FrameSlotWait`, queue polling and
descriptor preparation, uniform `WaitForUses`, upload overflow waits, and
presentation scopes. Record median and tail frame time, throughput, latency,
queue payload high-water marks, upload allocation high-water marks, and pressure
wait counts. A missing or inaccessible GPU leaves this gate open; a synthetic
CPU-only fixture cannot substitute for the runtime baseline.

### Stage 0 Runtime Baseline (2026-09-23)

Revision `305d4bfc794acc4089900eaafb04e91a13af477b` plus only the profile
zones in this stage was built with `Win64-Release-DurinEditor` (`all`). The
workload was Sandbox's default `/Game/Levels/GrayboxStage15` in DurinEditor,
3840x2064 FIFO swapchain with two images, on an Intel Core i7-12700 and NVIDIA
RTX 3090 (driver 616.64). Each independent process warmed for 40 seconds and
ran a 20-second Tracy capture; analysis removed two seconds from each end.
There were 966–967 complete frame intervals per retained sample. The three
runtime runs and a fourth memory-statistics run completed normally. Local raw
traces, exported zones, scripts, and logs are in
`Build/Profiling/RhiAsyncBufferUploadRefactor/`.

| CPU zone or matched interval | Run 1 median / p95 ms | Run 2 | Run 3 |
| --- | ---: | ---: | ---: |
| Render-thread `RHI.BeginFrame.FlushRHIThread` | 14.886 / 16.441 | 15.779 / 16.389 | 15.697 / 16.320 |
| Render dispatch to RHI `BeginFrame` replay start | 0.526 / 1.509 | 0.254 / 0.450 | 0.299 / 1.216 |
| RHI-thread `Vulkan.BeginFrame.FrameSlotWait` | 14.199 / 16.105 | 15.480 / 16.098 | 15.327 / 15.984 |
| Render-thread uniform preparation sync | 0.061 / 0.162 | 0.025 / 0.041 | 0.026 / 0.154 |
| Vulkan swapchain image acquisition | 0.012 / 0.022 | 0.007 / 0.009 | 0.007 / 0.021 |
| Vulkan presentation call | 0.207 / 0.547 | 0.104 / 0.207 | 0.123 / 0.526 |
| Frame interval | 16.694 / 19.189 | 16.666 / 17.369 | 16.666 / 18.837 |

The dispatch-to-replay interval matches each render-thread flush with the
RHI-thread replay event inside it; these rows are not obtained by subtracting
independent medians. Frame-slot wait accounts for nearly all of the replay
duration in this FIFO workload. Uniform preparation, queue polling, descriptor
preparation, and storage reset were much smaller. Neither upload-overflow zone
appeared. This supports a frame-slot pacing diagnosis for this workload, not a
claim that the GPU can render faster than FIFO cadence.

The fourth run's shutdown diagnostics reported 8 MiB of dynamic-upload arena
capacity (37,568 B live high-water), 8 MiB of transfer-upload arena capacity
(5,949,254 B live high-water), 207 upload operations / 6,609,094 B, and zero
arena overflow and wait events. Across runs, the RHI executor peaked at two
entries, seven batches, and 5,851,090 queued payload bytes, with no queue
backpressure or rejection. The memory diagnostics' dynamic-upload arena does
not account for every storage-producer byte, so Stage 2 must add a complete
upload-memory high-water measure before its pressure policy can be accepted.

Performance acceptance uses this exact FIFO scene and an additional controlled
delayed-RHI fixture. The ordinary render-thread BeginFrame path must have zero
unconditional RHI serial waits, and the fixture must demonstrate recording
frame N+1 while RHI frame N remains delayed. FIFO median frame interval must
stay at or below 17.5 ms and p95 at or below 20 ms on this host; queue limits
must bound frame count and payload bytes, with pressure waits and memory
high-water reported separately. A later upload or end-frame wait that simply
replaces the old 14–16 ms frame-slot wait does not pass. The controlled fixture
must show useful CPU/RHI overlap; correctness and memory limits remain hard
gates even if these timing budgets pass.

### Stage 0 Interface and Ownership Contract

The new front-end resource is `FRHIDeferredBuffer`, a counted `FRHIResource`
distinct from native `FRHIBuffer`. It stores an immutable `FRHIBufferDesc` and
`ERHIDeferredBufferUsage` (`SingleDraw`, `SingleFrame`, `MultiFrame`). Its public
`FRHIDeferredBufferRange` has a logical buffer pointer, byte offset, and byte
size; no backend page index or mapped pointer escapes. The shader parameter
resolver accepts that range for uniform and storage bindings alongside the
legacy physical range types during migration, then removes the legacy path.
Explicit deferred views own a deferred buffer and an existing
`FRHIBufferViewDesc`; creating a logical view performs descriptor validation
without native creation. A Vulkan descriptor is materialized during ordered
replay and keyed by the resolved backing version and allocation generation.

The following signatures are fixed for implementation; `FByteView` is copied
by default and `References` is a span of resource pointers copied into counted
ownership, separate from the raw constant bytes:

```cpp
enum class ERHIDeferredBufferUsage : uint8 { SingleDraw, SingleFrame, MultiFrame };
enum class ERHIDeferredBufferError : uint8 {
    InvalidDescriptor, InvalidRange, InvalidUsage, PayloadBudgetExceeded
};
auto FRHICommandListBase::TryCreateDeferredBuffer(
    const FRHIBufferDesc& Desc, ERHIDeferredBufferUsage Usage,
    FByteView InitialData, std::span<FRHIResource* const> References = {})
    -> std::expected<TRefCountPtr<FRHIDeferredBuffer>, ERHIDeferredBufferError>;
auto FRHICommandListBase::TryUpdateDeferredBuffer(
    FRHIDeferredBuffer* Buffer, uint32 Offset, FByteView Data,
    std::span<FRHIResource* const> References = {})
    -> std::expected<void, ERHIDeferredBufferError>;
auto FRHICommandListBase::TryCreateDeferredBufferView(
    FRHIDeferredBuffer* Buffer, const FRHIBufferViewDesc& Desc)
    -> std::expected<TRefCountPtr<FRHIDeferredBufferView>, ERHIDeferredBufferError>;
auto FRDGBuilder::QueueBufferUpload(
    FRDGBufferHandle Buffer, uint32 Offset, FByteView Data) -> FRDGPassHandle;
auto FRDGBuilder::QueueBufferUploadOwned(
    FRDGBufferHandle Buffer, uint32 Offset, FByteBuffer Data) -> FRDGPassHandle;
auto FRDGBuilder::CreateStructuredBuffer(
    std::string_view Name, uint32 Stride, FByteView Data,
    EBufferUsageFlags AdditionalUsage = EBufferUsageFlags::None)
    -> FRDGBufferHandle;
auto FRDGBuilder::CreateStructuredBufferOwned(
    std::string_view Name, uint32 Stride, FByteBuffer Data,
    EBufferUsageFlags AdditionalUsage = EBufferUsageFlags::None)
    -> FRDGBufferHandle;
auto FRHICommandListBase::UploadBuffer(
    FRHIBuffer* Buffer, uint32 Offset, FByteView Data) -> void;
```

`TryCreateDeferredBuffer` returns a usable logical reference after validation
and CPU-payload admission, never a claim of native readiness. Initial bytes and
references live in the logical object until first backend use. Any command
list may be the first consumer: that replay operation materializes initialization
before use, independent of which list created the object. Updates are copied
and ordered by accepted RHI command replay, including across command lists.
Inline replay runs the identical operations in the same order.

Every accepted update creates a content version. Uniform updates replace the
whole contents; storage uploads may name an exact subrange and must preserve
untouched bytes. A draw or dispatch resolves the version visible at its replay
point, retaining its physical backing and descriptor lease through all
submissions that use it. For `Update(A) -> Draw -> Update(B) -> Draw`, old GPU
work retains A even when the second draw binds B. Prepared parameter batches
hold logical references; they do not cache a native view or choose a version
before the binding command replays. A later update after a prepared bind must
invalidate that binding at the next draw/dispatch. Cross-queue consumers retain
the exact queue-qualified prerequisites. No-copy APIs are deferred until their
ownership through RHI replay can be proved.

Existing `WriteBuffer`, `UpdateUniformBuffer`, synchronous `RHITryCreateBuffer`,
and explicit physical-view factories remain for persistent, GPU-written, and
readback resources. `WriteBuffer` already owns bytes and remains the general
upload command for such resources. The new RDG helpers declare an exact
`TransferWrite` graph use, copy or move their CPU source into graph-owned
storage, and record `UploadBuffer` into an RHI command. Unlike `WriteBuffer`,
this command leaves the written range in `TransferWrite` so the graph's next
declared barrier sees the actual backend state. After the command copies the
bytes, graph destruction may release its source. Graph cancellation drops
unconsumed source owners.

### Stage 0 Failure and Pressure Contract

Invalid descriptors, incompatible usage, ranges, or front-end payload admission
return `ERHIDeferredBufferError` before any command is recorded. They leave no
partially ready logical reference. Existing recoverable native creation remains
on `RHITryCreateBuffer`; existing synchronous readback, locks, swapchain
acquisition, and explicitly requested GPU waits remain synchronous. A deferred
operation accepted for replay is frame-critical: native allocation/submission
failure follows the executor's terminal failure and Vulkan quarantine/device-
loss path, not a later false-ready result. The legacy dynamic-storage empty
result is preserved until each caller handles the new admission error
explicitly. RDG allocation failure remains an `FRDGExecutionResult` error.

Initial limits are a 16 MiB maximum individual deferred upload, 32 MiB of
front-end unsubmitted owned bytes, the existing RHI queue limits of eight
entries / sixteen batches / 32 MiB, and at most three completed render frames
awaiting RHI replay. Backend dynamic-uniform capacity is bounded to 64 MiB:
4 MiB normal pages plus tracked oversize allocations up to 16 MiB each.
Dynamic storage is bounded to 128 MiB under the same normal and oversize rules.
The existing transfer-upload arena remains four 8 MiB pages plus tracked
oversize ranges. These are total live capacities, including recorded but not
submitted owners; allocation counters and high-water marks must include them.

On pressure, reclaim completed leases, grow within the class limit, then wait
only for the oldest actual submitted owner that can release capacity. If only
unsubmitted work owns the needed capacity, submit an eligible recording first;
otherwise reject before recording, with the declared admission error. Never
wait on an unsubmitted reservation whose submission needs the blocked
allocator. Queue-frame admission may wait only after the three-frame limit and
must report count and duration. A steady-state wait relocated from BeginFrame
to that limit fails the performance gate even if correctness passes. Legacy
two-slot Vulkan frame pacing stays on the RHI thread until its remaining owners
are audited.

## Goal

Let the render thread prepare and record a later frame while the RHI thread
processes earlier work. Ordinary uniform and storage uploads must not require
an RHI round trip, a GPU frame-slot acquisition, or a shared active producer
to become ready on the render thread.

Keep resource reuse safe, memory bounded, command ordering deterministic, and
inline and threaded execution equivalent. Necessary resource-pressure and
latency-control waits remain explicit and measurable.

## Scope and Selected Decisions

Owners are RHI, VulkanRHI, RenderCore, and their renderer/frame-loop consumers.
Consumer migration must cover every project in `Durin.dworkspace`: Engine,
Sandbox, and RoadWeaver, including their source and test roots.

Reuse the existing GPU sync-point, payload ownership, submission coordinator,
and retirement machinery. The [RDG and RHI multi-queue plan](RdgRhiMultiQueueExecution.md)
continues to own async-compute rollout, split barriers, and transient aliasing;
this work must preserve its implemented queue and lifetime contracts without
claiming its remaining qualification gates are complete.

### Logical Resources and Deferred Data

The selected front-end model records owned CPU data and logical resources;
the backend chooses physical storage during ordered execution. Returning a
logical object does not certify native allocation, upload, or GPU completion.

The following interface families describe intent; Stage 0 freezes exact names,
signatures, error behavior, and migration against existing APIs:

| Family | Intended semantics |
| --- | --- |
| Create uniform buffer | Accept contents, layout, and single-draw, single-frame, or multi-frame usage; return a retained logical reference with an ordered initialization dependency. |
| Update uniform buffer | Own a content snapshot and referenced resources; order the update relative to consuming commands. |
| Bind uniform buffer | Bind the resource version visible at that point in the command stream. |
| Create buffer / buffer views | Describe size, usage, and logical SRV/UAV ranges independently of upload allocator placement. |
| Upload buffer | Own source bytes, destination reference, offset, and size; execute before dependent GPU use. |
| RDG structured-buffer creation / queued upload | Express initial data and graph dependencies; batch uploads where compatible. |

Usage declarations inform allocation strategy, never authorize frame-age reuse.
Uniform layouts must retain referenced resources as well as raw constant bytes.
Structured/storage buffers may be persistent, CPU-uploaded, or GPU-written;
they are not all frame-local upload allocations.

For `Update(A) -> Draw -> Update(B) -> Draw`, the first draw must see A and the
second B, even if both execute after front-end recording ends. A mutable
"latest contents" pointer alone is insufficient. Backend versioned storage or
correctly ordered GPU updates must preserve every outstanding use and invalidate
descriptor caches when the effective backing changes.

Default uploads copy data into command/graph-owned storage. Any no-copy or move
overload requires explicit ownership through the last CPU read, including graph
deferral and RHI replay where applicable. Graph execution ending alone is not a
universal source-release signal. Cancellation must release unconsumed owners.

Replace front-end dependencies on physical `FRHIUniformBufferRange` and
`FRHIStorageBufferRange` placement. Logical buffer offsets remain meaningful;
backend arena offsets remain private. Preserve alignment, bounds checks,
reflected binding compatibility, and exact transition ranges. A temporary
adapter is allowed during migration, but cannot remain a hidden synchronous
allocation path in the accepted ordinary upload flow.

### Backend Allocation and Retirement

Retain distinct allocation classes where appropriate; shared lifetime rules do
not require a single uniform/storage/staging allocator. CPU payloads survive
until copied; staging storage survives its transfer; destination storage survives
all GPU reads/writes. Flush non-coherent mapped writes and emit required GPU
barriers before consumption.

Represent recorded-but-unsubmitted ownership explicitly. A missing GPU sync
point does not imply free storage. Transfer ownership into submission payloads,
then reclaim only when the existing retirement rules permit it. Cross-queue use
must retain all applicable prerequisites, not a graphics-only projection.

Pressure policy is reclaim, bounded growth, then a wait on actual submitted
work that can release capacity. Never wait for an unsubmitted reservation whose
submission requires the blocked allocator; split/submit eligible pending work
or reject according to the declared failure contract. Budget front-end owned
bytes as well as backend pages, oversize allocations, and queued frames.
Submission failure and device loss must preserve quarantine/teardown semantics;
they do not count as successful completion.

### Frame Boundaries and Pacing

`BeginFrame` becomes an ordered asynchronous boundary. Front-end frame identity
is captured when recording and does not select reusable GPU memory. Backend
execution counters may remain for diagnostics and ordered bookkeeping.

Initially retain `FVulkanFrame::Prepare` and existing GPU pacing on the RHI
thread. Removing the render-thread flush does not require deleting the two GPU
slots. Audit every frame-slot consumer before changing this boundary.

Separately enforce a documented bound on queued work/frames and CPU payload
memory. Do not replace the removed frame-start wait with an unconditional wait
on the same serial in another upload or allocation helper. Legitimate pressure,
presentation, readback, and explicit synchronization boundaries remain observable.

### Command Buffers, Pools, and Descriptors

Preserve the existing completion-tracker return path for command buffers and
fences. A submitted command buffer cannot reset before its submission completes.
The current command pools permit individual command-buffer reset; whole-pool
reset would require every affected command buffer to be safe. Maintain queue
family and recording-owner constraints.

Descriptor batches already retain recording/in-flight leases. Preserve that
mechanism and allocation-generation cache invalidation while introducing buffer
versions; an old descriptor cannot silently bind recycled backing storage.

Fixed GPU frame slots may later be reduced to pacing metadata or removed from
resource ownership after all consumers are audited. Their deletion is not an
acceptance requirement for this plan. Any remaining removal scope must be
recorded explicitly rather than rewriting proven command-buffer retirement.

## Implementation Stages

### Stage 0: Freeze Interfaces, Ownership, and Baseline

Dependencies: current runtime contracts and the implemented multi-queue model.

- [x] Inventory dynamic allocation, uniform creation/update/binding, storage
  ranges, frame counters, synchronous resource creation, descriptor caching,
  and frame-slot consumers across all workspace projects and tests.
- [x] Freeze exact API signatures, uniform resource-reference layouts, content
  version rules, cross-command-list initialization dependencies, and inline
  behavior. Reuse existing upload commands where their contracts already fit.
- [x] Decide how deferred allocation failure is reported; do not silently
  change existing recoverable/terminal failure boundaries or return false
  readiness from logical resource creation.
- [x] Specify page, oversize, CPU payload, and queued-frame limits plus pressure
  progress rules. Identify calls that genuinely need synchronous results.
- [x] Capture comparable baseline profiles and separate RHI queue wait, GPU
  slot wait, uniform preparation, allocation pressure, and presentation wait.
  Record workload, revision, hardware, and performance acceptance budgets.

Completion: reviewed migration inventory, executable ownership contracts, and
baseline evidence; no claim that deleting one Flush alone solves the stall.

### Stage 1: Introduce Deferred Logical Buffer Operations

Dependency: Stage 0 interface and failure decisions.

- [ ] Implement logical uniform creation/update/binding with owned snapshots,
  retained resource references, and ordered version visibility.
- [ ] Implement or adapt general buffer upload and view operations; separate
  logical ranges from backend allocation placement.
- [ ] Add RDG queued upload/structured-buffer helpers with explicit copied and
  retained source ownership, compatible batching, and transition declarations.
- [ ] Verify source lifetime, repeated updates, initialization dependencies,
  cancellation, and inline/threaded equivalence using deterministic tests.

Completion: new operations work alongside legacy callers without removing
the existing safety boundary prematurely.

Stage 1 progress (2026-09-23): `FRHICommandListBase::UploadBuffer` owns replay
bytes and retains its physical destination. Vulkan leaves its written range in
`TransferWrite`, unlike the canonical-restoring `WriteBuffer`. RDG's
`QueueBufferUpload` and `QueueBufferUploadOwned` build Copy recording passes
with exact byte uses and 16 MiB single / 32 MiB per-builder source-capacity
limits. `CreateStructuredBuffer` and its owned-source variant queue complete
initial contents through that same path. The helpers require or supply
`DestinationCopy` and report invalid inputs as graph compilation errors. This
is a partial implementation of the second and third checklist items; logical
ranges/views and compatible upload batching remain pending. Debug `all` build,
`RHICommandListTests` (96/96), and `RenderContractTests` (192/192) passed for
this slice. The Vulkan graph-upload and copy-matrix integration cases passed
in both inline and threaded modes. A full `VulkanRHIIntegrationTests` run
initially timed out after 300 seconds, and a later repetition exited with an
access violation. The viewport transaction test recorded a present on the
render thread and then directly recreated the swapchain on the RHI thread
without submitting that pending present. It now flushes the RHI command list
before recreation and before releasing the detached viewport. Five consecutive
full-suite runs passed (104/104 each, approximately 15 seconds per run) with
a 90-second process limit after this ordering fix.

### Stage 2: Move Upload Storage Ownership to the Backend

Dependency: Stage 1 logical resource and upload contracts.

- [ ] Implement completion-aware allocation/versioning using existing payload
  leases and sync points, without a render-thread active producer or modulo-two
  upload slot selection.
- [ ] Handle staging and destination lifetimes, multi-queue use, alignment,
  non-coherent writes, descriptor invalidation, and exact range transitions.
- [ ] Enforce bounded pressure and forward progress for submitted and
  unsubmitted work; preserve failure quarantine and safe shutdown.
- [ ] Verify delayed GPU completion prevents reuse without blocking unrelated
  front-end preparation; retain current command-buffer/fence retirement.

Completion: ordinary new uploads need no synchronous RHI allocation response,
and backend memory reuse is proven independently of frame age.

### Stage 3: Migrate Consumers and Remove Frame-Start Synchronization

Dependency: Stage 2 lifetime and pressure gates.

- [ ] Migrate all workspace consumers and fixtures; remove obsolete physical
  range APIs/adapters after the last consumer is converted.
- [ ] Remove `PrepareUniformBufferSync` and front-end storage producer reset.
  Submit `BeginFrame` asynchronously and remove its unconditional RHI wait.
- [ ] Introduce or adapt explicit queue/latency limits; verify later ordinary
  upload helpers do not reintroduce the same serial wait.
- [ ] Retain backend frame pacing initially; audit remaining frame-slot owners
  and document any intentionally retained two-slot policy.
- [ ] Demonstrate frame N+1 preparation while RHI frame N is delayed, with
  correct output and bounded memory, in a controlled fixture and runtime trace.

Completion: the ordinary frame-start/upload path has no unconditional
render-thread RHI flush, and all consumer targets compile and pass their gates.

### Stage 4: Qualify Performance, Lifetimes, and Handoff

Dependency: Stage 3 migration complete.

- [ ] Cover repeated update/draw ordering, multiple submissions per frame,
  frames with no draws, delayed RHI/GPU work, and multi-queue resource use.
- [ ] Cover page exhaustion, oversize requests, pending unsubmitted work,
  canceled recording, rejected/failed submission, device loss, resize, and
  shutdown with live uploads. Assert no premature reset or overwrite.
- [ ] Compare identical workloads and instrumentation against Stage 0: report
  render-thread wait time, throughput, CPU/GPU overlap, frame latency, queued
  payload bytes, upload memory high-water marks, and pressure waits. A renamed
  or relocated wait is not performance acceptance.
- [ ] Run affected native suites and project targets plus the required `all`
  build for a shared Engine API migration. Record exact evidence.
- [ ] Update owning runtime contracts, close evidence-backed checklists, and
  complete this plan only after all required acceptance gates pass.

Completion: demonstrated overlap, preserved rendering correctness and resource
lifetime, bounded memory/latency, and no unexplained regression against the
recorded budgets.

## Validation and References

Follow [build guidance](../Agents/BuildAndRun.md) before configuration/build/run
and [testing guidance](../Agents/Testing.md) before selecting or running native
tests. Follow [documentation guidance](../Agents/Documentation.md) for this
plan and subsequent contract updates. A design-only commit does not require
native validation and does not renew earlier runtime evidence.

Current authorities:

- [RHI command execution](../Runtime/Rendering/RHICommandExecution.md)
- [Vulkan memory and GPU completion](../Runtime/Rendering/VulkanMemoryAndGPUCompletion.md)
- [RHI resource views and transfers](../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [Render Graph](../Runtime/Rendering/RenderGraph.md)

UE provides interface inspiration, not a verified implementation template for
a specific Vulkan backend version. Public references support usage declarations,
deferred uploads, explicit data ownership, and render/RHI overlap; they do not
establish that every UE backend avoids synchronization:

- [Uniform buffer usage](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/EUniformBufferUsage)
- [Uniform buffer creation](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/FDynamicRHI/RHICreateUniformBuffer)
- [RDG buffer uploads](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RenderCore/FRDGBuilder/QueueBufferUpload)
- [Initial-data ownership](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RenderCore/ERDGInitialDataFlags)
- [Parallel rendering](https://dev.epicgames.com/documentation/unreal-engine/parallel-rendering-overview-for-unreal-engine)
