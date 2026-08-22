# Synchronous Compute Pipeline Plan

Summary: Add backend-neutral compute PSO creation, reflected binding, direct dispatch, and storage-result validation on the existing synchronous Vulkan immediate queue.

Last reviewed: 2026-08-12

Status: Archived
Completed: 2026-08-12

## Current Status

Completed on 2026-08-12. Public RHI now owns canonical compute PSO creation,
recorded binding/push constants/direct dispatch, shared-immediate-context replay,
compute descriptor state, exact dispatch limits, storage buffer/image results,
compute-to-compute, compute-to-graphics, and copy/readback handoffs. Focused
tests, both executor modes, the native aggregate, full Debug Editor build,
validation-enabled hidden-window smoke, and documentation validation passed.

## Goal

Allow backend-neutral code to compile one compute shader, create or reuse a
complete compute PSO by canonical identity, bind reflected resources and push
constants, dispatch direct work through a recorded command list, and transition
the output for later compute, graphics, copy, or CPU readback use without raw
Vulkan calls or a whole-device idle dependency.

## Scope

- Compute PSO initializer, canonical key, RHI resource/reference type, factory,
  validation, diagnostics, and bounded Vulkan reuse.
- Compute pipeline selection and replay through the existing synchronous
  immediate command context.
- Recorded compute PSO binding, compute-aware shader parameters and push
  constants, and three-dimensional direct dispatch.
- Shared Vulkan pipeline-layout and descriptor-snapshot facilities where their
  contracts are identical, with separate graphics and compute pending state.
- Public-RHI storage-buffer and storage-image dispatch, synchronization,
  readback, failure, lifetime, inline-executor, and dedicated-thread evidence.
- Lasting compute-pipeline documentation and roadmap status updates.

## Non-Goals

- A dedicated compute queue, queue-family ownership transfer, cross-queue
  semaphore policy, or overlap scheduling.
- Indirect dispatch, GPU-generated argument buffers, or a render graph.
- Selecting or implementing the first Renderer-owned compute workload; that is
  the M3 `ComputeRendererIntegration` plan.
- Bindless resources, partially bound descriptor arrays, or typed uniform-byte
  parameter blocks.
- General shader-stage architecture changes unrelated to the compute vertical
  slice.

## Design Decisions and Invariants

### Public identity and ownership

- `FComputePipelineStateInitializer` contains exactly one non-null compute
  shader and one reflected `FPipelineLayoutDesc`. It carries no graphics fixed
  state, vertex input, render-target layout, or render-pass compatibility.
- `BuildComputePipelineStateKey` validates shader frequency, descriptor layout,
  stage visibility, push-constant ranges, and device limits before backend
  creation. The canonical key contains the compute shader content hash and the
  complete canonical pipeline layout.
- `DebugName` is diagnostics-only. There is no name lookup. Equal canonical
  descriptors may reuse one complete device-owned PSO; different descriptors
  cannot alias even when their debug names match.
- Compute cache ownership is bounded and least-recently-used, follows the
  existing cache-only eviction rule, and publishes no partial or failed entry.
  Explicit consumers still own publication, replacement, and fallback policy.
- The public result is complete or null. Expected Vulkan creation failure uses
  the existing fallible RHI creation boundary and does not poison later retry.

### Recording and execution

- `SwitchPipeline(Compute)` is legal only outside a render pass. It resolves to
  the same immediate Vulkan command context and queue currently used by
  graphics; pipeline selection does not imply asynchronous execution.
- `SetComputePipelineState`, compute shader parameters, compute push constants,
  and `Dispatch` are recorded typed commands. Payloads retain the PSO, shader,
  views, and parent resources until replay completes in both executor modes.
- `FRHICapabilities` publishes the three maximum direct-dispatch group counts
  from the admitted device. `Dispatch` requires an active compute pipeline and
  no render pass; zero or over-limit group counts are rejected while recording
  rather than silently omitted or deferred to native dispatch.
- Graphics draw commands remain graphics-only. Compute dispatch remains
  compute-only. Transitions stay pipeline-neutral and outside render passes.

### Vulkan state and descriptors

- The initial compute path uses the already-provisioned shared queue family and
  `FVulkanCommandListContext`; it adds no command pool, queue wrapper,
  submission path, or ownership transfer.
- Pipeline-layout construction, descriptor-set snapshot identity, resource
  retention, frame-generation reset, pool-completion gating, and budget
  enforcement become common facilities only where graphics and compute have
  the same contract. Viewport, scissor, vertex/index, render-pass, and draw
  state remain graphics-only.
