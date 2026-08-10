# Vulkan Memory, Transfer, and Retirement Plan

Summary: Establish explicit Vulkan allocation classes, reusable upload/readback storage, memory-pressure telemetry, and GPU-completion-aware recycling and destruction.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

M0 capability/startup, M1 resource transitions, M2 resource views/transfers,
and M3 graphics state/bindings are complete. Their public contracts provide
validated resource descriptions, exact copy ranges, one Vulkan state tracker,
counted view ownership, bounded structural caches, and observable descriptor
allocation. M4 can change backend allocation and retirement without changing
the recorded-command or binding identities.

The activation audit at `bf0613f8` found that Vulkan owned VMA through
`FVulkanMemoryManager`, persistently maps dynamic buffers, reuses fences and
command buffers, maintained two frame slots, and waited for a slot's frame fence
before resetting its command buffers and descriptor pools. Texture readback
performs the required invalidate before copying bytes to the CPU, while static
buffer writes and texture uploads use the public transition/copy paths.

Before Stage 1, the safety policy was frame-age based. `FDeferredDeletionQueue` tagged
entries with `GVulkanRHIDeletionFrameNumber` and destroyed them after
`kFrameInFlight` later end frames rather than after the submission which used
them completes. Descriptor pools, dynamic-uniform pages, and command-buffer
reuse are also selected by frame index. This is conservative during regular
two-frame cadence but is not a proof for irregular submission, empty frames,
long GPU work, synchronous operations, or future queue expansion.

Allocation placement is now explicit, while transfer storage remains
per-operation. Device-local, dynamic upload, transfer upload, and transfer
readback allocations have deterministic VMA usage and property policies;
host-visible classes remain persistently mapped. Static buffer writes, texture
uploads, and scoped texture readbacks still allocate one temporary Vulkan
buffer per operation until Stage 3 replaces them with bounded arenas.

Stage 0 is complete on the `windows-msvc-x64` profile with an NVIDIA GeForce
RTX 3090. Diagnostic-only counters now cover allocation-class candidates and
size buckets, transfer volume, heap budgets, frame-fence waits, descriptor
pressure, command-buffer reuse, and deferred deletion. The deterministic
threaded churn and a hidden 120-tick Sandbox editor capture froze the initial
arena bounds below. Every deletion type and frame-index reuse site has an exact
Stage 1 or Stage 4 completion rule, and the selected device contract provisions
one queue at index zero for graphics, compute, transfer, and presentation.

Stage 1 is complete. Every command submission reserves a token while recording
and publishes it only after `vkQueueSubmit` succeeds. The device-owned
completion tracker pairs each published token with its own pooled fence, polls
without blocking, advances one contiguous watermark, and releases payload and
command-buffer storage in token order. Frame slots now wait for their exact last
submission token rather than owning a frame fence.

`FDeferredDeletionQueue` no longer uses `GVulkanRHIDeletionFrameNumber` or
frame age. Recording-time destruction observes the reserved token, including
temporary staging buffers destroyed before their commands submit; ordinary and
cross-thread enqueue paths read the atomic reservation watermark, and native
handles release only when that exact token is completed. Shutdown waits and
retires all submitted tokens before destroying contexts, pools, the allocator,
fence manager, and device. Present fences and their semaphores remain owned by
viewport frame resources and are not folded into graphics completion.

Stage 2 is complete. The four allocation classes select explicit required and
preferred properties, mapped-range normalization is shared by flush and
invalidate, and backend-neutral RHI statistics expose live/peak allocation
pressure, heap budgets, transfers, waits, and retirement without native types.
Reset preserves live gauges and current heap snapshots while clearing interval
counters. Failure injection attributes failures independently to all four
classes.

Stage 3 is complete. Device-owned upload and readback arenas use the frozen
8 MiB/four-page and 4 MiB/two-page bounds, persistently mapped pages, aligned
free ranges, exact-token retirement, free-range coalescing, and tracked
oversize allocations. Capacity exhaustion submits the current payload when
necessary and waits only for the oldest range token. Static buffer writes and
texture uploads now use upload ranges; scoped texture readback waits its exact
producing token, invalidates its range, copies to owned CPU output, and returns
the range.

Stage 4 is complete. Descriptor allocation now rotates between bounded pool
batches selected by completed-token eligibility; every pool records the maximum
submission token referencing its allocated sets and resets only after that
token completes. Dynamic-uniform allocation retains two bounded producer
states and 4 MiB base pages, but selection is completion-aware rather than
`frame % 2`; each used chunk records its maximum end-frame token, overflow is
bounded to eight chunks per producer, and reset waits only for the oldest exact
token when both producers remain in flight. Command buffers and submission
fences were already returned exclusively by the Stage 1 completion tracker.
Stage 5 is complete. The aggregate Vulkan suite exercises mixed resource,
descriptor-array, transfer, readback, render, viewport, irregular-frame, and
shutdown churn in inline and threaded modes. Allocation failure is attributed
to all four classes; descriptor-pool and structural-cache creation failures
leave prior candidates usable and recover on the same RHI thread. Comparative
captures show the expected native transfer-allocation collapse and bounded
arena reuse without a wait or retirement regression.

