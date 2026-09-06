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
transfer reads and writes, host reads and writes, color and depth/stencil
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

The current contract is single-queue and does not provide queue-family
ownership transfer, asynchronous scheduling, render-graph barrier synthesis,
or GPU-completion retirement. Counted resource views and recorded transfers
consume these exact ranges through the separate
[RHI resource views and transfers](RHIResourceViewsAndTransfers.md) contract. Compute access intent and Vulkan
mapping are available for the synchronous compute pipeline, but compute PSO
creation and dispatch remain a separate milestone. New transfer commands and
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
