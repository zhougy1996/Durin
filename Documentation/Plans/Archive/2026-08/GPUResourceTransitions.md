# GPU Resource Transitions Plan

Summary: Establish one backend-neutral buffer and texture transition contract and one authoritative Vulkan resource-state tracker shared by graphics, transfer, readback, presentation, and future compute work.

Last reviewed: 2026-08-10

Status: Archived
Completed: 2026-08-10

## Current Status

The plan completed Stage 5 on the Stage 4 baseline `4bea0d07`. The public RHI
now records exact buffer and texture transitions with retained resource
lifetimes and inline/threaded replay parity. Vulkan maps the portable access
contract through synchronization2 or its legacy fallback and keeps one range
authority across explicit transitions, render passes, buffer writes, texture
upload/readback, and presentation. The lasting contract is published in
[RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md).

Qualification used Agent Build Profile `windows-msvc-x64`, preset
`Win64-Debug-DurinEditor-Tests`. Focused RHI transition validation passed 6/6,
RHI command-list recording and replay passed 47/47, RenderCore contracts passed
39/39, and Vulkan integration passed 24/24. The selected `test --target all`
native aggregate, normal Debug Editor `all` build, and hidden-window Sandbox
run for 10 ticks all completed successfully with clean shutdown.

The audit found three state authorities that must be reconciled rather than
extended independently:

- render-pass descriptions carry initial/final access and layouts while
  `FVulkanCommandListContext` commits only final image layouts;
- `FVulkanTexture` tracks one Vulkan layout per mip/layer but not the access or
  pipeline stages that produced it;
- texture upload and readback record their own legacy barriers, infer previous
  access from layout/creation flags, and hard-code graphics stages.

Stage 1 may now change public RHI headers without reopening descriptor shape.
This plan is the shared M1 child of both the Compute Shader Pipeline and
RHI/Vulkan roadmaps; it is not a compute-only synchronization path.

This work can proceed in parallel with
[Skeletal Runtime Pose and Playback](SkeletalRuntimePoseAndPlayback.md). The two
plans share no runtime source files and have no dependency edge. They must use
separate worktrees and build roots. Skeleton rendering S3 is the first phase
that may enter RHI-facing files; before it starts, its entry audit must either
wait for active M1 overlap to finish or record a genuinely disjoint working set.

## Goal

Make every supported buffer and texture handoff expressible through portable
RHI state and range descriptors, recordable through ordinary command lists,
and executable through Vulkan without implicit whole-device waits, divergent
layout trackers, or graphics-only stage assumptions.

## Scope

- A backend-neutral resource-access vocabulary covering current graphics,
  transfer, readback, presentation, and future compute consumers.
- Buffer byte ranges and texture aspect/mip/layer ranges with checked,
  deterministic normalization.
- Recorded transition commands that retain resources and owned descriptor
  payloads through deferred replay.
- One Vulkan replay-side state authority for buffer ranges and texture
  subresources.
- Vulkan synchronization2 mapping when the published capability is enabled and
  a behaviorally equivalent legacy-barrier fallback otherwise.
- Reconciliation of explicit transitions with render-pass attachment
  transitions, uploads, readbacks, initialization, and presentation.
- Focused validation of recording, replay, lifetime, mapping, state mismatch,
  existing rendering, transfer, readback, and WSI behavior.

## Non-Goals

- Compute PSO creation, compute dispatch, or renderer compute integration.
- Queue-family ownership transfer, asynchronous compute, asynchronous transfer,
  or multi-queue scheduling.
- Resource views, new copy/blit/resolve commands, or a general transfer API;
  those belong to `RHIResourceViewsAndTransfers`.
- A render graph, automatic frame scheduling, transient aliasing, or implicit
  barrier synthesis across independently recorded command lists.
- GPU completion tokens, staging-pool reuse, memory-budget telemetry, or native
  retirement policy.
- Replacing legacy Vulkan render passes with dynamic rendering.
- Changing texture asset build, compression, DDC, or residency policy.

## Design Decisions and Invariants

### Portable state and range contract

- Public state describes intended RHI usage. Vulkan pipeline stages, access
  masks, layouts, dependency flags, and native handles remain backend-private.
- The transition vocabulary distinguishes attachment, shader, vertex/index,
  uniform, transfer, host/readback, and presentation intent. Shader intent also
  distinguishes graphics from compute consumption; `ShaderRead` must not map
  unconditionally to all graphics stages.
- Read-only intents may be combined only where the Stage 0 mapping table proves
  a deterministic portable meaning. Write intents are exclusive except for a
  deliberately named read/write state. Invalid combinations fail RHI
  validation before a backend command is recorded.
