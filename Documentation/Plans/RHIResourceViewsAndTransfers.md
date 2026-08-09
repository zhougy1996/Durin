# RHI Resource Views and Transfers Plan

Summary: Add counted buffer and texture views plus explicit recorded copy operations, then migrate current Vulkan bindings, uploads, and readbacks onto those portable contracts.

Last reviewed: 2026-08-10

Status: Active
Completed:

## Current Status

The M1 transition prerequisite completed at baseline `99c82b01` through
[GPU Resource Transitions](GPUResourceTransitions.md). Buffer byte ranges and
texture aspect/mip/layer ranges now have checked portable state, recorded
inline/threaded replay, and one Vulkan authority. M2 can consume those ranges
without defining another synchronization system.

A bounded audit selected existing consumers rather than speculative texture
types. `FRHIShaderParameterResource` currently carries raw resources plus
buffer offset/size, while `FVulkanPendingState` binds the one native image view
stored on `FVulkanTexture`. Framebuffers synthesize separate attachment image
views. `RHIWriteBuffer`, `RHIUpdateTexture2D`, and `RHIReadTexture2D` each own a
private Vulkan copy path instead of replaying typed RHI transfer commands.
These binding, staged-write, texture-upload, and texture-readback paths are the
initial view and transfer consumers.

The selected transfer matrix is buffer-to-buffer, buffer-to-texture,
texture-to-buffer, and exact texture-to-texture copies for supported 2D/cube
subresources. Existing render-pass resolve remains the owner of current MSAA
resolve behavior. No current consumer requires a standalone resolve or scaled
blit command, so those operations remain deferred rather than entering M2 as
untested API surface. Texture2DArray, Texture3D, and TextureCubeArray creation
also remain unsupported until an asset or renderer consumer selects their full
sampling and attachment contract.

## Goal

Make shader-visible and attachment-visible resource ranges first-class counted
RHI objects, and make every currently required GPU copy expressible as a typed,
validated, recorded command whose visibility is governed by the completed M1
transition contract.

## Scope

- Immutable buffer-view and texture-view descriptors with exact parent range,
  format, aspect, mip, layer, dimension, and usage validation.
- Counted view resources that retain their parent allocation through command
  replay and view lifetime.
- An explicit default-view policy for current sampled texture, storage image,
  storage buffer, uniform buffer, and attachment consumers.
- Vulkan buffer/image-view construction and destruction with complete-or-null
  publication and full immutable identity.
- Portable buffer-to-buffer, buffer-to-texture, texture-to-buffer, and exact
  texture-to-texture copy descriptors.
- Typed command-list recording, owned descriptor payloads, retained resources,
  replay-side state checks, and Vulkan command emission.
- Migration of current staged buffer writes, texture uploads, synchronous
  texture readback, shader resource bindings, and attachment view creation to
  the shared contracts.
- Focused validation for descriptor rejection, lifetime, replay parity,
  subresource isolation, compressed formats, current renderer behavior, and
  Vulkan failure recovery.

## Non-Goals

- Standalone MSAA resolve, scaled or filtered blit, mip generation, format
  conversion, or depth/stencil conversion without a selected consumer.
- Enabling Texture2DArray, Texture3D, or TextureCubeArray resource creation.
- Descriptor arrays, bindless handles, binding-set redesign, or descriptor
  cache policy; those remain in `RHIGraphicsStateAndBindings`.
- Compute PSOs or dispatch, indirect execution, ray tracing, or mesh shaders.
- Queue-family ownership transfer, asynchronous transfer queues, timeline
  scheduling, or multi-queue execution.
- Staging/readback arena reuse, memory-budget policy, GPU completion tokens, or
  native retirement redesign; those remain in
  `VulkanMemoryTransferAndRetirement`.
- Render-graph scheduling, transient resources, aliasing, or automatic barrier
  synthesis.
- Texture asset formats, streaming, residency, or editor import workflows.

