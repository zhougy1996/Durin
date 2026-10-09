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

## Uniform and Storage Buffers

`FRHIUniformBuffer` derives directly from `FRHIResource`, independently of
`FRHIBuffer`. It owns an immutable constant-size layout and a lifetime usage
hint. Vulkan and Metal subclasses own the current native allocation. Initial
bytes and native-resource references are copied into pending initialization
storage; the creator list may be discarded before another list consumes the
resource. First replay binding materializes the initial allocation and releases
the pending CPU bytes. Uniform layout size is a nonzero multiple of 16.

`CreateStorageBuffer` returns ordinary `FRHIBuffer` ownership. Its internal
`Backend/RHIStorageBuffer.h` base carries a logical resource tag and CPU update
state; application-facing headers expose no separate Storage resource class.
The resource retains one mutable CPU copy for partial
updates; backend subclasses directly own their current immutable GPU allocation.
There is no Storage snapshot type, version counter, or update-policy enum.
Storage selects structured (nonzero stride dividing size) or byte-address
(stride four, size multiple of four), optionally with `ShaderResource`.
Other storage flags are rejected. Native GPU-written storage continues to use
ordinary buffers and Storage views. Lifetime usage hints never authorize
frame-age reuse or limit lifetime.

`UpdateUniformBuffer` copies complete replacement bytes and resource references
into the command list. `UpdateBuffer` accepts only resources created by
`CreateStorageBuffer` and copies
the modified byte range into the command list. Ordered RHI replay applies the
patch to the resource's CPU copy and replaces its backend allocation with the
complete contents. Untouched bytes follow submission order, not recording order.
Canceled updates release their payload without modifying the resource.
Storage accepts no sidecars. Uniform resources, Storage resources, and their
logical views are rejected as sidecars to prevent ownership cycles.

`FRHIUniformBufferRange` represents a typed uniform resource or a native buffer
range. `CreateUniformBufferRange` supplies counted ownership across preparation
and recording lists. Uniform parameter objects remain direct resources in
prepared batches; recording validates their ranges without creating a logical
buffer view. Native ranges and Storage are canonicalized into
counted views. Executing a parameter bind captures the current physical
allocation; draw/dispatch reuses that binding. Updates do not rewrite existing
bindings: execute another parameter bind, including reusing a prepared batch,
to select updated contents. Range and offset checks use active-device limits.
Vulkan allocation and submission ownership follow
[the memory contract](VulkanMemoryAndGPUCompletion.md#logical-buffer-versions).

`FRHIBufferView::Create` creates logical Storage views and enforces parent,
description, and active-device range/offset preconditions. `CanCreate` checks
those conditions without allocation or diagnostics for parameter-batch soft
admission. CPU allocation failures propagate normally; there is no separate
upload-error translation. Logical view creation is independent of command-list
recording and has no command-list forwarding API. Native view factories and
their cache paths reject Storage parents before backend work. Binding canonicalization selects the
correct path from the parent's resource type. Native write/upload, lock, vertex/index,
copy, and transition operations reject Storage buffers with enforced
preconditions; ordered data updates use the RHI APIs above. RDG external
buffer imports reject Storage parents as declaration errors because
their backing changes independently of graph access tracking. Graph-owned
upload helpers continue to allocate native graph resources.

Storage CPU contents, RDG sources, native buffer write/upload payloads,
and packed texture command arrays allocate CPU storage on demand. There is no
fixed per-upload size or process-wide CPU payload budget. Diagnostics track
live and peak owned bytes, plus aligned backing ranges and retained page
capacity. Copying a span owns its exact bytes; taking a vector tracks its
capacity. Shared graph and command owners count one source allocation until
its last owner releases it. Storage retains one full CPU copy; update commands own only their patch bytes.
Uniform commands own their upload bytes until replay or cancellation; native
allocations retain their resource references for the lifetime of captured bindings.

`CreateUniformBuffer` and `CreateStorageBuffer` return counted resources;
`UpdateUniformBuffer`, `UpdateBuffer`, `WriteBuffer`, and `UploadBuffer` return
void. Invalid descriptors, ranges, resource types, or sidecar ownership violate
enforced preconditions. Allocation failures follow ordinary allocation or
terminal replay failure handling rather than a caller-managed budget result.
Texture commands likewise own their exact packed bytes before recording.
Executor queue and frame backpressure remains at the submission boundary;
a single payload beyond the queue byte threshold runs alone.

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

The graph-specific `UploadBuffer` uses the same owned source and staging path,
but leaves the written destination range in `TransferWrite`. RDG supplies the
next transition from its declared use. Mapped destinations transition from
`HostWrite`; device-local destinations copy from a retired staging range.

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