Stage 6 and this plan are complete. The lasting rendering contract now owns the
allocation classes, mapped-range rules, bounded arenas, completion domains,
backend-pool reuse, statistics, failure behavior, and shutdown order. The
roadmap records M4 complete and M5 ready. Focused M4 targets, the full Debug
Editor build, and the hidden-window runtime passed. The repository-wide native
aggregate ran 1,386 targets with 1,385 passing; its sole failure is the stable,
independently reproduced
`FSourceReferenceIndexTests.RelocatesSharedSourceAndAllReferencingPackages`
Editor asset-source relocation case. No file in that implementation or test
changed from the M4 activation baseline through this plan, so it remains a
recorded out-of-scope repository follow-up rather than an M4 regression.

## Goal

Make Vulkan memory placement, CPU/GPU transfer storage, native-object
retirement, and reusable backend pools explicit and measurable, so no object or
suballocation is destroyed, reset, or overwritten until the exact GPU work
which may reference it has completed.

## Scope

- A backend-private, monotonically ordered GPU submission/completion token for
  the negotiated single graphics/present queue path.
- Completion-aware ownership and retirement for command buffers, descriptors,
  buffers, images, views, samplers, pipelines, framebuffers, and other native
  objects currently entering `FDeferredDeletionQueue`.
- Explicit VMA allocation classes for device-local static resources,
  persistently mapped dynamic/upload resources, and mapped readback resources.
- Reusable, bounded, persistently mapped upload and readback arenas with exact
  alignment, wrap/overflow, oversize, flush/invalidate, and shutdown rules.
- Migration of static buffer writes, texture uploads, texture readbacks,
  dynamic uniforms, descriptor pools, and command-buffer recycling to the
  completion authority where applicable.
- Queryable allocation, heap-budget, transfer, arena, wait, descriptor, and
  retirement statistics with resettable counters and stable snapshots.
- Failure injection and irregular-cadence qualification through public RHI
  commands in inline and dedicated-thread execution.

## Non-Goals

- Asset streaming, mip residency, eviction of Renderer resources, or a global
  Renderer memory-admission policy.
- Render-graph transient aliasing, sparse resources, virtual texturing, or
  bindless handle allocation.
- Separate transfer/compute queues, queue-family ownership transfers, timeline
  semaphores as a product feature, or cross-queue completion aggregation.
- Asynchronous public readback APIs. Existing scoped readback remains
  synchronous and waits only for the submission which produces its bytes.
- Changing CPU executor serial fences into GPU fences or making GPU completion
  visible as command-list completion.
- Changing resource/view identity, transition semantics, binding identity,
  graphics cache keys, or the complete-or-null creation contract.
- Device-loss recovery or treating out-of-budget pressure as recoverable after
  native submission has begun.

## Design Decisions and Invariants

### Completion ownership and ordering

- CPU executor serial completion and GPU completion are separate domains. CPU
  fences continue to prove replay and command-storage release only; native
  destruction and reuse consume Vulkan completion evidence.
- The required path has one graphics submission order. Each submitted batch is
  assigned a monotonically increasing `FVulkanCompletionToken`; zero means no
  GPU dependency. The completion tracker owns the batch fence and advances a
  contiguous completed-token watermark only after Vulkan reports the fence
  signaled.
- Recording payloads retain every counted RHI resource/view required by their
  native commands until the payload receives a submitted token. Retirement
  records use the last token which may reference the object. A token is never
  inferred from frame number, CPU executor serial, object age, or cache age.
- Polling completion is nonblocking during ordinary begin/end-frame
  maintenance. Blocking is allowed only for an exact synchronous result, reuse
  when a declared bound is exhausted, or orderly shutdown; each wait increments
  an attributable counter. Whole-device idle is not an ordinary recycling
  mechanism.
- Submission failure remains terminal. Candidate allocation and cache failure
  publishes nothing, leaves prior state intact, and reports allocation class,
  requested bytes, heap/budget snapshot when available, and native result.
- Shutdown stops admission, drains CPU replay, submits or discards only work as
  allowed by the existing shutdown contract, waits for outstanding GPU tokens,
  retires dependents in reverse ownership order, then destroys the allocator
  and device. No completion callback outlives its device.

### Allocation classes and accounting

- M4 defines four backend classes: `DeviceLocal`, `DynamicUpload`,
  `TransferUpload`, and `TransferReadback`. Static GPU resources prefer
  device-local memory and are never made host-visible merely to simplify
  initialization. Dynamic/upload classes require host visibility and request
  persistent mapping; readback additionally uses the correct host-access
  direction and invalidation policy.
- RHI descriptors select intent; Vulkan allocation classes and VMA flags remain
  backend-private. Unsupported or contradictory RHI usage is rejected by the
  existing backend-neutral validation before allocation.
- Flush and invalidate ranges are aligned to the selected memory type's
  non-coherent atom size. Coherent memory may avoid the native operation but
  follows the same observable byte-range contract.
- The statistics snapshot reports per-class live allocation count/bytes,
  allocation and failure totals, peak live bytes, dedicated-allocation totals,
  per-heap usage/budget where VMA provides it, upload/readback requested bytes,
  arena capacity/high-water/reuse/overflow/oversize counts, GPU wait count/time,
  and retirement pending/high-water/released counts. Heap budget is diagnostic,
  not a portable allocation guarantee.