## Design Decisions and Invariants

### View ownership and identity

- Add distinct `FRHIBufferView` and `FRHITextureView` counted resource types.
  Every view owns a `TRefCountPtr` to its parent buffer or texture; callers and
  recorded commands retain views, never backend handles.
- A view is immutable after successful creation. Its identity includes parent
  resource identity and every descriptor field. Native handle equality or
  address reuse is never cache identity.
- View factories validate the complete descriptor before native creation and
  publish either one complete counted view or null with one owned diagnostic.
  A failed candidate cannot enter a cache or mutate the parent.
- M2 does not add a global view cache. Resource owners keep the views they use;
  repeated creation is correct but observable in tests. A later cache must be
  bounded and keyed by full immutable identity.
- The parent does not own counted children, avoiding a parent/view reference
  cycle. A default-view helper constructs the canonical descriptor; the
  consumer owns the returned view beside the parent resource.

### Buffer views

- `FRHIBufferViewDesc` names a nonempty byte offset and size plus a view kind:
  uniform, structured storage, byte-address storage, or formatted texel data.
- Uniform, structured, and byte-address views validate alignment, stride, and
  creation usage. Existing dynamic uniform offsets remain submission data;
  they do not mutate immutable view identity.
- A formatted view carries an explicit `EPixelFormat`, validates element-size
  alignment and device support, and causes Vulkan buffer creation to include
  the required texel-buffer usage. Unsupported format/usage combinations fail
  before `vkCreateBufferView`.
- A buffer view never silently expands `Size == 0` to the rest of the buffer.
  The canonical default-view helper performs that lowering explicitly.

### Texture views

- `FRHITextureViewDesc` names view usage, dimension, format, aspects, first
  mip/count, and first layer/count. Counts are exact and nonzero.
- The initial format rule is exact parent-format identity. Reinterpretation,
  mutable-format images, and component swizzles remain deferred until a
  selected consumer defines compatibility and sampling semantics.
- View dimension must match the selected subresources: current support covers
  2D views and whole six-face cube views over supported 2D/cube parents. A cube
  face may be exposed as one 2D layer. Array and 3D view dimensions reject
  until their parent resource dimensions are supported.
- Sampled, storage, color-attachment, depth/stencil-attachment, and transfer
  view usage validates against parent creation flags, format aspects, sample
  count, and the exact M1 subresource range.
- Canonical default views cover the full valid parent range for one requested
  usage. Depth/stencil defaults preserve both supported aspects unless the
  consumer requests one explicitly.

### Binding and attachment migration

- Texture, storage-image, storage-buffer, and formatted-buffer shader
  parameters bind views. Samplers remain independent resources. Uniform-buffer
  bindings use an immutable range view plus the established dynamic offset
  when required.
- Command recording retains each bound view and therefore its parent.
  `FVulkanPendingState` consumes the native view carried by the Vulkan view
  object rather than reaching through a raw texture or rebuilding offset/range
  data.
- Current renderer resource owners create and retain canonical default shader
  views beside their textures and buffers. Texture references update the
  referenced allocation and its default-view candidate transactionally.
- Render-pass attachment descriptions select an exact texture view. Existing
  whole-resource call sites lower through a canonical attachment-view helper;
  framebuffer creation no longer invents an unrelated subresource range.
- Explicit view binding remains scalar in M2. Descriptor arrays and general
  binding-set ownership stay in M3.

### Transfer descriptors and state

- `FRHIBufferCopyRegion` names source/destination byte offsets and a nonzero
  size. Same-buffer copies require non-overlapping ranges.
- `FRHIBufferTextureCopyRegion` names one buffer byte offset and row/image
  layout plus one texture aspect, mip, layer range, texel offset, and extent.
  Validation uses checked block-compressed arithmetic and Vulkan-equivalent
  row/height constraints without exposing Vulkan structures.