- A buffer transition names a byte offset and size. A texture transition names
  aspect, first mip/count, and first layer/count. Whole-resource helpers lower
  to the same checked descriptors and do not bypass validation.
- Ranges are half-open after normalization. Zero-length, overflowed, out-of-
  bounds, unsupported-aspect, and empty subresource ranges are rejected.
- Every transition declares expected-before and required-after state. The
  expected state is validated against the replay-side authority; it is not a
  second mutable tracker. A deliberate discard/undefined source is legal only
  where prior contents are not required.

### Recording, replay, and ownership

- `FRHICommandListBase` records transitions like other typed commands. Recorded
  commands own descriptor arrays and retain counted resource references until
  replay or destruction.
- Structural validation occurs while recording. Ordering-dependent state
  validation and native emission occur on executor replay so inline and
  dedicated-thread modes observe the same admitted command order.
- Explicit transitions are invalid inside an active legacy render pass. Pass
  attachment entry/exit remains the only state mutation allowed within that
  boundary until a later rendering plan selects another contract.
- Transition failure is a state-contract violation and follows the existing
  terminal assertion/diagnostic policy. It does not partially advance tracked
  state or silently repair an incorrect expected-before state.
- Empty batches are no-ops; valid adjacent transitions may be coalesced only
  when coalescing preserves diagnostics and exact range/state results.

### Authoritative Vulkan state

- Vulkan owns one replay-side state record containing portable access plus the
  native stage/access/layout mapping needed for the next handoff. Texture state
  is tracked per aspect/mip/layer. Buffer range splitting and merging must be
  deterministic and bounded by the number of distinct recorded ranges.
- Native state advances only after barrier recording succeeds. Render-pass
  entry validates every attachment's declared initial state; render-pass exit
  commits the declared final state through the same authority.
- Upload, initialization, and readback use the common transition mapper. They
  may retain their current public convenience APIs, but they do not keep
  private layout inference or hard-coded `eAllGraphics` restoration rules.
- Swapchain acquire and presentation participate in the same image-state
  authority. This plan does not add cross-family ownership transfer; startup's
  currently selected same-family execution contract remains required.

### Synchronization2 and fallback

- When `FRHICapabilities::bSupportsSynchronization2` is true, Vulkan emits
  synchronization2 dependency structures. Otherwise it emits legacy pipeline
  barriers from the same normalized state mapping.
- Capability selection is immutable after startup. No transition call performs
  extension probing or exposes a second synchronization capability surface.
- Pure mapping tests prove that both emission paths represent the same source
  and destination intent. The fallback may be more conservative but may not
  omit required visibility or change layout results.

### Parallel-work boundary

- The expected implementation working set is RHI, VulkanRHI, RHITests,
  RenderCoreTests, and VulkanRHITests. It excludes Engine animation, skeletal
  assets, actor components, and skeletal tests.
- The concurrent skeletal S2 plan excludes RHI, VulkanRHI, Renderer, shaders,
  scene proxies, and render passes. Neither plan may widen into the other's
  working set without first recording a dependency and pausing that overlap.
- This coordination rule does not add an unconditional RHI M1 dependency to
  skeletal S3. The S3 entry audit decides from the then-current RHI binding and
  active-plan working sets whether it can remain disjoint or must wait.
- Texture Support Stage 5 may continue on source decoding and asset readiness.
  Any concurrent edit to `VulkanTexture.cpp`, low-level upload/readback state,
  or RHI access enums must be rebased onto this plan rather than introducing a
  fourth state path.
- Each implementation uses its own checkout and build directory. Full builds
  are qualification gates, not permission to run two writers against one build
  tree.

## Current Foundations and Gaps

| Area | Foundation | Gap closed by this plan |
| --- | --- | --- |
| Command transport | Typed owned command storage, counted resource retention, inline/threaded replay, submission serials, and shutdown drain are complete. | No transition command or descriptor lifetime coverage exists. |
| Capabilities | Startup publishes immutable synchronization2 support after device feature/extension negotiation. | Barrier emission does not consume the capability. |
| Public state | Attachment-oriented `ERHIAccess` and `ERHITextureLayout` describe current pass boundaries. | Buffer, copy, host, compute, ranges, and general handoffs are not expressible. |
| Texture state | Vulkan stores a layout for each mip/layer. | Access/stage/aspect state is absent and the layout is inferred independently by several paths. |
| Render passes | Initial/final attachment layouts and access feed legacy render-pass dependencies. | The pass boundary does not validate or update one general tracker. |
| Transfers | Texture upload/readback have focused barriers and hardware tests. | They infer state from layout/flags and restore through graphics-only stage masks. |
| Buffers | Buffer creation, binding, host writes, storage usage, and raw compute tests exist. | No portable byte-range transition or replay-side buffer state exists. |
| Presentation | Startup selects a valid graphics/present topology and swapchain replacement is transactional. | Acquire/present state is not reconciled with a general image authority. |