- Statistics use checked 64-bit counters, are queryable without exposing VMA or
  Vulkan handles, and can be reset without clearing live resources or changing
  ordering. M5 may consolidate their presentation but does not redefine them.

### Transfer arenas and reusable pools

- Upload and readback arenas own persistently mapped pages. Each suballocation
  has byte offset, byte size, alignment, allocation class, and retirement token;
  a range returns to the free set only after that token completes.
- Arena pages are bounded by Stage 0 evidence. Page growth stops at the frozen
  per-class byte/page caps. A request larger than the selected page class uses
  one tracked oversize allocation; exhaustion may wait for the oldest exact
  token, but cannot silently grow without bound.
- Static buffer writes and texture uploads copy CPU bytes into upload ranges,
  flush exact ranges when required, and use the established public copy and
  transition authority. Texture readback copies into readback ranges, waits for
  its producing token, invalidates the exact range, then copies to owned CPU
  output before returning the range.
- Dynamic uniforms retain their public range identity but page reuse is gated
  by completion, not merely `FrameNumber % kFrameInFlight`. Descriptor pools
  and command buffers follow the same completion rule; their existing capacity
  bounds and cache statistics remain authoritative.
- Cache eviction removes lookup ownership only. Native objects and descriptor
  pools referenced by submitted work remain alive until completion regardless
  of cache, frame, viewport, or Renderer replacement activity.

## Current Foundations and Gaps

| Area | Existing foundation | M4 gap |
| --- | --- | --- |
| Submission | `FVulkanPayload`, per-submit pooled fences, reserved/published completion tokens, and a contiguous completed watermark on the ordered graphics queue. Stable statistics expose wait and retirement pressure. | Stage 4 removes remaining frame-slot selection from reusable pools. |
| Deferred deletion | One thread-safe queue covers allocated resources and most native object types and releases entries by exact completion token; stable statistics expose pending/high-water/released counts. | Remaining reusable pools move to the same completion authority in Stage 4. |
| Allocation | One VMA owner; four explicit allocation classes; validated selected properties; shared non-coherent range normalization; complete-or-failure factories; stable per-class and heap-budget statistics. | Transfer allocations remain one-shot until Stage 3; Stage 5 adds budget-pressure policy. |
| Transfers | Public exact copy/transition paths now consume bounded, persistently mapped upload/readback arena ranges with aligned offsets, exact-token retirement, oversize fallback, and stable pressure statistics. | Stage 5 applies heap-budget pressure policy and comparative soak evidence. |
| Descriptors and commands | Bounded descriptor batches select only completed tokens; each pool records its maximum use token. Submitted command buffers and fences return through the completion tracker. | Stage 5 qualifies mixed cache, viewport, replacement, and failure pressure. |
| Dynamic uniforms | Two completion-selected producer states own persistently mapped 4 MiB base pages and at most eight chunks each; every used chunk records its maximum token and contributes stable arena pressure. | Stage 5 compares wait, capacity, high-water, and reuse behavior under editor churn. |
| Tests | Public RHI transfer coverage, arena alignment/fragmentation/cap/oversize/failure/reuse, allocation-class properties, descriptor churn, token ordering/retirement, irregular empty frames, inline/threaded replay, and hidden runtime smoke. | Stage 4 adds exact-token pool reset coverage; Stage 5 adds pressure/soak comparison. |

## Stage 0 Evidence and Frozen Contract

The diagnostic snapshot uses 16 allocation-size buckets with upper bounds of
4 KiB, 8 KiB, 16 KiB, and so on by powers of two. Class indices in the capture
are `0=DeviceLocal`, `1=DynamicUpload`, `2=TransferUpload`, and
`3=TransferReadback`. Counters saturate at `uint64` maximum; reset preserves
live descriptor/deletion gauges and the latest heap snapshot.

### Baseline captures

| Workload | Allocation and transfer evidence | Wait, reuse, descriptor, and retirement evidence |
| --- | --- | --- |
| `FVulkanTextureSamplingTests.ThreadedResourceCreationAndUniformOverflowStayRHIThreadOwned` | Device-local: 34 allocations / 13,056 requested bytes / 512-byte peak, all in the first bucket. Dynamic upload: one 4,194,560-byte allocation. Transfer upload: 18 allocations / 1,104 bytes; transfer readback: 17 allocations / 1,088 bytes. Upload was 18 operations / 1,104 bytes / 80-byte frame peak; readback was 17 / 1,088 / 64. | No frame-fence wait; command buffers allocated/reused 6/31; deferred delete pending/high-water/released/max-age was 15/15/55/3 frames. Heap usage/budgets were 32 MiB/18.99 GiB, 32 MiB/19.12 GiB, and 3.34 MiB/171.20 MiB. |
| `FVulkanCreateFailureInjectionTests.RuntimeFactoriesReturnNullThenRecoverOnTheSameRHIThread` | The focused descriptor draw created one 512-set pool and allocated two sets. | Descriptor pool/count capacity/live/peak was 1/512/2/2; this supplements the transfer workload without changing its deterministic counts. |
| Hidden Sandbox editor, 120 ticks, `DURIN_VULKAN_MEMORY_BASELINE=1` | Device-local: 33 allocations / 50,215,692 bytes / 12,795,904-byte peak, distributed `[7,1,3,1,3,2,3,1,4,5,0,0,3,0,0,0]`. Dynamic upload: 16 / 8,597,956 / 4,194,304, distributed `[8,0,3,0,3,0,0,0,0,0,2,0,0,0,0,0]`. Transfer upload: 267 / 8,942,568 / 1,048,576, distributed `[204,14,7,14,7,9,7,1,4,0,0,0,0,0,0,0]`. Upload frame peak was 7,623,576 bytes; no readback occurred. | Frame-fence waits were 106 / 948,312,900 ns; command buffers allocated/reused 6/234; descriptor pools/capacity/live/peak were 2/1,024/54/54; deferred delete pending/high-water/released/max-age was 502/502/8,662/3. Heap usage/budgets were 68.61 MiB/18.99 GiB, 32 MiB/19.12 GiB, and 3.34 MiB/171.20 MiB. |