- `FRHITextureCopyRegion` names exact source and destination aspects,
  subresources, offsets, and equal extents. Initial M2 requires identical
  formats and sample counts of one; format conversion and resolve reject.
- Copy commands are legal only outside a render pass. Recording validates
  structure, resource usage, bounds, overflow, format compatibility, and
  overlapping destinations before retaining resources or payloads.
- Explicit copies require source ranges in `TransferRead` and destination
  ranges in `TransferWrite` at replay. They do not insert hidden before/after
  transitions or guess a final shader state.
- Convenience writes/uploads/readbacks preserve their public behavior by
  recording the required M1 transitions and shared copy command. They restore
  the exact prior tracked state where the existing contract promises
  restoration. Synchronous readback alone owns a CPU completion wait.
- Buffer and texture creation flags state transfer-source and
  transfer-destination intent explicitly. Existing compatible flags lower to
  those intents during migration; unsupported copies reject before recording.

### Replay, failure, and threading

- Regular and immediate command lists record the same typed copy commands.
  Commands own descriptor arrays and counted source/destination resources.
- Inline and dedicated-thread replay must observe identical command ordering,
  validation, transitions, and retained lifetimes. Backend work remains on the
  RHI thread in threaded mode.
- A copy batch validates completely before the first native copy is recorded.
  Failed validation or injected native view creation cannot partially publish
  a view, update a tracker, or replace a current renderer resource.
- M2 adds no global idle wait. Existing synchronous readback waits only for the
  submission that produces requested CPU bytes; asynchronous copies return
  under normal command-list submission semantics.

## Current Foundations and Gaps

| Area | Foundation | M2 gap |
| --- | --- | --- |
| Public ranges | M1 validates buffer bytes and texture aspects/mips/layers. | No counted shader/attachment view owns one of those ranges. |
| Shader parameters | Bindings carry raw resources and optional buffer offset/size. | Texture bindings always use one backend default image view; view lifetime and identity are implicit. |
| Attachments | Render passes describe formats, load/store actions, and transition states. | Framebuffers synthesize native attachment views outside a public view contract. |
| Texture creation | 2D and cube resources create one whole native view. | Subresource, face, aspect, storage, and attachment views are not first-class. |
| Buffer creation | Structured/byte-address/storage usage maps to Vulkan storage buffers. | There is no counted range view or formatted texel-buffer contract. |
| Recording | Typed commands own payloads/resources and replay inline or threaded. | Copies exist only inside backend convenience operations. |
| Transfers | Staged buffer write, 2D/cube upload, readback, and render-pass resolve work. | Copy descriptors, portable validation, reusable replay, and exact usage admission are missing. |
| Synchronization | One Vulkan tracker owns explicit and implicit state. | Backend-private copies call the tracker directly instead of consuming public copy commands. |

## Implementation Stages

### Stage 0: Freeze selected view and copy contracts

- [ ] Record the selected shader, attachment, staged-write, texture-upload, and
  texture-readback consumers with exact current call sites and resource flags.
- [ ] Freeze buffer/texture view descriptor shapes, default-view lowering,
  counted parent ownership, identity, and failure behavior.
- [ ] Freeze the buffer/buffer-texture/texture copy descriptors, compressed
  layout arithmetic, overlap rules, and source/destination usage admission.
- [ ] Freeze the initial supported matrix for formats, aspects, dimensions,
  samples, view usages, and copy directions; list deterministic rejection
  diagnostics for every deferred combination.
- [ ] Confirm the file-level boundary with `RHIGraphicsStateAndBindings`,
  `VulkanMemoryTransferAndRetirement`, and the synchronous compute plan.

#### Acceptance Gate

- Every public type and supported combination has one selected consumer,
  validation owner, lifetime owner, and Vulkan lowering; resolve/blit,
  advanced texture dimensions, descriptor arrays, staging reuse, and
  completion policy remain explicitly outside M2.

### Stage 1: Add portable counted views and validation