## Stage 0 Contract and Handoff

### Audited mutation paths

| Path | Current authority and mutation | Stage 4 reconciliation |
| --- | --- | --- |
| Resource creation | Vulkan buffers have no state record; regular and external images begin with layout-only `Undefined` entries. Storage texture initialization immediately emits a legacy whole-image barrier to `General`. | Create regular resources as `None`; keep swapchain-image state per native image; route storage initialization through the common mapper. |
| Render-pass entry/exit | `FRHIAttachmentLayout` owns initial/final access and layout. Vulkan render-pass dependencies consume them, while context replay commits only mip 0/layer 0 final layouts after the pass. | Before beginning the native pass, validate every covered aspect/subresource against the declared initial state. Let the legacy render pass own its automatic barriers, then atomically commit declared final portable/native states after successful pass end. |
| Texture upload | `UpdateTexture2D` reads the layout-only tracker, infers access from layout and creation flags, emits two legacy barriers, then restores either graphics `ShaderReadOnly` or `General` with `eAllGraphics`. | Transition the exact mip/layer to `TransferWrite`, copy, then transition to the selected canonical post-upload state through the common mapper. Preserve all other subresources. |
| Texture readback | `ReadTexture2D` infers access/stage from layout, transitions to `TransferSrc`, restores the inferred layout, finalizes, and waits for the utility queue. | Snapshot the exact tracked state, transition to `TransferRead`, copy, and restore that exact state. The existing synchronous completion wait remains readback policy rather than transition policy. |
| Buffer upload and host write | Device-local `FVulkanBuffer::Write` copies then exposes the written range through `eAllCommands`; mapped writes flush memory and record no access state. Dynamic uniform allocation is another mapped host write. | Commit `HostWrite` for mapped ranges and `TransferWrite` for staged ranges, then use the common mapping to reach the canonical usage-derived state. |
| Swapchain acquire/present | Acquire selects a native image and semaphore but does not update the wrapper's layout-only state. Presentation assumes the render pass produced a presentable image. | Track each swapchain image independently. New images start `None`; acquire validates availability without changing access; render-pass exit or an explicit transition produces `Present`; present requires and retains `Present`. |

Bindings and draws consume states but do not mutate them. Command-buffer
submission, fences, queue waits, and resource retirement are ordering/lifetime
mechanisms and are not additional resource-state authorities.

### Public state vocabulary

`ERHIAccess` becomes a `uint32` flag enum with these canonical values. Names in
the read-only group may be combined only when the resource-kind rules below
admit the combination. Every write-capable value and `Present` is exclusive.

| Value | Kind | Meaning |
| --- | --- | --- |
| `None` | sentinel | Known uninitialized/no-access state used for newly created resources; never a valid required-after state. |
| `Discard` | source-only sentinel | Deliberately ignore tracked-before state and previous contents. It is legal only as expected-before, maps an image's old layout to `Undefined`, and never becomes tracked state. |
| `VertexBufferRead` | read | Graphics vertex-input fetch. |
| `IndexBufferRead` | read | Graphics index-input fetch. |
| `GraphicsUniformRead` | read | Uniform access from supported graphics shader stages. |
| `ComputeUniformRead` | read | Uniform access from compute shaders. |
| `GraphicsShaderRead` | read | Sampled/storage read from supported graphics shader stages. |
| `ComputeShaderRead` | read | Sampled/storage read from compute shaders. |
| `TransferRead` | read | Transfer source access. |
| `HostRead` | read | Host read after device visibility is established. |
| `ColorAttachmentReadWrite` | exclusive read/write | Color attachment access, including blending and resolves. |
| `DepthStencilReadWrite` | exclusive read/write | Combined depth/stencil attachment access under the current legacy-pass contract. |
| `GraphicsShaderReadWrite` | exclusive read/write | Storage access from supported graphics shader stages. |
| `ComputeShaderReadWrite` | exclusive read/write | Storage access from compute shaders. |
| `TransferWrite` | exclusive write | Transfer destination access. |
| `HostWrite` | exclusive write | Host write followed by flush/coherency handling. |
| `Present` | exclusive terminal consumer | Presentation-engine ownership-free consumption on the selected same-family topology. |