- Compute has its own active PSO and per-PSO pending descriptor snapshot. A
  pipeline switch cannot inherit bindings from an incompatible graphics or
  compute layout.
- Descriptor validation requires the exact tracked compute access state:
  uniform and sampled resources use compute-readable intent; storage resources
  use compute read/write intent. Vulkan layouts and stage/access masks continue
  to come only from the shared resource-state authority.
- Push-constant and parameter updates must target the compute shader owned by
  the active compute PSO and must match its reflected layout. Dispatch requires
  every reflected descriptor element to be populated.

## Current Foundations and Gaps

| Area | Existing foundation | Plan gap |
| --- | --- | --- |
| Shader frontend | `EShaderFrequency::Compute`, Slang stage selection, reflection, cache serialization, shader creation, and layout merging exist. | Add a compute shader-map fixture and prove its single-stage layout feeds the public PSO factory. |
| Resources and synchronization | Storage buffers/images, counted views, exact transitions, compute access mappings, copy, and readback exist. | Validate compute descriptor state and all post-dispatch consumer handoffs through public RHI. |
| Command recording | Typed recorded payloads, pipeline tracking, inline/threaded replay, and retained resource lifetimes exist. | Admit compute context selection and add compute PSO, parameter, push-constant, and dispatch commands. |
| Vulkan startup | Device admission requires one graphics/compute/present family; graphics, compute, transfer, and present aliases share queue zero. | Use the existing immediate context without advertising a separate or asynchronous compute path. |
| Pipeline infrastructure | Canonical graphics keys, structural layouts, bounded PSO/descriptor caches, driver cache, failure translation, and deferred deletion exist. | Add compute identity/creation and extract descriptor mechanics from graphics-only pending state without weakening graphics validation. |
| Validation | Reflection/storage tests and a raw Vulkan compute dispatch/readback test exist. | Replace the raw proof as acceptance evidence with public-RHI buffer/image, interop, cache, failure, lifetime, and executor-mode coverage. |

## Implementation Stages

### Stage 0: Freeze the public contract and baseline the missing path

- [x] Inventory every DynamicRHI implementation/test double, command replay
  seam, pending-state owner, cache statistic consumer, and native fixture that
  must change for the named compute API.
- [x] Record the focused rejection matrix for null/non-compute shaders,
  shader/layout stage mismatch, malformed descriptor arrays, overlapping or
  out-of-limit push constants, zero/over-limit dispatch dimensions, dispatch
  inside a render pass, graphics commands under compute, and compute commands
  under graphics.
- [x] Convert the existing raw Vulkan fixture into an explicit baseline: retain
  it as backend proof until the public-RHI replacement passes, but prevent it
  from being counted as M2 completion evidence.
- [x] Inventory graphics pending-state fields and separate genuinely shared
  descriptor snapshot/layout mechanics from graphics-only dynamic and draw
  state before changing production code.
- [x] Confirm the exact Vulkan property source and public field names for the
  three direct-dispatch group-count limits, plus the startup rejection rule for
  unavailable or zero limits.

#### Acceptance Gate

- The public API and rejection semantics are unambiguous, the current failure
  points are reproduced by focused tests, and descriptor-state extraction has
  a bounded symbol/file inventory with no unresolved queue or ownership choice.

### Stage 1: Add compute capability, PSO identity, and complete-or-null creation

- [x] Add the three direct-dispatch group-count limits to `FRHICapabilities`,
  publish them from Vulkan startup, and cover unavailable/zero admission plus
  exact value publication in RHI initialization tests.
- [x] Add `FComputePipelineStateInitializer`, canonical key construction and
  hashing, `FRHIComputePipelineState`, its RHI reference alias, forward
  declarations, and `RHICreateComputePipelineState`.
- [x] Add compile-time and validation tests for the compute initializer,
  canonical key, resource/reference type, DynamicRHI factory, and all Stage 0
  initializer/capability rejection cases.
- [x] Validate the exact compute shader frequency, reflected descriptor layout,
  push-constant stage/range contract, and supported device limits before
  scheduling Vulkan creation.
- [x] Add `FVulkanComputePipelineState` with one structural layout, pipeline
  layout, and native compute pipeline; keep native candidates local until all
  construction succeeds.
- [x] Route expected native failure through the existing fallible creation
  operation, roll back pipeline and layout handles exactly once, and keep later
  retry usable.