The editor command was `DevTool run --project Sandbox/Sandbox.dproject --args
--hidden-window --exit-after-ticks=120`. It exited normally. These numbers are a
selection baseline rather than portable performance promises; Stage 5 compares
like-for-like captures on the same profile and hardware.

### Native deletion ownership audit

| `FDeferredDeletionQueue::EType` | Current producer/owner | Frozen completion rule |
| --- | --- | --- |
| `RenderPass` | `FVulkanRenderPass` cache entry | Last submitted token of every command buffer which began the pass. Cache eviction drops lookup ownership only. |
| `Buffer` | `FVulkanBuffer`, with its VMA allocation | Maximum submitted token of recorded copy, bind, draw, dispatch, or transition payloads retaining the buffer. |
| `BufferView` | `FVulkanBufferView` | Maximum token of descriptor or native commands retaining the counted view. |
| `Image` | Locally owned `FVulkanTexture`, with its VMA allocation | Maximum submitted token of payloads retaining the texture or any counted view. Swapchain images remain externally owned. |
| `ImageView` | `FVulkanTextureView` | Maximum token of descriptors, framebuffer attachments, or commands retaining the view. Swapchain-owned views use the WSI boundary below. |
| `Pipeline` | `FVulkanGraphicsPipelineState` | Maximum token of command buffers which bound the pipeline. Structural-cache eviction does not shorten it. |
| `PipelineLayout` | `FVulkanGraphicsPipelineState` | Same submitted-token dependency as the owning pipeline and descriptor commands. |
| `Framebuffer` | `FVulkanFramebuffer` cache entry | Maximum token of command buffers which began its render pass. |
| `DescriptorSetLayout` | `FVulkanDescriptorSetLayoutCache` entry | Maximum token of descriptor sets/pipeline layouts whose submitted work retains it. |
| `DescriptorSet` | No producer; individual sets are pool-owned | Must not be enqueued individually. Pool reset/reuse follows the maximum token of all sets allocated from that pool. |
| `DescriptorPool` | No deferred producer; `FVulkanGlobalDescriptorPool` owns pools | Pool reset/reuse waits for the maximum token of its allocated sets; final destruction occurs after device token drain. |
| `Sampler` | `FVulkanSampler` | Maximum token of descriptor snapshots retaining the sampler. |
| `Semaphore` | `FVulkanSemaphore` for queue work; WSI resources also have an immediate retired path | Queue semaphores use the last signal/wait submission token. Present semaphores are destroyed only after their separate frame-resource present fence or fallback queue-idle boundary. |
| `ShaderModule` | `FVulkanShader` | Maximum token of pipelines/commands whose counted ownership retains the shader; pipeline creation alone needs no GPU token. |
| `Event` | No producer | Unsupported until a producer defines a last submitted use. |
| `ResourceAllocation` | No producer | Unsupported; VMA buffer/image allocations stay paired with their native handle. |
| `DeviceMemoryAllocation` | No producer | Unsupported until a concrete owner and last-use token exist. |
| `BufferSuballocation` | No producer | Reserved for arena ranges; Stage 3 returns ranges by their exact retirement token rather than this native queue. |
| `AccelerationStructure` | No producer | Unsupported until an acceleration-structure plan defines counted ownership. |
| `BindlessHandle` | No producer | Deferred follow-up; any future recycling consumes exact completion evidence. |

### Reset and reuse audit

