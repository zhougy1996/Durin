# Vulkan Memory and GPU Completion

Summary: Define stable RHI memory statistics and Vulkan allocation, transfer,
GPU-completion, reuse, destruction, and shutdown contracts.

Modules: RHI, VulkanRHI

## Completion Domains

Context `Finalize()` seals recording and returns a uniquely owned payload; it
does not submit native work. `FVulkanSubmissionCoordinator` is the single
native submission entry on the existing RHI thread. Explicit flush, frame end,
presentation, readback and bounded allocator pressure use that entry. Contexts
never submit one another directly. Payloads carry their own reserved ticket,
recorded storage and timing-query references, so a later reservation cannot
change an earlier payload's completion identity. Logical submission receipts and
ownership releases publish that recording payload's ticket, never the queue's
latest reservation, including when multiple contexts interleave on one queue.
Discarding an unsubmitted
payload cancels its ticket and resets its command buffers; ambiguous native
submission failure transfers ownership to quarantine instead of this path.

The coordinator owns sealed, pending payloads between submission scopes.
Enqueueing a context seals its current recording without submitting native work.
Explicit submission, presentation, readback and allocator pressure drain these
payloads together with participating contexts. Cancellation and device shutdown
discard queued recordings while their context command pools still exist.
The coordinator seals all participating contexts before submitting a batch.
It validates dependency authority and success state, includes implicit queue-local
reservation order, and constructs a deterministic topological order before native
submission. Each physical timeline also preflights its ordered pending prefix;
an earlier pending reservation outside the batch rejects the entire batch before
any queue submits, including independent work on other queues. Submitted and
canceled reservations do not create missing-work failures. RHI
`CanSubmitBatch` only observes admission; it never marks tickets submitted.
Pending producers must belong to the same batch; missing producers
and dependency cycles reject the batch and discard its unsubmitted payloads.
Native submission still requires accepted producers. If a native call fails after
earlier payloads were accepted, those earlier submissions retain their normal
completion ownership, the ambiguous call stays quarantined, and remaining
unsubmitted payloads are discarded. Batch validation does not roll back replayed
resource-state metadata or GPU work already accepted by a previous batch.

Vulkan resolves public submission scopes to the requested provisioned physical
queue. When independent queues exist, scope end seals the producer into the
coordinator; a later graphics join cannot be merged into that producer's payload.
Cross-queue waits retain the producer ticket for batch ordering and native
timeline lowering. Same-queue scopes retain the coalescing path on single-queue
devices. Diagnostic and timing intervals must close before changing physical
queues, including the return from a compute scope to the default graphics context.

CPU executor serial completion and Vulkan queue completion are different
proofs. An executor serial proves replay and release of executor storage ownership;
it never proves that the GPU has stopped referencing native resources.

The default Vulkan topology has one ordered graphics/present queue; diagnostic
provisioning can add an independent compute queue. Recording
reserves a monotonically increasing completion token, and successful
`vkQueueSubmit` publishes that token with one pooled fence and a queue-qualified
RHI submission ticket. Polling or an exact
wait advances a contiguous completed-token watermark; it cannot skip an older
unsignaled submission. Submission failure publishes no completion and remains
terminal. Each physical `FVulkanQueue` owns its completion tracker; queue roles
may alias the same queue but cannot create a second authority for it. Device
generation is allocated once per device and shared by its queue timelines.
Exact waits route by queue ID and verify ticket ownership, including rejection
of a foreign timeline claiming identical device/queue coordinates. Polling and
device teardown enumerate physical queues once. Native payloads may submit only
through their owning queue. When supported, each queue additionally owns a
timeline semaphore and signals its ticket value at native submission. Cross-queue
payload waits require accepted producer tickets from this device; every success
dependency is validated before native waits are reduced to one maximum per
producer queue. Binary WSI waits/signals remain distinct and use zero values in
the mixed timeline submit descriptor. Native queue waits do not substitute for
resource access/layout barriers or exclusive-family ownership transfers.