- [ ] Add buffer/texture view resource types, descriptors, aliases, canonical
  default-view helpers, and complete backend-neutral validation.
- [ ] Publish complete-or-null view factories through `FDynamicRHI` with
  synchronous creation on the established RHI execution boundary.
- [ ] Retain parent resources from views without introducing parent/child
  cycles or raw-handle identity.
- [ ] Add unit coverage for bounds, overflow, alignment, format/aspect,
  dimension, samples, usage flags, default descriptors, and parent lifetime.

#### Acceptance Gate

- Valid descriptors create immutable counted candidates; invalid or
  unsupported descriptors fail before backend creation; destroying the last
  parent reference cannot invalidate a live view.

### Stage 2: Implement Vulkan views and migrate current consumers

- [ ] Add Vulkan buffer/image view objects with exact descriptor mapping,
  failure injection, deferred native destruction, and debug identity.
- [ ] Add transfer/texel usage admission to Vulkan resource creation only for
  the portable flags selected in Stage 0.
- [ ] Migrate sampled/storage texture and storage/uniform/formatted buffer
  shader parameters to counted views while preserving scalar binding behavior.
- [ ] Migrate attachment selection and framebuffer construction to exact
  texture views, including color, resolve, depth, stencil, cube face, mip, and
  layer validation used by current render passes.
- [ ] Migrate renderer resource publication and texture-reference replacement
  so parent and default views publish transactionally.

#### Acceptance Gate

- Current draws and render passes bind only complete view candidates; exact
  subresources reach Vulkan descriptors/framebuffers; injected failure leaves
  current resources usable; validation reports no leaked or prematurely
  destroyed native view.

### Stage 3: Record explicit copy commands

- [ ] Add public copy descriptors and deterministic backend-neutral validation
  for all selected directions and batches.
- [ ] Add `IRHICommandContext` and regular/immediate command-list APIs for
  buffer, buffer-to-texture, texture-to-buffer, and texture copies.
- [ ] Record typed commands with owned region arrays, exact payload accounting,
  and retained source/destination lifetimes.
- [ ] Reject copies inside render passes, overlapping or aliased invalid
  regions, undefined layouts, unsupported formats/samples, and missing usage
  before backend replay.
- [ ] Add fake-context tests for ordering, batching, lifetime, empty commands,
  regular/immediate lists, and inline/threaded replay parity.

#### Acceptance Gate

- Every selected copy can be recorded without Vulkan knowledge and replays
  identically inline or threaded; invalid commands cannot reach a backend or
  partially retain a batch.

### Stage 4: Implement Vulkan copies and migrate convenience paths

- [ ] Map each portable copy region to exact Vulkan copy structures with
  checked narrowing and no format or subresource inference.
- [ ] Require M1 `TransferRead`/`TransferWrite` state for exact source and
  destination ranges and preserve tracker state on rejected batches.
- [ ] Lower staged static buffer writes and `RHIUpdateTexture2D` through shared
  explicit copy commands and selected transitions.
- [ ] Lower `RHIReadTexture2D` through texture-to-buffer copy while preserving
  tight CPU output, exact prior-state restoration, and its scoped completion
  wait.
- [ ] Add hardware-backed tests for every copy direction, disjoint regions,
  compressed blocks, cube faces/mips, alias rejection, visibility to graphics
  and compute intent, and repeated upload/readback.

#### Acceptance Gate

- The selected transfer matrix produces exact bytes through public RHI
  commands, uses one transition authority, and adds no global idle wait or
  backend-private duplicate copy implementation.

### Stage 5: Qualify M2 and publish lasting contracts

- [ ] Run focused RHI, RenderCore, Renderer, and Vulkan view/transfer suites in
  inline and threaded modes where supported.
- [ ] Run existing texture sampling/compression, cube, MRT/MSAA/depth,
  failure-injection, viewport, resource replacement, and shutdown regressions.
- [ ] Run the selected native suite, normal Debug Editor full `all` build, and
  hidden-window runtime smoke through DurinDevTool.