- [x] Add a bounded canonical compute-PSO cache beside the existing graphics
  cache and reuse the driver pipeline cache and structural-layout cache.
- [x] Generalize `FRHIGraphicsCacheStatistics` and its getter/reset API to
  pipeline-cache terminology, keep shared descriptor/layout counters
  aggregated, and expose separate graphics and compute PSO
  hit/miss/creation/eviction/failure occupancy.
- [x] Verify same-name/different-key separation, different-name/equal-key reuse,
  full-cache eviction, cache pressure with externally owned entries, failure
  non-publication, deferred destruction, and orderly shutdown.

#### Acceptance Gate

- The public factory returns null or one valid compute PSO; canonical equality
  alone controls bounded reuse, all failed native candidates roll back, and
  graphics pipeline creation/cache behavior remains unchanged.

### Stage 2: Record, replay, bind, and dispatch compute work

- [x] Extend DynamicRHI context selection so graphics and compute both resolve
  to the supported shared immediate context while `None` resolves to no active
  context.
- [x] Teach command recording and replay to admit `SwitchPipeline(Compute)` and
  retain the active pipeline distinction without duplicating the underlying
  synchronous context.
- [x] Add `RHISetComputePipelineState`/`SetComputePipelineState` and
  `RHIDispatch`/`Dispatch` typed commands with PSO lifetime retention and exact
  recording-time/replay-time validation, including zero and over-limit group
  counts from the published capability snapshot.
- [x] Make push constants and shader-parameter recording pipeline-aware. Retain
  graphics behavior and require compute-only stage/shader ownership on the
  compute path.
- [x] Add Vulkan compute pending state that binds the compute pipeline, applies
  reflected descriptor snapshots with `eCompute` bind points and stage masks,
  validates required resource states, and emits `vkCmdDispatch`.
- [x] Clear incompatible pending state on PSO deletion, frame-generation reset,
  pipeline switches, and context reset without rescanning unrelated cache
  entries.

#### Acceptance Gate

- Regular and immediate command lists record and replay a compute PSO,
  parameters, push constants, and direct dispatch in exact order through the
  shared context; invalid pipeline/render-pass combinations fail
  deterministically and existing graphics command-list tests remain green.

### Stage 3: Prove storage buffers, storage images, and consumer handoffs

- [x] Add a RenderCore compute shader type and typed parameters covering a
  storage buffer, storage image, sampled input, uniform range, and push
  constants; build its PSO layout exclusively from reflection.
- [x] Add a public-RHI storage-buffer dispatch whose output is transitioned to
  transfer read and verified through the existing readback path without a
  whole-device idle memory dependency.
- [x] Add consecutive compute dispatches with an explicit write-to-read/write
  transition and deterministic second-pass output.
- [x] Add a public-RHI storage-image dispatch and validate exact mip/layer view
  state, output bytes, and descriptor layout.
- [x] Add compute-write to graphics-read coverage using a bounded test draw,
  proving the same resource-state authority and descriptor snapshot survive a
  compute-to-graphics pipeline switch.
- [x] Exercise partial ranges, missing/incorrect transitions, mismatched views,
  missing descriptors, resource replacement, cache hits, and recorded-command
  lifetime until submission completion.

#### Acceptance Gate

- Storage-buffer and storage-image results, compute-to-compute,
  compute-to-graphics, and compute-to-copy/readback dependencies all pass
  through public RHI with Vulkan validation clean and no raw handles outside
  the Vulkan test/backend boundary.

### Stage 4: Qualify both executors and publish the lasting contract

- [x] Run focused `RHITests`, `RenderCoreTests`, and `VulkanRHITests` coverage
  through the root [build and run](../../../Development/Build/BuildAndRun.md)
  workflow, first in the default dedicated-thread mode and then with
  `DURIN_RHI_EXECUTION=inline` for the compute vertical slice.
- [x] Because the change crosses RHI, RenderCore, VulkanRHI, recorded command
  infrastructure, and multiple native targets, run the native aggregate at
  default target granularity and a successful full `all` build.
- [x] Run a validation-enabled hidden-window Debug Editor smoke from the same
  Agent Build Profile, including startup, several frames, and orderly shutdown.
- [x] Remove or narrow the raw Vulkan compute test once every behavior it proves
  has a public-RHI owner; retain only backend-native failure/translation
  coverage that cannot be expressed portably.