| Site | Current boundary | Frozen replacement |
| --- | --- | --- |
| `FVulkanFrame::Prepare/Reset` | Wait/reset one of two frame fences; delete payloads and reset their command buffers. | Completion tracker polls all ordered submissions; payload dependencies and command buffers release when their own token completes. Exact waits occur only at a declared bound or synchronous result. |
| `FVulkanGlobalDescriptorPool::ResetPoolsForCurrentFrame` | Reset every pool selected by `FrameNumber % 2`. | Each pool carries its maximum submitted token and resets only after it is eligible. |
| `FVulkanDynamicUniformBufferAllocator::BeginFrameProducer` | Zero offsets for the selected two-frame slot. | Each page/range carries its last submitted token; reuse scans eligible pages before bounded growth or an exact oldest-token wait. |
| `FVulkanCommandBufferPool::FreeUnusedCommandBuffers` | Move only never-submitted or already frame-fence-reset buffers to the free list. | Submitted command buffers enter the free list directly from completion retirement; recording-only buffers remain immediately reusable. |
| `FVulkanFenceManager::ReleaseFence` | Frame owner releases and resets a known-signaled fence. | Completion records return their signaled fence to the pool only after observing the associated token. |
| Pending descriptor snapshots and structural caches | Begin-frame/cache-budget lookup ownership may be cleared or evicted. | Lookup removal remains independent; counted native dependencies survive to the maximum submitted token. |
| Viewport frame resources | Per-image present fence with `VK_EXT_swapchain_maintenance1`, otherwise graphics/present queue idle on swapchain teardown. | Remains a separate WSI retirement boundary. A graphics completion token proves rendering-done semaphore signal submission, not presentation-engine consumption. |

### Single-queue proof and arena bounds

Physical-device admission selects one queue family with graphics and Win32
presentation support. Logical-device creation requests exactly one queue;
`GraphicsQueue`, `ComputeQueue`, `TransferQueue`, and `PresentQueue` all point to
queue index zero, and `SetupPresentQueue` rechecks surface support. Therefore
all M4 command submissions have one total queue order. Presentation completion
is intentionally excluded from the token watermark and remains governed by
the per-image present fence or teardown fallback above.

The initial arena policy frozen from the captures is:

| Class | Page and cap | Alignment and oversize | Exhaustion wait |
| --- | --- | --- | --- |
| `TransferUpload` | 8 MiB pages, at most four pages / 32 MiB live arena capacity. The page size covers the 7,623,576-byte observed frame peak with headroom. | Suballocations use the maximum of 16 bytes, the request's Vulkan copy alignment, and `nonCoherentAtomSize`; allocation buckets are 16 B, 256 B, and 4 KiB. Requests larger than 8 MiB use one tracked dedicated oversize allocation. | Reclaim completed ranges first, then wait only for the oldest exact range token when the cap is exhausted. |
| `TransferReadback` | 4 MiB pages, at most two pages / 8 MiB live arena capacity. | The same alignment rule and buckets; requests larger than 4 MiB use one tracked dedicated oversize allocation. | A synchronous readback waits for its producing token. Capacity exhaustion may additionally wait for the oldest exact range token, never whole-device idle. |

Page counts and bytes are hard caps, not growth targets. Stage 5 must explain
any cap change with a new like-for-like capture before changing these values.

### Stage 1 completion evidence

- `FVulkanCompletionWatermarkTests.AdvancesOnlyAcrossContiguousObservedTokens`
  observes token 2 before token 1 and proves the watermark and retirement
  eligibility cannot skip the earlier token.
- `FVulkanCompletionIntegrationTests.SubmissionPublishesTokenAndRetirementWaitsForItsFence`
  proves a real Vulkan submission publishes its reservation, retains a pending
  native deletion, and releases both payload and native object only after the
  exact fence-backed token wait.
- `FVulkanCompletionIntegrationTests.EmptyIrregularFramesAdvanceBySubmissionRatherThanFrameAge`
  submits empty frames numbered `0, 17, 2, 101, 4`; tokens remain strictly
  monotonic and drain independently of frame age or cadence.
- `VulkanRHIIntegrationTests` passed all 31 cases after the migration, including
  allocation/device failure cleanup, threaded transfer churn, descriptor draw,
  viewport candidate recovery, and WSI paths. A hidden 120-tick Sandbox run
  exited normally without validation or shutdown errors.

## Implementation Stages

### Stage 0: Baseline Capture and Contract Freeze

- [x] Add diagnostic-only counters for VMA allocation size/class candidates,
  static upload/readback bytes and operation counts, frame-fence waits,
  descriptor pool usage, command-buffer reuse, and deferred-delete queue depth.
- [x] Capture a deterministic focused transfer/descriptor churn workload and a
  representative Sandbox editor run; record allocation-size distributions,
  per-frame and peak transfer bytes, wait sites/time, heap usage/budget,
  descriptor pressure, and deferred-delete high-water/age.
- [x] Audit every `FDeferredDeletionQueue::EType` producer and every frame-index
  reset/reuse site; classify its exact owner, last-use point, and required
  completion dependency.
- [x] Prove that all required M4 submissions use one ordered graphics queue and
  document how present fences remain a separate WSI retirement boundary.
- [x] Freeze upload/readback page size, alignment buckets, per-class page/byte
  caps, oversize threshold, and allowed exact-wait policy from the captures.
- [x] Add headless tests for completion-token ordering, watermark advancement,
  counter reset/saturation, and retirement eligibility before wiring Vulkan.

#### Acceptance Gate

- The baseline records concrete sizes, volumes, waits, budgets, descriptor
  usage, and delete depth; every native owner/reset site has one selected
  completion rule; arena numbers and the single-queue token contract are
  recorded in `Current Status` before policy changes begin.

### Stage 1: GPU Completion Authority and Native Retirement

- [x] Add the device-owned submission token allocator, fence-backed completion
  tracker, nonblocking poll, exact wait, completed watermark, and statistics.
