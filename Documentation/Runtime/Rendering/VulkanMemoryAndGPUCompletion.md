# Vulkan Memory and GPU Completion

Summary: Define stable RHI memory statistics and Vulkan allocation, transfer,
GPU-completion, reuse, destruction, and shutdown contracts.

Modules: RHI, VulkanRHI

## Completion Domains

CPU executor serial completion and Vulkan queue completion are different
proofs. An executor serial proves command replay and command-storage release;
it never proves that the GPU has stopped referencing native resources.

The supported Vulkan topology has one ordered graphics/present queue. Recording
reserves a monotonically increasing completion token, and successful
`vkQueueSubmit` publishes that token with one pooled fence. Polling or an exact
wait advances a contiguous completed-token watermark; it cannot skip an older
unsignaled submission. Submission failure publishes no completion and remains
terminal.

Submitted payloads, command buffers, and fences stay owned by the completion
tracker until their token completes. Native buffers, images, views, samplers,
pipelines, layouts, framebuffers, descriptor objects, and other deferred
handles record the last token that may reference them and retire only when the
watermark reaches it. Frame number, CPU serial, cache age, and object age are
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
Free intervals split and coalesce. Every live range carries the reserved token
of the payload that records its copy; it cannot return to the free set until
that token completes. At capacity, the context submits the current payload if
the oldest range is only reserved, waits that exact oldest token, and retries.
It does not grow past the cap or wait for unrelated device work.

Static device-local buffer writes and texture updates use upload ranges while
retaining the public transition/copy authority. Scoped texture readback uses a
readback range, finalizes its producing payload, waits its exact token,
invalidates the range, copies exact packed bytes, and then retires the range.
Repeated operations therefore reuse native pages without overwriting in-flight
bytes or allocating one Vulkan buffer per operation.

## Uniform, Descriptor, Command, and Fence Reuse

Dynamic uniforms keep two producer states with one 4 MiB base page each. The
RHI thread selects a state only when every used chunk's maximum token is
complete; if both states are pending, it waits the older exact token. Each
producer is bounded to eight chunks, including tracked oversize chunks. Public
`FRHIUniformBufferRange` buffer/offset/size behavior and alignment remain
unchanged.

Descriptor allocation rotates between at most two pool batches. Pools record
the maximum end-frame token that may reference their allocated sets and reset
only after it completes. Command-context descriptor snapshots remain bounded
and frame-local as binding caches, but clearing a snapshot never authorizes an
in-flight native pool reset. Command buffers and submission fences return to
their pools directly from completion-tracker retirement, not from a frame-age
path.

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
- [RHI and Vulkan backend evolution roadmap](../../Roadmaps/RHIAndVulkanEvolution.md)

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
