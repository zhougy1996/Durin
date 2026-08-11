# RHI Resource Views and Transfers

Summary: Define counted resource-range views, canonical binding and attachment
lowering, and validated recorded copies shared by RHI and Vulkan.

Modules: RHI, RenderCore, VulkanRHI

## Counted View Contract

`FRHIBufferView` and `FRHITextureView` are immutable counted RHI resources.
Each view retains its parent resource and publishes its exact description, so
command payloads, descriptor state, and framebuffer-cache entries never rely
on a parent outliving a raw native view handle.

A buffer view selects a byte offset and size. Uniform and storage ranges are
unformatted; formatted-buffer views additionally name a pixel format and
require the parent `FormattedBuffer` usage. Validation checks nonempty and
overflow-safe bounds, required alignment, view type, format compatibility, and
the parent usage needed by uniform, storage, or formatted interpretation.

A texture view selects aspect, first mip and mip count, first array layer and
layer count, dimension, format, and sampled, storage, or attachment usage.
Validation checks exact parent bounds, aspect/format compatibility, dimension,
sample count, cube face grouping, usage, and format reinterpretation. The
current backend supports 2D and cube identities selected by the published
texture capability contract.

Default view descriptors are deterministic lowering helpers for a whole
resource; they do not add implicit range semantics. View factories are
fallible and publish a complete counted view or null. Vulkan creates a native
buffer view only for formatted buffers and an exact native image view for each
texture description. Native view destruction uses the deferred deletion path.

## Binding and Attachment Ownership

Shader-parameter recording accepts the compatibility scalar resource form but
canonicalizes every buffer or texture binding into a counted view before the
command is retained. Explicit caller ranges become exact buffer views; scalar
textures and whole-buffer bindings use canonical default descriptions.
Vulkan descriptor writes consume the retained Vulkan view object rather than
reconstructing a native range from a raw resource.
Sampled and storage descriptors validate that exact retained range against the
authoritative texture state tracker before either cache reuse or a native
descriptor write. A dual-use sampled/storage texture remains legal, but it
must explicitly transition between graphics shader read and graphics shader
read/write access; creation flags do not stand in for current state.

Render-pass descriptions carry attachment resources and their selected counted
views together. Recording canonicalizes missing attachment views, retains the
exact views through replay, and the framebuffer cache retains the same view
identity for its lifetime. A texture object does not own a backend-wide default
image view. Framebuffer-held views are released before the final RHI resource
and Vulkan allocation drains during shutdown.

## Portable Copy Contract

The public copy matrix consists of:

- `CopyBuffer` with `FRHIBufferCopyRegion`;
- `CopyBufferToTexture` and `CopyTextureToBuffer` with
  `FRHIBufferTextureCopyRegion`;
- `CopyTexture` with `FRHITextureCopyRegion`.

Regions name exact offsets, extents, aspects, mips, and layers. Buffer/texture
regions also publish row length and image height when storage is not tightly
packed. Shared footprint arithmetic accounts for compressed block geometry and
is used by both validation and Vulkan state checks.

Validation runs for the complete batch before a command is retained or a
backend call is made. It rejects null resources, missing source/destination
usage, empty or overflowing ranges, out-of-bounds offsets and subresources,
invalid compressed-block edges, incompatible format/sample/aspect pairs,
source/destination aliasing, and overlapping destination regions. An empty
batch is a no-op. Copies are rejected inside a render pass.

Regular and immediate command lists own copied region arrays and retain both
resources. Inline and threaded replay therefore observe the same ordering,
payload, and lifetime. Copy replay never infers a transition: every exact
source range must already be `TransferRead`, and every exact destination range
must already be `TransferWrite`, under the common transition authority.

## Vulkan Lowering and Convenience Paths

Vulkan revalidates each complete batch, checks the exact tracked states, and
maps portable regions directly to `vk::BufferCopy`, `vk::BufferImageCopy`, or
`vk::ImageCopy`. Rejection cannot mutate tracker state. The four context copy
methods are the only backend-private native copy-recording authority.

Static device-local buffer writes, `RHIUpdateTexture2D`, and
`RHIReadTexture2D` use bounded persistently mapped upload/readback arena ranges,
selected transitions, and the same public copy semantics. Upload restores the
resource's canonical graphics or storage access. Readback restores the exact
prior texture state, transitions its range for host read, waits the producing
submission token, invalidates noncoherent mapped memory, and publishes tightly
packed CPU bytes before returning the range. These paths do not introduce a
whole-device idle wait or a second layout tracker. Arena ownership, bounds, and
reuse are defined by [Vulkan memory and GPU completion](VulkanMemoryAndGPUCompletion.md).

Legacy static-buffer uploads, shader-resource or storage texture uploads, and
CPU-readback textures receive compatibility copy usage during Vulkan creation.
New callers should still declare `SourceCopy` and `DestinationCopy` explicitly
when transfer is part of their public resource contract.

## Boundaries and Follow-ups

Render-pass MSAA resolve remains owned by the render-pass contract. Standalone
resolve, scaled blit, queue-family transfer, asynchronous transfer scheduling,
and multi-queue ownership are outside this contract. Descriptor arrays consume
the counted views defined here. Completion-token-owned transfer arenas replace
per-operation temporary allocation without changing public copy semantics.

## Related Documentation

- [RHI resource transitions](RHIResourceTransitions.md)
- [RHI command execution](RHICommandExecution.md)
- [Shader parameters](ShaderParameters.md)
- [RHI capabilities and Vulkan startup](RHICapabilitiesAndVulkanStartup.md)
- [RHI and Vulkan backend evolution roadmap](../../Roadmaps/Archive/2026-08/RHIAndVulkanEvolution.md)
- [Vulkan memory and GPU completion](VulkanMemoryAndGPUCompletion.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanView.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanFramebuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