- [x] Make payload submission publish a token atomically with successful queue
  submission and retain recorded native dependencies until that token retires.
- [x] Replace `GVulkanRHIDeletionFrameNumber` and frame-age entries with exact
  completion tokens, including cache eviction and cross-thread enqueue paths.
- [x] Preserve special presentation-fence ownership while preventing viewport
  or swapchain teardown from sweeping unrelated device resources.
- [x] Make shutdown drain tokens and release objects in an audited order before
  fence manager, allocator, logical device, and instance teardown.
- [x] Add delayed-fence and out-of-order-observation tests proving the watermark
  cannot skip an incomplete earlier submission or release its dependents.

#### Acceptance Gate

- Native objects survive while their token is pending and retire promptly after
  it completes under regular, empty, irregular, and delayed frame cadence;
  ordinary recycling adds no device-idle wait and CPU fence behavior is
  unchanged.

### Stage 2: Explicit Allocation Classes and Memory Statistics

- [x] Add backend-private allocation-class descriptors and deterministic VMA
  required/preferred flags for buffers, images, dynamic data, upload, and
  readback.
- [x] Route static buffers/textures to device-local preference, keep dynamic
  resources mapped and host-visible, and validate the selected memory
  properties after allocation.
- [x] Centralize non-coherent atom alignment and exact flush/invalidate handling
  for owners and future arena suballocations.
- [x] Publish stable RHI-facing memory/transfer/retirement statistics and VMA
  heap usage/budget snapshots without exposing native handles or types.
- [x] Extend allocation failure injection to assert diagnostic attribution and
  complete-or-null publication for each allocation class.

#### Acceptance Gate

- Tests observe device-local preference for static resources and correct mapped
  properties for dynamic/upload/readback resources on the supported hardware;
  coherent and non-coherent helpers cover exact ranges; statistics and failure
  diagnostics are accurate and resettable.

#### Stage 2 Evidence

- `VulkanRHIIntegrationTests` passed all 33 tests on the NVIDIA GeForce RTX
  3090, including explicit property selection for all four allocation classes,
  atom-aligned mapped-range clamping, live/peak/reset semantics, and per-class
  allocation failure attribution.
- `DevTool build --target all` completed successfully after the public
  `FDynamicRHI` statistics contract changed, ensuring every editor module uses
  the updated ABI.
- The Sandbox hidden-window editor ran for 120 ticks and exited normally with
  the rebuilt module set.

### Stage 3: Completion-Aware Upload and Readback Arenas

- [x] Implement bounded persistently mapped upload/readback pages, aligned
  suballocation, free-range coalescing, token retirement, oversize fallback,
  exact-oldest wait, and shutdown cleanup.
- [x] Migrate static buffer writes and texture initialization/update to upload
  ranges while preserving the established transition and copy paths.
- [x] Migrate scoped texture readback to readback ranges with exact producing-
  token wait and invalidate-before-copy ordering.
- [x] Exercise page reuse, fragmentation, wrap, cap exhaustion, oversize
  requests, interleaved upload/readback, allocation failure, and shutdown with
  outstanding ranges.

#### Acceptance Gate

- Repeated selected uploads/readbacks reuse native pages without per-operation
  Vulkan buffer allocation; no range is overwritten before completion; caps and
  waits are observable; all copied bytes remain correct in inline and threaded
  modes.

#### Stage 3 Evidence

- `VulkanRHIIntegrationTests` passed all 34 tests on the NVIDIA GeForce RTX
  3090. The dedicated arena test covers alignment, fragmented-hole reuse,
  bounded page exhaustion, exact-oldest waiting, oversize fallback, allocation
  failure, upload/readback interleaving, page reuse, and byte preservation.
- The existing inline/threaded public copy matrix and threaded churn test pass
  with one persistent upload page and one persistent readback page instead of
  one VMA buffer allocation per operation; transfer operation and byte totals
  remain unchanged.
- `DevTool build --target all` completed successfully after the public memory-
  statistics layout gained arena pressure fields. The Sandbox hidden-window
  editor then ran for 120 ticks and shut down normally with arena pages alive.

### Stage 4: Completion-Aware Backend Pool Reuse

- [x] Gate dynamic-uniform page reset/reuse by completion tokens while
  preserving public `FRHIUniformBufferRange` behavior and overflow bounds.
- [x] Reset/reuse descriptor pools only after the last submission referencing
  their sets completes; keep M3 snapshot/cache identity and statistics intact.
- [x] Return command buffers and submission fences to their pools by the same
  completion authority, removing redundant frame-age assumptions.
- [x] Verify cache eviction, shader/resource replacement, viewport churn, and
  empty/synchronous submissions cannot reuse descriptors, command storage, or
  uniform bytes early.

#### Acceptance Gate

- Dynamic uniform, descriptor, command-buffer, and fence reuse is safe under
  delayed GPU completion and irregular frame cadence, stays within declared
  bounds, and introduces no new normal-path device-idle wait.

#### Stage 4 Evidence

- `VulkanRHIIntegrationTests` passed all 34 tests. The threaded uniform-
  overflow/churn case verifies two completion-selected base producers,
  oversize allocation, exact submitted-token association, repeated transfer
  and resource replacement, and orderly shutdown. The descriptor-array draw
  verifies that its pool records the exact submission token before retirement.