For buffers, any non-empty combination of the eight read values is legal when
the buffer usage flags admit every member. For textures, only
`GraphicsShaderRead | ComputeShaderRead` may combine; vertex, index, uniform,
and `Present` combinations are invalid. Attachment states require matching
targetable usage and aspects, shader write states require storage usage,
transfer states require the backend creation usage already selected for upload
or readback, and host states require host-visible/readback-capable storage.
`Discard` is not a bit that can be ORed with another value.

The existing `ERHITextureLayout` remains in render-pass descriptions for legacy
render-pass compatibility. Stage 1 adds `TransferSource` and
`TransferDestination`; transition descriptors do not carry a separate layout.
Access uniquely selects the layout below, and attachment validation rejects an
access/layout pair that disagrees. The bounded source migration renames current
`ColorAttachmentWrite`, `ShaderRead`, and `ShaderReadWrite` uses to
`ColorAttachmentReadWrite`, `GraphicsShaderRead`, and
`GraphicsShaderReadWrite` respectively; `DepthStencilReadWrite`, `Present`, and
`None` retain their names.

### Descriptor and normalization contract

- `FRHIBufferTransition` contains `FRHIBuffer* Buffer`, `uint64 Offset`,
  `uint64 Size`, `ERHIAccess ExpectedBefore`, and `ERHIAccess RequiredAfter`.
- `ERHITextureAspect` is a flag enum containing `Color`, `Depth`, and
  `Stencil`. `FRHITextureSubresourceRange` contains exact aspect flags plus
  `uint32 FirstMip`, `NumMips`, `FirstArrayLayer`, and `NumArrayLayers`.
  `FRHITextureTransition` contains the texture pointer, that range,
  expected-before, and required-after.
- Counts and sizes are always explicit. Zero never means "all remaining", and
  no max-value sentinel is accepted. Whole-resource helpers inspect the live
  resource and produce the same exact public descriptors before validation.
- Normalization uses checked subtraction (`Offset <= resource size` followed by
  `Size <= resource size - Offset`, and equivalent mip/layer checks) so addition
  overflow cannot be admitted. Zero-sized, empty, null, unsupported-aspect, and
  out-of-bounds descriptors fail before command recording.
- A color format admits only `Color`; a depth-only format admits only `Depth`;
  a depth/stencil format admits `Depth`, `Stencil`, or both. A descriptor may
  cover multiple admitted aspects, but the Vulkan authority expands it to
  aspect/mip/layer atomic units.
- Descriptors for the same resource may be disjoint or exactly adjacent within
  a batch. Any overlap within one batch is rejected before recording, even when
  states match, so diagnostics and all-or-nothing replay remain deterministic.
  Empty batches are no-ops.
- Replay first validates every affected atomic unit or buffer interval. A
  concrete expected-before value must exactly equal all tracked state in its
  range. `Discard` deliberately bypasses equality for that range and invalidates
  prior contents. A mismatch emits resource identity, normalized range,
  expected, tracked, and requested states; no barrier or tracker update from the
  batch occurs.
- Buffer interval updates split at range boundaries and merge adjacent equal
  states after a successful batch. Image updates are per aspect/mip/layer.
  State advances only after all native barrier recording for the batch succeeds.

### Creation and convenience-path state policy

- Every ordinary buffer and texture begins as `None` over its full range.
  Contents remain undefined until a write, clear, render pass, or explicit
  discard transition establishes them.
- A mapped buffer write establishes `HostWrite` for the exact flushed range; a
  staged buffer write establishes `TransferWrite`. The convenience path then
  transitions that range to the canonical usage-derived state: the union of
  admitted vertex/index/graphics-uniform/graphics-shader reads for read-only
  usages, or exclusive `GraphicsShaderReadWrite` for unordered-access storage.
- Texture initialization for storage usage transitions the whole resource from
  `Discard` to `GraphicsShaderReadWrite`. Upload transitions only the addressed
  mip/layer through `TransferWrite`, then to `GraphicsShaderRead` for sampled
  textures or `GraphicsShaderReadWrite` for storage textures. Readback restores
  the exact pre-readback state rather than a creation-flag inference.
- New swapchain images begin `None`. Successful presentation leaves the native
  image tracked as `Present`; reacquire preserves that state. Swapchain
  replacement creates a fresh per-image state set and publishes it only with
  the rest of the transactional replacement.

### Pure Vulkan mapping

