# RHI Resource Views and Transfers

Summary: Define explicit resource views, shader ranges and attachment
lowering, validated recorded copies and asynchronous GPU readback shared by RHI backends.

Modules: RHI, RenderCore, VulkanRHI, MetalRHI

GPUScene is a client of ordinary structured `FRHIBuffer` storage and counted
ranges. Dirty updates use ordered `WriteBuffer` patches; culling declares scene
reads, ID writes and indirect reads through RDG. No GPUScene-specific RHI resource
class or rendering readback is required. The scene-level policy is specified in
[GPUScene](../Scene/GPUScene.md).

## Counted View Contract

`FRHIBufferView` and `FRHITextureView` are immutable counted RHI resources.
Each explicit view retains its parent and identifies a fixed subresource range.

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

Vulkan allocates a native buffer view for formatted buffers. Native views are
retired through deferred deletion.

## Experimental Transient Aliasing

`Experimental/RHITransientAliasing.h` owns the opt-in allocation policy and
memory-handoff value. `FRHITextureCreateDesc::Aliasing` and
`FRHIBufferCreateDesc::Aliasing` default to disabled. The related DynamicRHI
support query and aliased factories, and command-list/context aliasing barriers,
remain experimental while multi-queue qualification is open.

A supported aliased factory creates a distinct compatible native image/buffer
bound to its source's allocation and retains the backing owner. It returns null
on creation failure. Vulkan uses alias-enabled VMA allocations, alias image flags
and separate native handles; each alias has its own resource-state tracker.
Mapped/dynamic buffers are excluded. Metal and unimplemented backends report
unsupported and retain independent allocations.

`AliasingBarrier` retains the before/after resources and records a memory barrier
outside render passes. Null `Before` acquires previously ordered pooled backing;
otherwise the resources must be distinct. Queue waits must already prove that
all previous users precede acquisition. The barrier does not transfer native
resource ownership or establish queue ordering itself. Queue waits, ownership
acquires, memory barriers and initial discard/layout transitions together order
the new resource before its first recorded use; resource layouts are not inherited
from another alias. Replay storage and alias owners keep all
native handles and the shared allocation alive through GPU completion; only the
root frees memory, after all aliases retire.

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

Ordinary buffer bindings retain the buffer and select a byte range. Updating that
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

