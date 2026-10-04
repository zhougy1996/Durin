# RHI Resource Transitions

Summary: Define portable buffer and texture access handoffs, exact range
semantics, recorded command behavior, and the authoritative Vulkan state model.

Modules: RHI, VulkanRHI

## Portable Access Contract

`ERHIAccess` describes how an RHI resource was last used and how it will be
used next. Callers express portable intent; Vulkan pipeline stages, access
masks, image layouts, dependency flags, and native handles remain
backend-private. The vocabulary covers vertex and index reads, graphics and
compute uniform reads, graphics and compute shader reads or read/write access,
transfer reads and writes, host reads and writes, indirect-argument reads, color and depth/stencil
attachment access, presentation, and discard.

Compatible read-only intents may be combined. Write intents are exclusive,
apart from the deliberately named shader read/write states. `None` represents
an uninitialized tracked state. `Discard` is valid only as an expected state:
it is a compatibility wildcard that discards prior contents while retaining
all tracked source stages and accesses, and never becomes the resulting state.
New transitions express content discard independently with
`bDiscardContents`, keeping an exact `ExpectedBefore`. Vulkan may use an
undefined old image layout for discard, but still waits for every overlapping
tracked access, including mixed ranges in resources reused across graphs.

`ExpectedBefore` is a checked precondition, not a hint. Replay fails with a
resource-qualified diagnostic when the tracked state for any selected range
does not match it. `RequiredAfter` becomes the tracked state only after the
complete transition batch has validated and its barrier has been recorded.

## Exact Transition Descriptors

`FRHIBufferTransition` names a nonempty byte range with `Offset` and `Size`.
`FRHITextureTransition` names nonempty color, depth, or stencil aspects and an
exact mip and array-layer range through `FRHITextureSubresourceRange`.
`FRHIBufferTransition::Whole` and `FRHITextureTransition::Whole` construct the
corresponding whole-resource ranges; they do not weaken range validation.

Public validation rejects null or wrong-kind resources, empty and overflowing
ranges, out-of-bounds subresources, invalid aspect/format combinations,
unsupported access combinations, access incompatible with resource usage, and
overlapping ranges for the same resource in one batch. Disjoint buffer ranges
and texture aspects, mips, or layers remain independently tracked.

## Recording and Replay

`FRHICommandListBase::TransitionBuffers` and `TransitionTextures` record typed
commands on regular or immediate command lists. A command owns its descriptor
payload and retains every referenced resource until replay completes. Empty
batches are no-ops; invalid batches fail while recording. Transitions are legal
outside a render pass with no active pipeline or any admitted pipeline, and are
rejected inside a legacy render pass because the pass owns attachment
dependencies.

Replay routes through the operation context's `RHITransitionBuffers` and
`RHITransitionTextures`. Command ordering, retained lifetime, and observed
state are identical in inline and dedicated-thread execution; threaded callers
use the existing submission serial and fence contract when they need CPU
completion.

## Split Transition Contract

`Experimental/RHITransition.h` defines one execution-local, single-use
transition object. `RHICreateTransition` validates and copies its exact buffer
and texture ranges, and the object retains every resource through its final
recorded and backend use. `BeginTransition` and `EndTransition` must pair on the
same command list, cannot nest the same object, cannot overlap another open
transition on the same range, and are illegal inside a render pass. A command
list cannot be sealed while a transition remains open. Graph and manual callers
must not access a transitioned range between the pair; overlapping ordinary
transition commands are rejected while it is open.

Backend split support is published by `FRHIQueueCapabilities::bSplitBarriers`
independently of independent-compute or queue-family support. An unsupported
backend records Begin as a no-op and lowers End to the same complete buffer and
texture barriers used by the ordinary API. This fallback preserves validation,
state updates, resource retention, and results without claiming overlap.

RDG compiles candidate begin/end relationships only when one exact producer
batch precedes the consumer batch. It groups transitions with the same pair,
resolves all physical backings centrally during execution preparation, and uses
split lowering only when both batches map to one physical queue and the backend
publishes support. Begin records after the producer batch; End records before
the consumer barriers. Cross-queue ranges continue through queue ownership
transfer, initial transitions and ambiguous multi-producer ranges remain full,
and transitions within one batch remain full. Consequently begin/end are always
outside render passes and upload/pass batching cannot separate a pair illegally.

The Vulkan backend implements native split lowering with matching
`vkCmdSetEvent2` and `vkCmdWaitEvents2` dependency descriptions. It publishes
the capability only when synchronization2 and native device events are both
available and `DURIN_VULKAN_SPLIT_BARRIERS=1` explicitly selects the diagnostic
policy. Portability-subset devices are conservatively kept on the full-barrier
fallback because Durin does not enable their optional event feature; Apple
MoltenVK additionally reports native events unsupported. Native split remains
off by default until its performance gate is qualified; support is not inferred
from multi-queue availability.

## Metal State Authority

Metal stores portable access state with each native resource wrapper. Buffer byte
ranges and texture aspect/mip/layer ranges remain independent. Replay preflights
an entire transition batch, including native resource identity and exact
`ExpectedBefore`, before applying any `RequiredAfter`. A mismatch reports the
resource, selected range, expected, tracked and requested accesses. `Discard`
waives only the previous-state comparison; `bDiscardContents` does not waive it.

The current backend uses one queue and tracked Metal resources, so these portable
state updates require no separate native barrier. Ending transfer/compute encoders
and the existing command ordering preserve native hazard synchronization. Split
transition fallback uses the same checked ordinary transition path.

Buffer initialization and convenience writes publish canonical usage access;
raw uploads publish `TransferWrite`. Texture uploads publish the canonical shader
read or storage access for their selected subresource. Render passes check their
attachment initial accesses and publish final accesses when the pass ends. Native
hazard tracking does not replace these portable state preconditions.

