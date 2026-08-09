# GPU Resource Transitions Plan

Summary: Establish one backend-neutral buffer and texture transition contract and one authoritative Vulkan resource-state tracker shared by graphics, transfer, readback, presentation, and future compute work.

Last reviewed: 2026-08-10

Status: Active
Completed:

## Current Status

The plan is active at Stage 0 against baseline
`4eed3955a02cd8b8b067f313ab1de8436a08886d`. Its entry gate is satisfied:
recorded command batches and dedicated-thread replay are established, the RHI
capability snapshot now publishes `bSupportsSynchronization2`, and the current
state-mutating paths have a bounded implementation inventory.

The audit found three state authorities that must be reconciled rather than
extended independently:

- render-pass descriptions carry initial/final access and layouts while
  `FVulkanCommandListContext` commits only final image layouts;
- `FVulkanTexture` tracks one Vulkan layout per mip/layer but not the access or
  pipeline stages that produced it;
- texture upload and readback record their own legacy barriers, infer previous
  access from layout/creation flags, and hard-code graphics stages.

The first implementation stage freezes the portable vocabulary, exact range
semantics, render-pass reconciliation, and barrier fallback rules before public
headers change. This plan is the shared M1 child of both the Compute Shader
Pipeline and RHI/Vulkan roadmaps; it is not a compute-only synchronization path.

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

## Implementation Stages

### Stage 0: Freeze the transition and state contract

- [ ] Inventory every current buffer/image state mutation in render passes,
  uploads, initialization, readback, viewport acquire/present, and tests.
- [ ] Record the exact portable access values and legal read/write
  combinations, including graphics-versus-compute shader intent.
- [ ] Freeze buffer byte-range and texture aspect/mip/layer normalization,
  whole-resource helpers, discard semantics, and overlap rules.
- [ ] Freeze the expected-before validation policy and the initial-state policy
  for every buffer/texture creation usage.
- [ ] Record the pure Vulkan mapping table for synchronization2 and legacy
  barriers, including conservative fallback stages.
- [ ] Define how legacy render-pass automatic layouts validate and commit
  through the general tracker without duplicating barrier ownership.
- [ ] Name focused test suites and preserve a handoff with the audited working
  set before implementation expands.

#### Acceptance Gate

- One reviewed contract accounts for every current mutation path, every public
  state has an unambiguous native mapping, invalid combinations/ranges are
  enumerated, and no open decision can change public descriptor shape.

### Stage 1: Add portable descriptors and validation

- [ ] Extend the RHI access vocabulary and add normalized buffer and texture
  transition descriptors without exposing Vulkan concepts.
- [ ] Add descriptor validation for null resources, resource type/usage,
  aspects, ranges, state combinations, and discard rules.
- [ ] Add whole-resource helpers that lower to the validated descriptors.
- [ ] Keep render-pass layout compatibility stable while adapting attachment
  access to the shared vocabulary.
- [ ] Add pure RHI tests for valid and invalid states, overflow, edge ranges,
  aspects, and helper equivalence.

#### Acceptance Gate

- All supported current and future-compute handoffs are representable, invalid
  descriptors fail without backend access, and existing render-pass
  descriptions remain source-compatible or have a bounded migration recorded.

### Stage 2: Record and replay transition commands

- [ ] Add context and command-list transition entrypoints for normalized
  descriptor batches.
- [ ] Add a typed recorded command whose owned payload accounting includes the
  descriptor batch and whose captures retain every resource.
- [ ] Enforce render-pass ordering and active-pipeline rules consistently for
  regular and immediate lists.
- [ ] Add fake-context tests for order, batch contents, empty batches, recorder
  destruction, deferred replay, resource lifetime, and inline/threaded parity.

#### Acceptance Gate

- Transition commands replay in admitted order with exact descriptors and live
  resources in both executor modes, while illegal pass ordering and malformed
  batches fail before partial execution.

### Stage 3: Implement the Vulkan state authority and barrier mapping

- [ ] Replace layout-only image tracking with checked portable/native
  subresource state and add deterministic buffer-range state tracking.