- Existing completion tests continue to prove command-buffer and pooled-fence
  return only after contiguous token completion, including empty and irregular
  frame numbers; no frame-age return path remains.
- `RHICommandListTests` passed all 54 tests, preserving inline/threaded replay,
  synchronous-operation, resource-retention, and public uniform-range behavior.
- `DevTool build --target all` succeeded. The Sandbox hidden-window editor ran
  for 120 ticks and shut down normally; its log contained no error, validation
  VUID, or assertion diagnostic.

### Stage 5: Integration, Stress, and Failure Qualification

- [x] Run mixed resource creation/destruction, descriptor-array replacement,
  buffer/texture upload, texture readback, render, resize, minimize, empty-frame,
  and shutdown churn through public RHI commands.
- [x] Qualify the same state/lifetime cases in inline and dedicated-thread
  execution, including CPU replay completing well before GPU completion.
- [x] Inject allocation, descriptor-pool, and structural-cache failures at
  pressure boundaries; prove prior candidates remain usable and diagnostics
  name the responsible class/token/bytes.
- [x] Compare post-change captures with the Stage 0 baseline and record native
  allocation reduction, arena high-water/reuse, heap pressure, waits, and
  retirement lag; investigate any unexplained regression.

#### Acceptance Gate

- The roadmap M4 exit gate passes: placement is attributable, repeated
  transfers avoid per-operation allocation, and destruction/reuse is proven
  safe under irregular submission and frame cadence with validation-clean
  rendering and orderly shutdown.

#### Stage 5 Evidence

- `VulkanRHIIntegrationTests` passed all 34 tests. The recoverable-factory case
  injects descriptor-pool creation failure during a descriptor-array draw,
  closes the partial render pass on the RHI thread, observes no published pool,
  and succeeds on the immediate retry. Existing cases cover all allocation
  classes and structural/native factory boundaries.
- In the deterministic threaded churn capture, transfer-upload VMA allocations
  fell from 18 one-shot buffers to one 8 MiB page and readback allocations fell
  from 17 to one 4 MiB page, while operation/byte totals remained 18/1,104 and
  17/1,088. Upload/readback reuse was 17/16 with 80/64-byte high-water marks.
  Command buffers improved from 6 allocations/31 reuses to 3/34. The 17
  readback waits (2.47 ms total) are newly attributable exact-token waits
  replacing the old uncounted `deviceWaitIdle`, not additional stalls.
- Dynamic uniforms used 12,583,168 bytes of capacity including one tracked
  oversize chunk, reached 4,195,072 bytes high-water, and recorded one reuse,
  one overflow, and one oversize allocation. Transfer deletion pressure at the
  capture point fell from 15 pending/15 peak to 5/5, with maximum token lag 1.
- In the hidden Sandbox 120-tick capture, upload VMA allocations fell from 267
  one-shot buffers to two bounded 8 MiB pages. The pages served 261 operations
  and 9,451,524 bytes with 259 reuses and an 8,488,796-byte high-water mark.
  Dynamic uniforms used the two 4 MiB base pages, reached 6,016 bytes, and
  recorded 2,414 reuses. Command and descriptor figures remained 6/234 and
  2 pools/1,024 sets capacity/54 peak live sets. Waits changed from 106/948 ms
  to 102/922 ms; retirement changed from 502 pending/peak and lag 3 to 426 and
  lag 2.
- The post-change hidden window was `3840x2019`. Its three additional roughly
  29.4 MiB device-local allocations explain the device-local byte increase
  relative to the Stage 0 capture whose peak allocation was 12.8 MiB; no M4
  path creates a new device-local transfer resource. The transfer-class and
  wait comparisons remain attributable.
- `DevTool build --target all` succeeded, and the diagnostic hidden run exited
  normally without an error, validation VUID, or assertion diagnostic.

### Stage 6: Lasting Contract and Final Handoff

- [x] Publish the stable allocation, statistics, transfer-arena, and completion
  lifetime contract under `Documentation/Runtime/Rendering/`.
- [x] Update the RHI/Vulkan roadmap with M4 completion evidence and unblock the
  M5 diagnostics/conformance child plan.
- [x] Run focused affected native targets, then the full native aggregate
  because M4 crosses RHI execution, Vulkan resources, descriptors, submission,
  WSI retirement, and shared test infrastructure.
- [x] Complete the full Debug Editor build and hidden-window Sandbox smoke under
  the same Agent Build Profile, following the root build/run and native-test
  instructions.
- [x] Record exact test/build/runtime evidence, remaining conditional follow-ups,
  and the verified editor executable before marking the plan completed.

#### Acceptance Gate

- Lasting documentation owns the implemented contract; focused, aggregate,
  full-build, validation-clean runtime, and shutdown evidence is recorded; M5
  can assert stable public behavior without freezing temporary Vulkan details.

#### Stage 6 Evidence

- `VulkanRHIIntegrationTests` passed all 34 tests and
  `RHICommandListTests` passed all 54 tests after the final M4 implementation.