Vulkan mapped versions are logical slices of shared device pages. The logical
buffer capacity and stride remain unchanged; binding and synchronization add
the captured physical offset. Whole logical transitions intersect the populated
prefix when emitted natively, avoiding neighboring slices. Normal patches that
extend this prefix preserve its bytes and access states by materializing the
original backing, including refreshing existing bindings. See the
[dynamic discard memory contract](VulkanMemoryAndGPUCompletion.md#dynamic-vertex-and-index-discard-writes).

`UpdateUniformBuffer` still copies complete replacement bytes and resource
references into the command list. Uniform binding captures the current
immutable physical allocation during replay; subsequent updates do not rewrite
existing bindings. Rebind parameters, including a prepared batch, to select the
new uniform allocation.

`FRHIUniformBufferLayout::Resources` describes typed resource-table members.
`GetResourceTable` exposes the logical Uniform's counted current references.
Updates publish current table values on the recording thread while copying an
independent ordered replay payload. A shader parameter with a `ResourceIndex`
identifies a member of its logical Uniform; canonicalization resolves that member
before recording the ordinary RHI parameter command. Direct resources use
`INDEX_NONE_U32`. The index describes layout membership, not binding identity.

Material texture members can name a concrete fallback member. Recording resolves
TextureReference targets, including null-to-fallback, before retaining the selected texture. Changing a target never rewrites previously recorded commands.
Typed Uniform members can reference other Uniforms, such as Scene Collections;
updates reject ownership cycles by checking the actual resource references, without
maintaining a dependency registry. Untyped legacy sidecar references cannot contain
Uniforms. These are actual Uniform resource owners, not prepared binding packets.

`FRHIUniformBufferRange` represents a typed uniform resource or a native buffer
range. `CreateUniformBufferRange` supplies counted ownership across preparation
and recording lists. Native buffer bindings carry the backing buffer, byte offset
and size. A logical uniform selects its physical allocation during bind replay.

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
or cyclic Uniform resource ownership violate enforced preconditions. Native creation
uses the [required-success contract](RHICommandExecution.md#required-success-resource-creation).
Native allocation failures and OOM terminate, including deferred allocations
during consuming replay. Queue diagnostics count the owned upload bytes through replay
and capture retirement.

## Binding and Attachment Ownership

Recording retains each bound buffer, texture, sampler or explicit view through
replay. Ordinary buffer descriptors use the backing buffer and byte range.
Dynamic uniform offsets are applied at bind time and excluded from descriptor
identity. An ordinary texture uses its default shader view. An explicit texture
view selects its own mip, layer and aspect range. The selected range must be in
the access state required by the shader binding.

A bare render-pass attachment selects mip zero and layer zero. RDG resolves
explicit attachment ranges through the texture backing's view cache. A pooled
texture keeps that cache across executions. Vulkan selects the acquired
swapchain image's attachment view on replay and matches framebuffers by their
native attachments.

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

### Asynchronous GPU Buffer Readback

`FRHIGPUBufferReadback` is a reusable EnqueueCopy/IsReady/Lock/Unlock facade;
it may be stack-owned. Commands/backends retain shared internal storage, not the
facade. One object supports one in-flight copy: reuse requires native completion
or explicit abandonment before execution. Concurrent results use separate objects,
without revision tokens or replacing an active publication. Backend staging remains
independently reusable. Texture readback preserves TakePixels and polling APIs;
PollTextureReadbacks forwards to PollReadbacks. Completion/failure publication is
backend-only; public facades never expose Complete/Fail or internal storage.

EnqueueCopy records without waiting, outside raster passes into a GPU recording.
The source requires SourceCopy usage and TransferRead access, which is preserved.
Zero NumBytes selects the remainder from Offset. Ranges must be nonempty, in bounds
and four-byte aligned; invalid/unsupported copies publish Failed. IsReady and polling
are nonblocking; GetState reports publication status. Cancel discards publication,
while IsInFlight stays true until the backend retires or abandons the operation.
Cancellation alone never authorizes reuse; ResetReady also requires no in-flight copy.

Lock borrows ready bytes; Unlock releases the borrow without consuming the result,
so it may be locked again until the next copy/reset/cancel. After completion, a new
copy may be recorded while the old result is locked: old borrowed bytes survive
until their Unlock, which never clears the new result. Host-memory invalidation
precedes publication. Destroying a facade cancels observation while recorded/native
owners retain storage and GPU leases. Abandoned commands cancel and retire their
unsubmitted copy. Buffer reads expose only Lock/Unlock; the base facade has no
public consuming read. Texture-only TakePixels transfers the ready byte storage
once; pending, canceled, failed or already-consumed reads leave the output unchanged.
IsReady reports copy completion and remains true after successful consumption;
GetDataSize becomes zero. A subsequent enqueue starts a new result.

Vulkan shares its texture/buffer queue and arena, bounded to eight pending requests
and 64 MiB per context. Metal permits eight requests / 64 MiB in flight; completed
or unsubmitted-canceled buffers may enter an eight-buffer / 16 MiB cache. Failed
submitted copies are not cached, and pool return occurs outside the cancellation
lock. Capacity pressure rejects without a GPU wait; neither path waits for device idle.

## Boundaries and Follow-ups

Render-pass MSAA resolve remains owned by the render-pass contract. Standalone
resolve, scaled blit, queue-family transfer, asynchronous transfer scheduling,
and multi-queue ownership are outside this contract. Descriptor arrays consume
the counted views defined here. Completion-token-owned transfer arenas replace
per-operation temporary allocation without changing public copy semantics.

## Related Documentation

- [RHI resource transitions](RHIResourceTransitions.md)
- [RHI command execution](RHICommandExecution.md)
- [Shader parameters](../Shaders/ShaderParameters.md)
- [RHI capabilities and Vulkan startup](RHICapabilitiesAndVulkanStartup.md)
- [RHI and Vulkan backend evolution roadmap](../../../Roadmaps/Archive/2026-08/RHIAndVulkanEvolution.md)
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