The mapper unions stages and access for admitted read combinations. "Graphics
shader" means vertex, fragment, and geometry stages currently published by the
RHI; it never includes compute. The synchronization2 columns are authoritative.
Legacy emission uses the listed conservative stages and the same access/layout;
unsupported synchronization2 bits are narrowed only after equivalence tests
prove no visibility is lost.

| RHI state | synchronization2 stage / access | Legacy stage / access | Texture layout |
| --- | --- | --- | --- |
| `None` / `Discard` source | `None` / `None` | `TopOfPipe` / none | `Undefined` |
| `VertexBufferRead` | `VertexInput` / `VertexAttributeRead` | same | invalid |
| `IndexBufferRead` | `VertexInput` / `IndexRead` | same | invalid |
| `GraphicsUniformRead` | graphics shader stages / `UniformRead` | same | invalid |
| `ComputeUniformRead` | `ComputeShader` / `UniformRead` | same | invalid |
| `GraphicsShaderRead` | graphics shader stages / `ShaderRead` | same | `ShaderReadOnlyOptimal` |
| `ComputeShaderRead` | `ComputeShader` / `ShaderRead` | same | `ShaderReadOnlyOptimal` |
| `TransferRead` | `Transfer` / `TransferRead` | same | `TransferSrcOptimal` |
| `HostRead` | `Host` / `HostRead` | same | `General` |
| `ColorAttachmentReadWrite` | `ColorAttachmentOutput` / `ColorAttachmentRead \| ColorAttachmentWrite` | same | `ColorAttachmentOptimal` |
| `DepthStencilReadWrite` | early + late fragment tests / depth-stencil read + write | same | `DepthStencilAttachmentOptimal` |
| `GraphicsShaderReadWrite` | graphics shader stages / shader read + write | same | `General` |
| `ComputeShaderReadWrite` | `ComputeShader` / shader read + write | same | `General` |
| `TransferWrite` | `Transfer` / `TransferWrite` | same | `TransferDstOptimal` |
| `HostWrite` | `Host` / `HostWrite` | same | `General` |
| `Present` | `None` / `None` | `BottomOfPipe` / none | `PresentSrcKHR` |

Image and buffer barrier mappers share this access/stage table. Image mapping
adds the derived layout and normalized aspect/subresource range; buffer mapping
adds byte offset/size. Both set queue-family indices to ignored. Legacy
dependency flags are empty for explicit transitions; legacy render-pass
dependencies retain `ByRegion` because the render pass, not the explicit
transition emitter, owns those attachment barriers.

### Render-pass reconciliation and focused tests

Legacy render-pass automatic transitions remain the sole native barrier owner
between attachment initial/subpass/final layouts. Entry expands every color,
resolve, depth, and stencil attachment into tracker units, validates its
declared initial access/layout (or accepts `Undefined` only with a non-load
action), and records pending final states without mutation. Explicit transition
commands are rejected while a render pass is active. Successful pass end
commits all declared final states atomically; begin/end failure leaves tracked
state unchanged.

Stage 1 adds `RHIResourceTransitionValidationTests` under RHITests. Stage 2 adds
`RHICommandListTransitionTests` beside existing command ownership/parity tests.
Stage 3 adds `VulkanResourceTransitionMappingTests` and
`VulkanResourceTransitionTests` under VulkanRHITests. Stage 4 extends existing
`RenderTargetLayoutTests`, Vulkan texture sampling/readback, viewport,
failure-injection, and repeated upload/readback coverage. Stage 5 runs those
named suites in supported inline/threaded modes plus the plan's regression and
qualification matrix.

Stage 0 handoff: baseline `4eed3955a02cd8b8b067f313ab1de8436a08886d`;
working set `RHI`, `VulkanRHI`, `RHITests`, `RenderCoreTests`, and
`VulkanRHITests`; key symbols `ERHIAccess`, `ERHITextureLayout`,
`FRHIAttachmentLayout`, `FVulkanTexture::SubresourceLayouts`,
`FVulkanBuffer::Write`, `FVulkanCommandListContext::RHIBeginRenderPass`,
`FVulkanCommandListContext::RHIEndRenderPass`, `FVulkanViewport::Present`, and
`FVulkanSwapchain::AcquireImageIndex`; open decisions: none that can change the
public descriptor shape. Validation: targeted source audit and plan validation;
no build is required for this documentation-only stage.

## Implementation Stages

### Stage 0: Freeze the transition and state contract

- [x] Inventory every current buffer/image state mutation in render passes,
  uploads, initialization, readback, viewport acquire/present, and tests.