- [x] Publish the stable compute PSO, binding, dispatch, synchronization,
  cache/lifetime, and synchronous-queue contract under
  `Documentation/Runtime/Rendering/` and remove compute limitations that are no
  longer true from shader-parameter documentation.
- [x] Mark M2 complete in the
  [Compute Shader Pipeline roadmap](../../../Roadmaps/Archive/2026-08/ComputeShaderPipeline.md) and
  record the M3 entry state without selecting its renderer consumer here.

#### Acceptance Gate

- Focused targets in both executor modes, the native aggregate, full build,
  validation-enabled runtime smoke, documentation validation, and all M2
  validation-matrix rows pass; the lasting documentation is authoritative and
  M3 can select a renderer consumer without revisiting core compute design.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Compute shader map to PSO | One compute shader and reflected layout produce one complete public PSO | RenderCore and Vulkan focused tests |
| Invalid initializer or native creation failure | Deterministic rejection or null result; no partial cache entry; later retry succeeds | RHI validation and Vulkan failure-injection tests |
| Canonical cache identity | Equal full descriptors reuse; unequal descriptors and same names do not alias; bounded eviction releases cache-only entries | Vulkan cache tests |
| Recorded compute commands | PSO, shader, descriptor resources, and push bytes survive inline/threaded replay in exact order | RHI command-list tests |
| Dispatch admission | Active compute pipeline, no render pass, nonzero in-limit group counts | RHI and Vulkan negative tests |
| CPU/upload to compute read | Explicit transition makes uploaded input visible to compute | Public-RHI Vulkan integration test |
| Compute write to compute read/write | Second dispatch observes the first only through the declared transition | Public-RHI Vulkan integration test |
| Compute write to graphics read | Draw consumes compute output after an explicit pipeline-neutral transition | Public-RHI interop test |
| Compute write to copy/readback | Expected bytes return without using device idle as the memory dependency | Public-RHI buffer/image readback tests |
| Descriptor completeness and state | Missing, mismatched, stale, or incorrectly transitioned resources fail before native dispatch | RHI/Vulkan negative tests |
| Replacement and shutdown lifetime | Recorded work retains old resources; cache/context/device shutdown drains cleanly | Lifetime counters and shutdown test |

## Definition of Done

- Backend-neutral code can create, bind, and directly dispatch a compute PSO
  with reflected storage resources and push constants.
- Compute uses canonical descriptor identity, bounded reuse, diagnostic-only
  names, complete-or-null creation, and completion-safe lifetime behavior.
- Graphics and compute share only common layout/descriptor mechanisms;
  graphics dynamic state and all existing draw behavior remain intact.
- Buffer and image results plus compute-to-compute, compute-to-graphics, and
  compute-to-copy/readback transitions pass through public RHI in both executor
  modes without Vulkan escape hatches or a whole-device idle dependency.
- Focused, aggregate, build, runtime, and documentation qualification pass, the
  lasting runtime contract is published, and roadmap M2 is complete.

## Deferred Follow-ups

- Renderer consumer selection, capability/fallback UX, resource reload policy,
  and measurable feature value (M3).
- Indirect dispatch and argument-buffer validation (conditional M4).
- Dedicated queue contexts, cross-queue synchronization, ownership transfer,
  overlap measurement, and synchronous fallback (evidence-gated M5).
- Bindless descriptors, partially bound arrays, and typed uniform-byte blocks.
- Compute-specific prewarming or persistent-cache policy beyond reuse of the
  existing Vulkan driver cache.

## Related Documentation

- [Compute Shader Pipeline roadmap](../../../Roadmaps/Archive/2026-08/ComputeShaderPipeline.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [Graphics State and Bindings](../../../Runtime/Rendering/GraphicsStateAndBindings.md)
- [Shader Parameters](../../../Runtime/Rendering/ShaderParameters.md)
- [RHI Capabilities and Vulkan Startup](../../../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h`
- `Engine/Source/Runtime/RHI/Public/RHICapabilities.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Public/RHIContext.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/RHI/Private/RHICommandList.cpp`
- `Engine/Source/Runtime/RenderCore/Public/Shader/Shader.h`
- `Engine/Source/Runtime/RenderCore/Private/Shader/Shader.cpp`
- `Engine/Source/Runtime/RenderCore/Private/Shader/SlangShaderCompiler.cpp`
- `Engine/Source/Runtime/VulkanRHI/Public/VulkanDynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanContext.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp`
- `Engine/Tests/Native/RHITests/Private/RHICommandListTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderFoundationTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderReflectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