- [ ] Implement one pure portable-to-Vulkan mapping used by both
  synchronization2 and legacy emission.
- [ ] Emit exact image and buffer barriers on the selected immediate queue and
  update state only after successful recording.
- [ ] Diagnose resource identity, range, expected state, tracked state, and
  requested state on mismatch without leaking native handles into RHI APIs.
- [ ] Add hardware-backed tests for buffer and texture transitions, disjoint
  subresources, overlapping buffer ranges, transfer-write visibility, and both
  mapping paths where the test environment permits; cover compute intent at the
  existing backend test seam without adding public dispatch.

#### Acceptance Gate

- Vulkan validation remains clean; synchronization2 and fallback mappings
  produce equivalent observable results; subresource/range state does not
  bleed across disjoint regions; and no transition requires a global idle wait.

### Stage 4: Reconcile render passes, transfers, readback, and presentation

- [ ] Validate attachment initial state and commit attachment final state
  through the authoritative tracker, including resolve and depth/stencil
  attachments.
- [ ] Route texture initialization and upload pre/post state through the common
  mapping while preserving partial-mip and partial-layer contents.
- [ ] Route readback pre/post state through the common mapping and restore the
  exact tracked state rather than an inferred graphics state.
- [ ] Reconcile buffer host writes and current transfer-backed writes with the
  selected buffer-state policy.
- [ ] Reconcile swapchain acquire, render-target use, and presentation without
  adding queue-family ownership transfer.
- [ ] Extend regression tests for texture sampling, compressed mips, readback,
  MRT/resolve/depth, viewport recreation, and repeated upload/readback cycles.

#### Acceptance Gate

- Explicit transitions and every existing implicit state-changing convenience
  path agree on one tracker; current graphics, texture, readback, and WSI tests
  pass without divergent layouts, hard-coded graphics restoration, or new
  device-idle synchronization.

### Stage 5: Qualify the shared foundation and publish lasting contracts

- [ ] Run focused RHI, RenderCore, and Vulkan transition suites in inline and
  threaded execution modes where supported by the test harness.
- [ ] Run the existing Vulkan sampling, failure-injection, viewport, and
  shutdown regressions plus the selected native suite.
- [ ] Complete the normal Debug Editor full build and hidden-window runtime
  smoke using the repository build/run guidance.
- [ ] Document the lasting RHI transition/state contract under
  `Documentation/Runtime/Rendering/`.
- [ ] Update both owning roadmaps with completion evidence and the stable M2
  entry handoff; do not activate compute M2 or RHI M2 in this plan.

#### Acceptance Gate

- The full validation matrix passes, lasting documentation owns the shipped
  behavior, both roadmaps reference one completed child, and downstream plans
  can consume the public transition API without Vulkan-specific knowledge.

## Validation Matrix

| Layer | Required evidence |
| --- | --- |
| Contract/unit | Descriptor/state/range validation, mapping-table equivalence, buffer interval splitting/merging, and image subresource isolation. |
| Recording | Typed command ordering, payload accounting, retained lifetimes, invalid pass ordering, regular/immediate lists, and inline/threaded replay parity. |
| Vulkan integration | Buffer and image visibility, graphics/compute-intent mappings, synchronization2/fallback emission, render-pass entry/exit, upload, readback, and presentation. |
| Regression | Existing texture sampling/compression, MRT/MSAA/depth, viewport recreation, initialization failure, dedicated-thread shutdown, and resource lifetime suites. |
| Qualification | Selected native tests, full Debug Editor `all` build, validation-clean hidden-window editor smoke, and clean shutdown. |

All build, test, and runtime commands follow
[Build and Run](../Development/Build/BuildAndRun.md). The implementation handoff
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

- [Compute Shader Pipeline Roadmap](../Roadmaps/ComputeShaderPipeline.md)
- [RHI and Vulkan Backend Evolution Roadmap](../Roadmaps/RHIAndVulkanEvolution.md)
- [RHI Capabilities and Vulkan Startup](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

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