- [x] Record the exact portable access values and legal read/write
  combinations, including graphics-versus-compute shader intent.
- [x] Freeze buffer byte-range and texture aspect/mip/layer normalization,
  whole-resource helpers, discard semantics, and overlap rules.
- [x] Freeze the expected-before validation policy and the initial-state policy
  for every buffer/texture creation usage.
- [x] Record the pure Vulkan mapping table for synchronization2 and legacy
  barriers, including conservative fallback stages.
- [x] Define how legacy render-pass automatic layouts validate and commit
  through the general tracker without duplicating barrier ownership.
- [x] Name focused test suites and preserve a handoff with the audited working
  set before implementation expands.

#### Acceptance Gate

- One reviewed contract accounts for every current mutation path, every public
  state has an unambiguous native mapping, invalid combinations/ranges are
  enumerated, and no open decision can change public descriptor shape.

### Stage 1: Add portable descriptors and validation

- [x] Extend the RHI access vocabulary and add normalized buffer and texture
  transition descriptors without exposing Vulkan concepts.
- [x] Add descriptor validation for null resources, resource type/usage,
  aspects, ranges, state combinations, and discard rules.
- [x] Add whole-resource helpers that lower to the validated descriptors.
- [x] Keep render-pass layout compatibility stable while adapting attachment
  access to the shared vocabulary.
- [x] Add pure RHI tests for valid and invalid states, overflow, edge ranges,
  aspects, and helper equivalence.

#### Acceptance Gate

- All supported current and future-compute handoffs are representable, invalid
  descriptors fail without backend access, and existing render-pass
  descriptions remain source-compatible or have a bounded migration recorded.

Stage 1 handoff: baseline `8e596cdc`; working set
`RHIResources.h/.cpp`, bounded render-pass access-name consumers, and
`RHIResourceTransitionValidationTests`; key symbols `ERHIAccess`,
`ERHITextureAspect`, `FRHIBufferTransition`, `FRHITextureTransition`,
`GetTextureLayoutForAccess`, `ValidateBufferTransitions`, and
`ValidateTextureTransitions`; decisions: public descriptors use exact counts,
texture creation flags are retained in `FRHITexture` for backend-neutral usage
validation, and render-pass layout remains a compatibility field derived from
access. Open questions: none for Stage 2 transport. Validation profile
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor-Tests`: targeted builds
`RHIResourceTransitionValidationTests`, `RHICommandListTests`,
`RenderContractTests`, `EditorRenderingTests`, `VulkanRHIIntegrationTests`, and
`TextureEditor` passed; tests passed 6/6 transition validation, 42/42 command
list, 3/3 render-target layout, and 5/5 renderer layout cases.

### Stage 2: Record and replay transition commands

- [x] Add context and command-list transition entrypoints for normalized
  descriptor batches.
- [x] Add a typed recorded command whose owned payload accounting includes the
  descriptor batch and whose captures retain every resource.
- [x] Enforce render-pass ordering and active-pipeline rules consistently for
  regular and immediate lists.
- [x] Add fake-context tests for order, batch contents, empty batches, recorder
  destruction, deferred replay, resource lifetime, and inline/threaded parity.

#### Acceptance Gate

- Transition commands replay in admitted order with exact descriptors and live
  resources in both executor modes, while illegal pass ordering and malformed
  batches fail before partial execution.

Stage 2 handoff: baseline `443101c1`; working set `RHIContext.h`,
`RHICommandList.h/.cpp`, `VulkanContext.h/.cpp`, and
`RHICommandListTests.cpp`; key symbols
`IRHICommandContext::RHITransitionBuffers`,
`IRHICommandContext::RHITransitionTextures`,
`FRHICommandListBase::TransitionBuffers`,
`FRHICommandListBase::TransitionTextures`, `FBufferTransitionCommand`, and
`FTextureTransitionCommand`; decisions: transitions use the operation context
and are legal with `ERHIPipeline::None` or any future admitted pipeline, but
never inside a legacy render pass. Vulkan replay intentionally terminates until
Stage 3 installs the authority, preventing a silent no-op backend path. Open
questions: none for the Vulkan tracker. Validation profile
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor-Tests`:
`RHICommandListTests` passed 47/47 and `VulkanRHIIntegrationTests` compiled.

### Stage 3: Implement the Vulkan state authority and barrier mapping

- [x] Replace layout-only image tracking with checked portable/native
  subresource state and add deterministic buffer-range state tracking.
- [x] Implement one pure portable-to-Vulkan mapping used by both
  synchronization2 and legacy emission.