Submitted payloads, replay-storage leases, command buffers, and fences stay
owned by the completion tracker until their ticket retires. Native buffers, images, views, samplers,
pipelines, layouts, framebuffers, descriptor objects, and other deferred
handles retain queue-qualified retirement prerequisites. Native deletion
conservatively captures the last reserved ticket from every physical queue;
the default topology contains one queue. The numeric graphics token is
retained for graphics-only transfer/timing policy and diagnostic lag, not as a cross-queue
comparison. Frame number, CPU serial, cache age, and object age are
not GPU-lifetime evidence. Present fences and semaphores remain owned by each
viewport's frame resources because presentation completion is a distinct WSI
contract.

Ordinary maintenance polls without blocking. Blocking is limited to a
synchronous result, reuse after a declared bound is exhausted, frame pacing
that targets an exact token, or orderly shutdown. These waits are counted; a
whole-device idle wait is not an ordinary recycling mechanism.

## Allocation Classes

The public RHI describes resource intent while Vulkan keeps VMA policy private.
Four stable statistics classes describe the selected intent:

| Class | Vulkan policy |
| --- | --- |
| `DeviceLocal` | Static buffers and images use automatic placement with a device-local preference. Initialization does not make them host-visible. |
| `DynamicUpload` | Dynamic and uniform storage requires host-visible memory, persistent mapping, and sequential host write access. |
| `TransferUpload` | Upload pages require host-visible, persistently mapped sequential-write memory and transfer-source usage. |
| `TransferReadback` | Readback pages require host-visible, persistently mapped random host access and transfer-destination usage. |

Successful host classes validate both the selected host-visible property and a
non-null persistent mapping. Candidate failure publishes no partial resource
and reports class, requested bytes, native result, and the current heap budget
snapshot. VMA heap usage and budget are diagnostic pressure evidence, not a
portable promise that a later allocation will succeed.

Flush and invalidate use one mapped-range normalizer. It aligns the start down
and end up to `nonCoherentAtomSize`, clamps the tail to the allocation, and
preserves the requested observable byte range. Readback always waits before
invalidate and copies into caller-owned CPU storage before releasing its range.
The VMA result from each flush or invalidate is checked at this memory-manager
boundary. Failure reports the allocation class, requested and normalized
ranges, and native result, then terminates the current execution path; upload
or readback cannot continue with stale mapped data.

## Transfer Arenas

The device owns two bounded, persistently mapped arenas:

| Arena | Normal pages | Oversize rule |
| --- | --- | --- |
| Upload | 8 MiB each, at most four pages / 32 MiB | A request larger than 8 MiB receives one tracked allocation. |
| Readback | 4 MiB each, at most two pages / 8 MiB | A request larger than 4 MiB receives one tracked allocation. |

Suballocations align to at least 16 bytes, the noncoherent atom size, the
required copy-offset alignment, and the texture block size where applicable.
Free intervals split and coalesce. Each live range exposes an allocation lease
retained by both the CPU range and its recording payload. Retiring or destroying
the CPU range cannot free an interval while a payload still retains it. Reuse
requires lease expiration, including after unsubmitted payload cancellation;
failed submissions keep their leases in quarantine. Each page has physical-queue affinity;
fully free pages may be replaced for another queue within the capacity bound.
At capacity, CPU retirement order selects a retired range. The coordinator
uses the range's queue-qualified ticket to locate and submit its producer if
necessary, then waits the actual payload owners through the coordinator and
retries. The ticket is a producer locator, not an independent reuse authority.
Normal pressure does not wait for unrelated device work. Arena teardown
drains submitted queues before destroying native pages.

Static device-local buffer writes and texture updates use upload ranges while
retaining the public transition/copy authority. Scoped texture readback uses a
readback range, submits its producing payload through the coordinator, waits its exact ticket,
invalidates the range, copies exact packed bytes, and then retires the range.
Repeated operations therefore reuse native pages without overwriting in-flight
bytes or allocating one Vulkan buffer per operation.

## Uniform, Descriptor, Command, and Fence Reuse

Frame retirement first submits both provisioned command contexts, then captures
the physical queue prefixes. Reusing a frame waits all of those prerequisites.
The render-thread begin-frame flush completes that wait before resetting the
frame's mapped storage producer, so graphics completion alone cannot authorize
CPU overwrite while compute remains in flight.

Dynamic uniforms keep two producer states with one 4 MiB base page each. Pages
are frame-owned and conservatively inherit all queue prefixes from their frame,
rather than a graphics-only token. The RHI thread selects a state only when
those prerequisites retire; if both states remain busy, it waits the state
retired earliest on the CPU. Each producer is bounded to eight chunks, including
tracked oversize chunks. Public
`FRHIUniformBufferRange` buffer/offset/size behavior and alignment remain
unchanged.