- [ ] Publish lasting view and transfer behavior under
  `Documentation/Runtime/Rendering/` and update related rendering contracts.
- [ ] Update the RHI/Vulkan roadmap with M2 completion evidence and the stable
  M3/M4 handoff; do not activate either downstream plan here.

#### Acceptance Gate

- Public views and selected transfers are portable, lifetime-safe,
  validation-clean, replay-equivalent, and consumed by current runtime paths;
  lasting documentation owns shipped behavior and downstream milestones no
  longer depend on raw Vulkan views or private copy commands.

## Validation Matrix

| Layer | Required evidence |
| --- | --- |
| Descriptor/unit | View identity, parent lifetime, bounds, overflow, alignment, formats, aspects, dimensions, samples, copy layout arithmetic, and rejection order. |
| Recording | Typed command ordering, batch ownership, payload accounting, retained source/destination/view lifetime, invalid pass ordering, and inline/threaded parity. |
| Vulkan views | Exact image/buffer view creation, shader and attachment consumption, default-view lowering, failure injection, replacement, and deferred destruction. |
| Vulkan transfers | All selected copy directions, state preconditions, disjoint subresources, compressed rows, cube faces/mips, exact output bytes, and no partial tracker mutation. |
| Regression | Texture sampling/compression, storage, MRT/MSAA/depth, uploads/readbacks, resource refresh, viewport recreation, failure recovery, and shutdown. |
| Qualification | Selected native tests, full Debug Editor `all` build, validation-clean hidden-window runtime smoke, and clean shutdown. |

All build, test, and runtime commands follow
[Build and Run](../Development/Build/BuildAndRun.md) and
[Native Tests](../Development/Build/NativeTests.md). Each stage handoff records
the exact profile, commands, filters, and observed outcomes.

## Definition of Done

- Counted buffer and texture views own exact parent ranges and remain valid
  independently of caller-held parent references.
- Shader and attachment consumers use portable views rather than raw Vulkan
  image views or ad hoc buffer offset/range reconstruction.
- Buffer-to-buffer, buffer-to-texture, texture-to-buffer, and exact
  texture-to-texture copies record and replay through public RHI commands.
- Current staged writes, uploads, and readbacks reuse those commands and the M1
  transition authority.
- Invalid view/copy combinations fail at the RHI boundary with stable,
  resource-qualified diagnostics.
- Inline and threaded execution preserve ordering, lifetime, failure, and
  output equivalence.
- No new path depends on global device idle, hidden state inference, or native
  handle identity.
- Current graphics, texture, transfer, viewport, and shutdown behavior remains
  validation-clean.
- Lasting Runtime documentation replaces the plan as the shipped contract and
  the roadmap records the M2 handoff.

## Deferred Follow-ups

- Standalone resolve, scaled/filtered blit, mip generation, and format
  conversion after a concrete consumer is selected.
- Texture2DArray, Texture3D, and TextureCubeArray creation and sampling in
  their owning asset/renderer plans.
- Descriptor arrays, binding-set ownership, and bounded descriptor/view caches
  in `RHIGraphicsStateAndBindings`.
- Staging/readback reuse, placement policy, memory telemetry, GPU completion,
  and native retirement in `VulkanMemoryTransferAndRetirement`.
- Queue ownership and asynchronous transfer in a measured multi-queue plan.
- Render-graph copy scheduling, transient aliasing, and synthesized barriers.

## Related Documentation

- [RHI and Vulkan Backend Evolution Roadmap](../Roadmaps/RHIAndVulkanEvolution.md)
- [RHI Resource Transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Capabilities and Vulkan Startup](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHIShaderParameters.h`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanFramebuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Tests/Native/RHITests/`
- `Engine/Tests/Native/RenderCoreTests/`
- `Engine/Tests/Native/EngineTests/`
- `Engine/Tests/Native/VulkanRHITests/`