- `DevTool test --target all` exercised 1,386 native targets: 1,385 passed and
  the only failure was
  `FSourceReferenceIndexTests.RelocatesSharedSourceAndAllReferencingPackages`
  in `EditorAssetWorkflowTests`. The case failed identically when isolated;
  neither its test nor the mounted-source relocation implementation is in the
  M4 diff from the activation baseline.
- `DevTool build --target all` completed successfully for the
  `windows-msvc-x64` / `Win64-Debug-DurinEditor` profile.
- The Sandbox hidden-window editor completed 120 ticks and exited normally.
  Its final log contained no error, Vulkan validation/VUID, or assertion match.
- The verified editor executable is
  `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`.
- [Vulkan Memory and GPU Completion](../Runtime/Rendering/VulkanMemoryAndGPUCompletion.md)
  owns the lasting contract, and the roadmap records M4 complete with M5 ready.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| CPU executor serial -> GPU token | Replay completion releases command storage but cannot retire native dependencies; exact CPU fence results and executor modes remain unchanged. |
| Queue submission -> completion watermark | Successful batches receive ordered tokens; polling/waiting cannot skip incomplete work; failed submission publishes no completed token. |
| Resource/view -> deferred destruction | Every native handle survives its last submitted use and retires after exact completion under delayed and irregular frames. |
| RHI usage -> VMA allocation class | Static, dynamic, upload, and readback paths select the documented required/preferred properties and reject or diagnose allocation failure atomically. |
| CPU write -> device-local use | Upload ranges flush as required, transition/copy exact ranges, survive until completion, and reuse without byte corruption. |
| GPU write -> CPU readback | Readback ranges wait for the producing token, invalidate as required, copy exact bytes, and return to the arena only after completion. |
| Token -> arena/pool reuse | Upload/readback ranges, dynamic uniform bytes, descriptor pools, command buffers, and fences cannot reset or recycle early. |
| Pressure -> statistics/diagnostics | Heap usage/budget, live/peak class bytes, transfer volume, arena cap/overflow, wait, descriptor, and retirement counters match forced workloads and reset safely. |
| Failure -> publication | Allocation/pool failure leaves prior resources and caches usable, publishes no partial candidate, and reports class, bytes, budget context, and native result. |
| WSI/shutdown -> retirement | Resize/minimize/viewport teardown preserves separate present completion; shutdown drains outstanding work without unrelated device-idle sweeps, use-after-free, or leak. |
| Inline -> threaded execution | The same transfer and lifetime scenarios pass; threaded mode additionally proves CPU replay can finish while GPU owners remain retained. |

Validation follows [Native Tests](../Development/Build/NativeTests.md) and
[Build and Run](../Development/Build/BuildAndRun.md). Start with the smallest
affected targets, including `RHICommandListTests`,
`VulkanRHIIntegrationTests`, and renderer/Vulkan reload or viewport targets
touched by a stage. Hardware-backed cases declare the repository GPU resource
lock; token-order and failure paths retain headless coverage where possible.

## Definition of Done

- GPU completion, not frame age or CPU replay, governs all native destruction
  and reusable memory/object pools in the selected single-queue path.
- Static, dynamic, upload, and readback placement policies are distinct and
  verified; heap budget and per-class live/peak pressure are queryable.
- Repeated buffer/texture upload and scoped readback use bounded reusable arenas
  without per-operation native allocation or in-flight overwrite.
- Dynamic uniforms, descriptors, command buffers, fences, resources, views,
  pipelines, and framebuffers survive through their exact last GPU use.
- Statistics attribute allocation, transfer, wait, descriptor, and retirement
  pressure without changing ordering or requiring validation facilities.
- Allocation and pool failures remain complete-or-null; submission/state
  failures retain their established terminal behavior.
- Focused M4 native tests, full Debug Editor build, validation-clean
  hidden-window runtime smoke, irregular cadence, and orderly shutdown pass
  with evidence recorded. The full native aggregate is recorded with one
  independently reproduced, out-of-scope Editor asset-source relocation
  failure and 1,385 passing targets.
- The lasting contract is published, the roadmap marks M4 complete, and M5 is
  ready to become active.

## Deferred Follow-ups

- A render graph and transient aliasing remain evidence-gated by measured
  transient pressure after M4.
- Asset texture residency, streaming admission, and eviction consume M4
  accounting in a separate Renderer/asset plan.
- Separate transfer/compute queues require multi-queue token aggregation,
  ownership transfers, fallback, and measured overlap in the C3 plan.
- Bindless handle recycling consumes M4 completion evidence only after M5 and a
  measured descriptor bottleneck.
- Device-loss recovery needs a complete Renderer resubmission inventory and
  product policy after M5.

## Related Documentation

- [RHI and Vulkan Backend Evolution roadmap](../Roadmaps/RHIAndVulkanEvolution.md)
- [RHI Capabilities and Vulkan Startup](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [RHI Resource Transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Resource Views and Transfers](../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [Graphics State and Bindings](../Runtime/Rendering/GraphicsStateAndBindings.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Asynchronous Texture2D Build and Readiness](AsynchronousTexture2DBuildAndReadiness.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/VulkanRHI/Public/VulkanDynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanSubmission.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanSubmission.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanQueue.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanCommandBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDescriptorSets.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.cpp`
- `Engine/Tests/Native/RHITests/Private/RHICommandListTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