Descriptor allocation rotates between at most two pool batches. Every graphics
or compute descriptor bind, including cache hits, retains the active batch's
allocation lease in its payload. Frame retirement drops the allocator's active
lease; a batch can reset only after all recording and in-flight owners release
it. Discarding an unsubmitted payload returns its lease; normal GPU completion
returns submitted leases, and failed submissions keep them quarantined.
The pool stores no ticket set or completion watermark. If both batches remain
busy, the coordinator finds the actual submissions holding the oldest retired
batch and waits their queue-qualified tickets. Unsubmitted external owners are
rejected as a pressure wait target. This lookup is limited to pressure and
diagnostics, not ordinary binds. The legacy test token array derives its graphics
projection from payload ownership and never controls reuse.
Command-context descriptor snapshots remain bounded
and frame-local as binding caches, but clearing a snapshot never authorizes an
in-flight native pool reset. Command buffers and submission fences return to
their pools directly from completion-tracker retirement, not from a frame-age
path.

Live descriptor snapshot and retained-value occupancy belong to the command
context and change in the same operation that commits insertion, eviction,
frame clear, pipeline deletion, or context reset. Failed candidates and cache
hits are occupancy-neutral. Statistics reset preserves these exact live totals
while clearing accumulated counters; production cache mutations do not derive
them by traversing every pipeline state.

## Statistics and Reset

`FDynamicRHI::RHIGetMemoryStatistics` returns a non-waiting,
backend-neutral `FRHIMemoryStatistics` snapshot. Per allocation class it
reports live allocation count/bytes, peak live bytes, allocation count/bytes,
failures, dedicated allocations, largest allocation, arena capacity/live/high-
water bytes, reuse, overflow, oversize, and exact-wait counts. The snapshot also
contains VMA heap usage/budget, upload/readback operations and bytes, GPU wait
count/time, and native retirement pending/high-water/released/token-lag values.

`RHIResetMemoryStatistics` clears interval counters without changing ordering
or freeing resources. It preserves live allocation gauges, arena capacity/live
bytes, descriptor live occupancy, retirement pending depth, and the current
heap snapshot. Reset peaks restart from their corresponding live value.

The consolidated `FRHIDiagnosticSnapshot` composes this value unchanged and
mirrors its retirement fields into the Completion section for attribution. Its
single reset boundary delegates back to this authority; it does not maintain a
second memory counter store. See
[RHI Diagnostics and Conformance](RHIDiagnosticsAndConformance.md).

## Shutdown and Failure

Queue submission prepares its tracker entry, payload vector and fence ownership
before calling Vulkan. The successful commit allocates no ownership storage.
If the native call fails, its payloads and fence remain quarantined; neither
the signal value nor normal retirement is published. Device teardown stops
native work (or observes device loss), then destroys quarantined fences without
reset/reuse and detaches payloads before destroying their command pools.
Outstanding ticket metadata becomes terminal failure, never synthetic success.

Shutdown stops new work through the existing executor contract, drains CPU
replay, waits all published completion tokens, destroys command contexts and
completion-aware arenas/pools, clears deferred native resources, then destroys
VMA, queues, fences, and the device in dependency order. No completion callback
or reusable page outlives its device.

Allocation, transfer-page, descriptor-pool, and structural-cache candidate
failures publish nothing and leave earlier candidates usable. Submission,
state-contract, presentation, and device-loss failures retain their established
terminal behavior.

## Related Documentation

- [RHI command execution](RHICommandExecution.md)
- [RHI resource transitions](RHIResourceTransitions.md)
- [RHI resource views and transfers](RHIResourceViewsAndTransfers.md)
- [Graphics state and bindings](GraphicsStateAndBindings.md)
- [Viewport rendering](ViewportRendering.md)
- [RHI diagnostics and conformance](RHIDiagnosticsAndConformance.md)
- [RHI and Vulkan backend evolution roadmap](../../Roadmaps/Archive/2026-08/RHIAndVulkanEvolution.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanCompletion.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanCompletion.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTransferArena.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTransferArena.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDescriptorSets.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDiagnostics.cpp`