## Vulkan State Authority

Each Vulkan buffer owns an interval state tracker, and each Vulkan texture owns
state per aspect, mip, and array layer. Explicit transitions, buffer writes,
texture initialization and upload, readback, render-pass entry and exit, and
swapchain acquire and presentation all validate or update these same trackers.
There is no parallel layout-only authority.

The backend maps portable access through one pure mapping to Vulkan stage,
access, and image-layout values. If the immutable startup capability
`bSupportsSynchronization2` is true, replay emits synchronization2 barriers;
otherwise it lowers the same mapping to legacy pipeline barriers. Both paths
share validation and state-commit behavior. Capability selection precedes
lowering, so each input constructs exactly one native barrier for the active
path and no record for the inactive representation. Barrier vectors are
operation-local and reserve only the current batch size; their capacity cannot
be retained by the command context after either ordinary or burst batches.

`IndirectArgumentRead` is a buffer-only, combinable read intent admitted only
for `DrawIndirect` resources. Vulkan maps it to draw-indirect stage and
indirect-command-read access in both synchronization2 and legacy forms; it is
not a graphics or compute shader read.

A transition batch is validated completely before native recording begins.
Buffer intervals split around exact writes and merge again when adjacent
ranges reach the same state. Texture state changes only for the selected
planes and subresources. Tracker state commits after barrier recording returns,
so a rejected batch cannot partially advance the authority.

Legacy render passes retain ownership of their native attachment dependencies.
Pass entry checks each attachment's declared initial access against the common
tracker; successful pass completion commits declared final access for color,
resolve, depth, and stencil subresources. Upload and readback temporarily enter
transfer states through the same mapping and restore the exact prior tracked
state. Swapchain state is retained per native image across wrapper rebinding;
acquire and present do not imply queue-family ownership transfer.

## Boundary and Follow-ups

Vulkan additionally tracks exclusive queue-family ownership by buffer byte
range and texture aspect/mip/layer range. Ordinary barrier recording first
validates the entire batch against that ownership and claims unowned ranges
only after recording succeeds. The ownership state machine can release a
range set under a transfer identity and acquire only the identical set with
matching source and destination families. Released ranges reject accesses,
including discard; unrelated ranges retain their prior ownership. Multi-range
release/acquire rejects mismatches before mutation and rolls metadata back if
allocation fails. Ownership state is recorded execution order, not GPU
completion or permission to recycle storage.

`FVulkanQueueTransfer` owns an immutable resource/range description and a
single-use native release/acquire pair. Source recording captures actual
access scopes and layouts; both sides retain identical ranges, and distinct
families retain identical source/destination family indices and old/new
layouts. Shared-family handoffs omit ownership indices and perform the image
layout change once, on release. Acquire waits for the accepted release sync point
through the native queue's timeline dependencies. Synchronization2 and legacy
lowering use the same saved access mapping. Context payloads retain the pair
and its resource references until their respective queue completions. State
changes are prepared on copies before recording and committed afterward, so
allocation or pairing rejection cannot partially change the tracked resources.
`RHICreateQueueTransfer` exposes this protocol as a shared `FRHIQueueTransfer`.
Creation copies metadata and retains resources without submitting GPU work or
waiting for RHI replay. Invalid or unsupported descriptions return null.
`ReleaseQueueOwnership` and `AcquireQueueOwnership` record owning references
outside render passes; within a GPU submission scope, their source or destination
must match the scope's physical queue. Discarding a recording releases its
references without replay. Vulkan routes each side to the corresponding
device-owned context and retains the pair in that context's native payload.
Acquire replay may reference a recorded, unsubmitted release; it adds a GPU
timeline wait and does not wait on the CPU. Explicit GPU submission seals both
provisioned contexts and the coordinator orders producer before consumer using
the [submission batch contract](VulkanMemoryAndGPUCompletion.md#completion-domains).
An unsubmitted producer must be present in that batch. Production RDG scheduling
still uses graphics.

Ordinary transition commands still emit full barriers on their recording queue.
Counted resource views and recorded transfers
consume these exact ranges through the separate
[RHI resource views and transfers](RHIResourceViewsAndTransfers.md) contract. Compute access intent and Vulkan
mapping are available for the synchronous compute pipeline. New transfer commands and
views consume these exact range semantics rather than defining another state
system.

Correctness does not rely on `RHIBlockUntilGPUIdle`, incidental command-buffer
submission, or graphics-only stage assumptions. Existing synchronous readback
may still wait for its utility work to complete so CPU bytes are available;
that completion wait is separate from the memory dependency expressed by the
transition.

The staged adoption of render-graph barrier synthesis, Renderer migration,
transient allocation, and later queue scheduling is owned by the
[Render Graph Architecture Roadmap](../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md).

## Related Documentation

- [RHI public API stability](RHIPublicAPIStability.md)
- [RHI command execution](RHICommandExecution.md)
- [RHI resource views and transfers](RHIResourceViewsAndTransfers.md)
- [RHI capabilities and Vulkan startup](RHICapabilitiesAndVulkanStartup.md)
- [Texture system](TextureSystem.md)
- [Viewport rendering](ViewportRendering.md)
- [Compute shader pipeline roadmap](../../Roadmaps/Archive/2026-08/ComputeShaderPipeline.md)
- [RHI and Vulkan backend evolution roadmap](../../Roadmaps/Archive/2026-08/RHIAndVulkanEvolution.md)
- [Render Graph architecture roadmap](../../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)
- [Build and run](../../Development/Build/BuildAndRun.md)
- [Native tests](../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResourceState.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResourceState.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.cpp`