- [x] Emit exact image and buffer barriers on the selected immediate queue and
  update state only after successful recording.
- [x] Diagnose resource identity, range, expected state, tracked state, and
  requested state on mismatch without leaking native handles into RHI APIs.
- [x] Add hardware-backed tests for buffer and texture transitions, disjoint
  subresources, overlapping buffer ranges, transfer-write visibility, and both
  mapping paths where the test environment permits; cover compute intent at the
  existing backend test seam without adding public dispatch.

#### Acceptance Gate

- Vulkan validation remains clean; synchronization2 and fallback mappings
  produce equivalent observable results; subresource/range state does not
  bleed across disjoint regions; and no transition requires a global idle wait.

Stage 3 handoff: baseline `78436d74`; working set
`VulkanResourceState.h/.cpp`, `VulkanBuffer.h/.cpp`, `VulkanTexture.h/.cpp`,
`VulkanContext.cpp`, and `VulkanResourceTransitionTests.cpp`; key symbols
`MapVulkanResourceState`, `FVulkanBufferStateTracker`,
`FVulkanTextureStateTracker`,
`FVulkanCommandListContext::RHITransitionBuffers`, and
`FVulkanCommandListContext::RHITransitionTextures`; decisions: resources store
portable access while the pure mapper deterministically reconstructs native
stage/access/layout, discard emits no source dependency, a whole batch is
validated before any native call, and state is committed only after barrier
recording returns successfully. Legacy layout accessors temporarily adapt to
the same tracker for Stage 4 migration rather than retaining a second store.
Open questions: none for implicit-path reconciliation. Validation profile
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor-Tests`:
`VulkanRHIIntegrationTests` compiled and the focused transition filter passed
5/5, including a hardware buffer/image barrier test on the published
synchronization2 path; pure mapping tests cover the legacy fallback.

### Stage 4: Reconcile render passes, transfers, readback, and presentation

- [x] Validate attachment initial state and commit attachment final state
  through the authoritative tracker, including resolve and depth/stencil
  attachments.
- [x] Route texture initialization and upload pre/post state through the common
  mapping while preserving partial-mip and partial-layer contents.
- [x] Route readback pre/post state through the common mapping and restore the
  exact tracked state rather than an inferred graphics state.
- [x] Reconcile buffer host writes and current transfer-backed writes with the
  selected buffer-state policy.
- [x] Reconcile swapchain acquire, render-target use, and presentation without
  adding queue-family ownership transfer.
- [x] Extend regression tests for texture sampling, compressed mips, readback,
  MRT/resolve/depth, viewport recreation, and repeated upload/readback cycles.

#### Acceptance Gate

- Explicit transitions and every existing implicit state-changing convenience
  path agree on one tracker; current graphics, texture, readback, and WSI tests
  pass without divergent layouts, hard-coded graphics restoration, or new
  device-idle synchronization.

Stage 4 handoff: baseline `89eda5eb`; working set `VulkanBuffer.cpp`,
`VulkanTexture.cpp`, `VulkanContext.h/.cpp`, and `VulkanViewport.h/.cpp`; key
symbols `FVulkanBuffer::Write`, `FVulkanDynamicRHI::InitializeTexture`,
`FVulkanDynamicRHI::UpdateTexture2D`, `FVulkanDynamicRHI::ReadTexture2D`,
`FVulkanCommandListContext::RHIBeginRenderPass`,
`FVulkanCommandListContext::RHIEndRenderPass`,
`FVulkanBackBuffer::AcquireBackBufferImage`, and
`FVulkanBackBuffer::CommitPresentedImageState`; decisions: legacy render passes
remain native barrier owners while validating/committing portable state,
synchronous readback retains only its existing completion wait, implicit writes
use exact-range tracker updates plus common transitions, and swapchain acquire
restores the selected image's prior state without inventing ownership transfer.
Open questions: none for qualification. Validation profile
`windows-msvc-x64`, preset `Win64-Debug-DurinEditor-Tests`:
`VulkanRHIIntegrationTests` passed 24/24, covering sampling, compressed mips,
storage, readback, failure injection, swapchain recreation, explicit
transitions, and repeated resource lifecycle paths.

### Stage 5: Qualify the shared foundation and publish lasting contracts

- [x] Run focused RHI, RenderCore, and Vulkan transition suites in inline and
  threaded execution modes where supported by the test harness.
- [x] Run the existing Vulkan sampling, failure-injection, viewport, and
  shutdown regressions plus the selected native suite.
- [x] Complete the normal Debug Editor full build and hidden-window runtime
  smoke using the repository build/run guidance.
- [x] Document the lasting RHI transition/state contract under
  `Documentation/Runtime/Rendering/`.
- [x] Update both owning roadmaps with completion evidence and the stable M2
  entry handoff; do not activate compute M2 or RHI M2 in this plan.

#### Acceptance Gate

- The full validation matrix passes, lasting documentation owns the shipped
  behavior, both roadmaps reference one completed child, and downstream plans
  can consume the public transition API without Vulkan-specific knowledge.

Stage 5 handoff: baseline `4bea0d07`; working set
`Documentation/Runtime/Rendering/RHIResourceTransitions.md`, this plan, and
the Compute Shader Pipeline and RHI/Vulkan Backend Evolution roadmaps; key
contracts `ERHIAccess`, `FRHIBufferTransition`, `FRHITextureTransition`,
`FRHICommandListBase::TransitionBuffers`,
`FRHICommandListBase::TransitionTextures`, `MapVulkanResourceState`,
`FVulkanBufferStateTracker`, and `FVulkanTextureStateTracker`; decisions: the
completed plan remains in place pending the repository's periodic archive
batch, compute and RHI M2 entry gates are met but neither child is activated,
and the lasting Runtime document owns shipped behavior. Open questions: none.
Validation profile `windows-msvc-x64`, preset
`Win64-Debug-DurinEditor-Tests`: `test --target
RHIResourceTransitionValidationTests` passed 6/6, `test --target
RHICommandListTests` passed 47/47, `test --target RenderContractTests` passed
39/39, `test --target VulkanRHIIntegrationTests` passed 24/24, `test --target
all` passed, `build --target all --agent` passed, and `run --project
Sandbox\Sandbox.dproject --args --hidden-window --exit-after-ticks=10` passed
with clean shutdown.

## Validation Matrix

| Layer | Required evidence |
| --- | --- |
| Contract/unit | Descriptor/state/range validation, mapping-table equivalence, buffer interval splitting/merging, and image subresource isolation. |
| Recording | Typed command ordering, payload accounting, retained lifetimes, invalid pass ordering, regular/immediate lists, and inline/threaded replay parity. |
| Vulkan integration | Buffer and image visibility, graphics/compute-intent mappings, synchronization2/fallback emission, render-pass entry/exit, upload, readback, and presentation. |
| Regression | Existing texture sampling/compression, MRT/MSAA/depth, viewport recreation, initialization failure, dedicated-thread shutdown, and resource lifetime suites. |
| Qualification | Selected native tests, full Debug Editor `all` build, validation-clean hidden-window editor smoke, and clean shutdown. |

All build, test, and runtime commands follow
[Build and Run](../../../Development/Build/BuildAndRun.md). The implementation handoff
must record the exact profile, commands, test filters, and outcomes actually
used.

## Definition of Done

- One portable state/range vocabulary covers current graphics, transfer,
  readback, presentation, and next-step compute dependencies.
- Recorded transitions retain resources and replay identically inline or on the
  dedicated RHI thread.
- One Vulkan authority owns buffer and texture subresource state across explicit
  transitions and existing implicit operations.
- Synchronization2 and legacy fallback consume the immutable startup capability
  and preserve equivalent correctness.
- No required path depends on `RHIBlockUntilGPUIdle`, device idle, incidental
  command-buffer submission, or a graphics-only stage guess.
- Existing rendering and texture behavior remains validation-clean.
- Stable contracts move to Runtime rendering documentation and both roadmaps
  carry completion evidence.

## Deferred Follow-ups

- Compute PSOs and direct dispatch (`SynchronousComputePipeline`).
- Resource views and general copy/blit/resolve operations
  (`RHIResourceViewsAndTransfers`).
- GPU completion, staging reuse, memory telemetry, and retirement
  (`VulkanMemoryTransferAndRetirement`).
- Queue ownership and asynchronous execution (`AsyncComputeExecution` or a
  later measured transfer plan).
- Render-graph barrier synthesis and transient aliasing.

## Related Documentation

- [Compute Shader Pipeline Roadmap](../../../Roadmaps/ComputeShaderPipeline.md)
- [RHI and Vulkan Backend Evolution Roadmap](../../../Roadmaps/RHIAndVulkanEvolution.md)
- [RHI Capabilities and Vulkan Startup](../../../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Texture System](../../../Runtime/Rendering/TextureSystem.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderPass.cpp`
- `Engine/Tests/Native/RHITests/`
- `Engine/Tests/Native/RenderCoreTests/`
- `Engine/Tests/Native/VulkanRHITests/`
