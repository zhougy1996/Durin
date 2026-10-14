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

Structured and byte-address shader data use ordinary `FRHIBuffer` resources
created through `RHICreateBuffer` with
`FRHIBufferCreateDesc`. Usage flags select supported operations; buffer views
select an exact range and its structured, byte-address, uniform, or formatted
interpretation. Storage is a shader binding category, not a resource subclass
or separate resource identity. `FRHIStorageBufferRange` remains a shader
parameter range over an ordinary buffer.

`WriteBuffer` copies a patch into command-owned storage, or retains an immutable
owned payload without copying its bytes, and applies it during ordered replay.
The written range is left in `TransferWrite`; callers record an explicit
transition to the next use, or let RDG derive it. Writing does not restore a
canonical access state. Untouched GPU bytes are preserved; upload bytes live
through replay or cancellation. No full CPU shadow or replacement allocation
is maintained. Canceled commands release their payload without writing.
Initial data in a native creation descriptor uses the native initialization
path; callers cannot rely on discarding an initialization command list while
retaining its initialized contents.

Ordinary buffer bindings name the retained resource and view. Updating that
resource does not preserve earlier bound contents: visibility follows ordered
GPU writes, transitions, and submission dependencies. Callers requiring both
old and new contents create distinct buffers and retain them through GPU
completion. Native buffers participate in copies, transitions, queue transfers,
vertex/index binding, and RDG external imports according to their descriptors
and the applicable access contracts.

`LockBuffer(WriteOnly)` retains the ordinary patch semantics. A
`WriteOnlyDiscard` lock replaces contents and leaves bytes outside its range
undefined. Dynamic vertex/index buffers can opt into bounded native mapped
versions with `CPUWriteDiscard`; unsupported backends or exhausted pools use
the existing temporary-byte upload, moving those bytes into the command at
unlock. Neither lock path waits for GPU completion. Unlock records the commit;
ordered replay leaves the written range in `TransferWrite` in both paths.
Callers still transition explicitly and rebind after a discard write to select
the new physical version. Existing vertex/index bindings retain their version
through command-buffer restoration and GPU completion. Mapped versions cannot
be reacquired while CPU locks, recorded commands, active versions or bindings
retain them, or while any GPU use is awaiting retirement. Failed/device-lost
GPU uses never authorize normal reuse.

`UpdateUniformBuffer` still copies complete replacement bytes and resource
references into the command list. Uniform binding captures the current
immutable physical allocation during replay; subsequent updates do not rewrite
existing bindings. Rebind parameters, including a prepared batch, to select the
new uniform allocation. Uniform sidecars may retain ordinary GPU resources and
views, but uniform resources are rejected to prevent ownership cycles.

`FRHIUniformBufferRange` represents a typed uniform resource or a native buffer
range. `CreateUniformBufferRange` supplies counted ownership across preparation
and recording lists. Native buffer ranges are canonicalized into counted native
views through backend factories and caches. Range and offset validation uses
active-device limits. There is no logical Storage view creation path.
Vulkan uniform allocation ownership follows
[the memory contract](VulkanMemoryAndGPUCompletion.md#logical-uniform-allocations).

RDG sources, native buffer upload payloads, pending uniform data, and packed
texture command arrays allocate CPU storage on demand. Diagnostics track live
and peak owned bytes, aligned uniform backing ranges, and retained page
capacity. Copying a span owns its exact bytes; taking a vector tracks its
capacity. Shared graph and command owners count one source allocation until its
last owner releases it. Ordinary buffers retain no permanent CPU upload copy.

`CreateUniformBuffer` returns a counted resource; `UpdateUniformBuffer`,
and `WriteBuffer` return void. Invalid ranges, resource types,
or uniform sidecar ownership violate enforced preconditions. Native creation
uses its existing recoverable result contract. Allocation failures during
replay follow terminal replay failure handling. Executor backpressure remains
at the submission boundary; a single payload beyond the queue byte threshold
runs alone.

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
selected transitions, and the same public copy semantics. Buffer writes leave
the destination in `TransferWrite`; texture updates likewise leave the affected
mip/array slice in `TransferWrite`. Uploads perform the internal transitions
needed for copying but do not infer the next consumer access. Callers explicitly
transition the written range before sampling, compute or further copies.
CPU uploads use the shared checked footprint and packer in
`Backend/RHITextureUpload.h`, including source offsets, compressed blocks,
row/depth pitches, complete source bounds, and packed-size limits. Ordinary
Texture updates pack once when recorded; RDG packs at declaration and shares
that immutable payload with recording. Backends use the same layout/copy helper
for the required staging copy. Calls record through
`FRHICommandListBase::UpdateTexture2D/3D`; DynamicRHI has no upload forwarding
methods. Complete mip/face initialization and ImGui dirty-region batches
transition to the consumer state after the complete batch.
Readback restores the exact prior texture state, transitions its range for host read, waits the producing
submission token, invalidates noncoherent mapped memory, and publishes tightly
packed CPU bytes before returning the range. These paths do not introduce a
whole-device idle wait or a second layout tracker. Arena ownership, bounds, and
reuse are defined by [Vulkan memory and GPU completion](VulkanMemoryAndGPUCompletion.md).

RDG uses the same `WriteBuffer` owned source and staging path, leaving the
written destination range in `TransferWrite`. RDG supplies the
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
