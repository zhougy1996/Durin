# RHI Capability and Vulkan Startup Plan

Summary: Publish a portable immutable RHI capability contract and make Vulkan instance, device, queue, texture-support, and structural-cache startup decisions explicit and transactional.

Last reviewed: 2026-08-09

Status: Active
Completed:

## Current Status

This plan executes M0 of the
[RHI and Vulkan Backend Evolution Roadmap](../Roadmaps/RHIAndVulkanEvolution.md).
It was activated after a roadmap and code audit at baseline commit
`bf8697ecf44f38f1a01cdae4344e6113567a1a2a` confirmed that command transport,
thread ownership, exact CPU serial waits, initialization rollback, and public
factory complete-or-null behavior are already established foundations.

The remaining startup boundary is not yet a coherent capability contract:

- `ERHIFeatureLevel` is a declaration without a runtime capability snapshot or
  consumer-facing query.
- Vulkan instance creation requests `VK_LAYER_KHRONOS_validation`
  unconditionally, while required, optional, diagnostic, and promoted
  extensions are activated through one undifferentiated path.
- physical-device selection ranks unsuitable devices with score zero but still
  selects the highest entry even when every candidate is unsuitable;
- logical-device queue creation is completed before surface compatibility is
  known, while later viewport setup may select a presentation family that was
  not provisioned during device creation;
- public texture descriptions advertise 2D arrays, cube arrays, and 3D
  textures, while native image creation always selects `vk::ImageType::e2D`
  and the support query checks format feature bits without validating the full
  description; and
- render-pass creation catches native failure, retains a null handle, and
  inserts the incomplete object into the structural cache.

Stage 0 freezes the supported platform/topology contract, the smallest useful
public capability vocabulary, and the exact structural-cache working set
before implementation changes begin. No implementation stage is complete yet.

## Goal

Make runtime capability and startup behavior truthful, portable, and
failure-atomic. After this plan, Renderer and later RHI plans can select a
supported path from one immutable snapshot, unsupported texture work is
rejected before native creation, Vulkan startup publishes only a suitable
device and provisioned queue topology, optional diagnostics never become a
shipping requirement, and structural-cache failure cannot poison a later
retry.

## Scope

- A backend-neutral immutable RHI capability and limit snapshot published only
  after successful backend initialization.
- A consumer inventory that justifies every public capability field and names
  the fallback or rejection behavior attached to it.
- Backend-neutral texture-description validation plus Vulkan support checks for
  the complete dimension, extent, format, usage, mip, layer, sample, and limit
  contract.
- Explicit classification and negotiation of Vulkan API version, instance
  layers, instance/device extensions, promoted core features, physical-device
  features, and queue families.
- Deterministic physical-device rejection, ranking, diagnostics, and
  transactional logical-device publication.
- A supported Win64 presentation topology for the main window and editor
  detached viewports, including the decision that makes surface-compatible
  queue families known or safely constrained before first use.
- Transactional creation and retry for startup-adjacent internal structural
  caches, beginning with render passes and covering directly dependent
  framebuffer, descriptor-layout, pipeline-layout, and pipeline candidates
  found by the Stage 0 inventory.
- Focused fake/public-RHI tests, Vulkan failure-injection tests, supported WSI
  qualification, runtime startup/shutdown coverage, and lasting contract
  documentation.

## Non-Goals

- General buffer/image access transitions, queue-family ownership transfers,
  or synchronization2 adoption; those belong to `GPUResourceTransitions`.
- Typed resource views, general copy/blit/resolve commands, or texture asset
  types beyond the RHI creation contract.
- Compute PSOs, dispatch, asynchronous compute, asynchronous transfer, or
  multi-queue scheduling.
- Full graphics-state expansion, binding-set semantics, descriptor arrays,
  bindless descriptors, or persistent pipeline-cache policy.
- VMA policy changes, staging/readback arenas, GPU-completion retirement,
  memory budgets, or pressure telemetry.
- The M5 debug messenger, systematic object naming, GPU queries, and complete
  RHI conformance matrix. This plan only makes diagnostic layers and extensions
  optional and explicitly negotiated.
- Device-loss recovery or Renderer resource resubmission.
- Adding a second production backend. Portability is enforced at the public
  contract while Vulkan remains the implemented backend.

## Design Decisions and Invariants

### Capability snapshot ownership

- `FDynamicRHI` publishes one const capability/limit snapshot after `Init()`
  succeeds. Callers never observe a partially populated snapshot and cannot
  mutate it.
- Public fields describe portable rendering decisions, not Vulkan structs,
  extension names, stage/access masks, or native handles.
- Every field must name at least one current or next-milestone consumer and a
  deterministic fallback or rejection path. Unconsumed Vulkan properties stay
  backend-private.
- `ERHIFeatureLevel` may summarize a proven baseline, but it does not replace
  independently queried limits and optional capabilities and cannot advertise
  unsupported enum-comment promises.
- CPU executor serial completion and GPU capability publication remain
  separate concerns. Capability reads are immutable after startup and do not
  require an RHI-thread round trip.

### Texture validity and support are distinct

- `ValidateTextureCreateDesc` owns backend-neutral structural validity:
  dimension-specific depth/layer rules, cube face grouping, legal mip/sample
  combinations, mutually exclusive usage, and checked bounds.
- The capability/support query owns device-dependent support for the complete
  description, including format features, image type, tiling, usage, flags,
  extent, mip count, array layers, and sample counts.
- Structurally invalid descriptions fail at the public RHI boundary with an
  exact diagnostic. Valid but unsupported descriptions return unsupported and
  never reach `vkCreateImage`.
- An enum constructor is not a support promise. 2D arrays, cube arrays, and 3D
  textures become creatable only when the native mapping and focused tests land;
  otherwise the capability query and creation path reject them consistently.

### Vulkan requirement classification

- Instance/device requirements are classified as required runtime,
  platform-required, optional feature, optional diagnostic, or promoted core.
  Support, request, activation, and publication are separate states.
- Missing required requirements reject one startup candidate with an owned
  diagnostic. Missing optional or diagnostic requirements disable the
  associated capability and permit normal startup.
- Validation layers are enabled only by the Stage 0 selected configuration
  policy and only when available. Their absence cannot block a normal
  non-validation runtime.
- Extension and feature chains are assembled from stable candidate-owned
  storage and published only with the successfully created instance/device.

### Device and queue topology

- A physical device must pass every hard requirement before it receives a
  ranking score. A zero or rejected score is never selectable.
- Candidate rejection records device-qualified reasons; total failure reports
  the complete bounded set of candidate reasons rather than only the final
  native exception.
- Queue-family selection is deterministic and distinguishes a required
  graphics/present path from optional compute/transfer opportunities. This
  plan provisions only queues that the synchronous runtime may legally use; it
  does not activate asynchronous scheduling.
- Stage 0 must select one explicit Win64 WSI rule: obtain presentation support
  before logical-device commitment, use a proven platform presentation-support
  query, or require a provisioned graphics/present family with transactional
  viewport rejection. `SetupPresentQueue` may never manufacture a queue from a
  family omitted from logical-device creation.
- Main-window and ImGui detached viewports share the selected supported WSI
  topology unless qualification proves they need distinct constraints.

### Transactional native publication

- Instance, device, queue-owner, render-pass, framebuffer, descriptor-layout,
  pipeline-layout, and pipeline candidates remain local until all required
  native handles and dependent state are complete.
- Constructors do not log-and-continue after a required native creation
  failure. They either produce a complete candidate or return/throw one owned
  failure to the boundary that controls publication.
- Structural caches insert only complete candidates. A failed lookup leaves
  no entry and a retry of the same immutable key can succeed exactly once.
- Debug names label diagnostics only; immutable structural descriptors own
  cache identity.

### Threading and failure behavior

- Initialization and native mutation continue on the established RHI executor
  thread. Immutable capabilities may be read from other threads after startup.
- `RHIInit` remains the owner of backend/module rollback. This plan extends its
  diagnostic precision without adding partial recovery or device-loss
  semantics.
- Expected runtime resource-creation failures remain nullable. Missing required
  startup capability remains an initialization failure; state-contract
  violations remain assertions rather than silent fallbacks.

## Current Foundations and Gaps

| Area | Established foundation | Gap owned by this plan |
| --- | --- | --- |
| CPU execution | Recorded owned batches, dedicated RHI thread by default, inline diagnostic mode, exact serial fences, bounded backpressure, and audited drain. | Preserve the executor contract while publishing immutable post-init data. |
| Startup rollback | Instance, device, and allocator failure injection already unwinds the backend module and clears `GDynamicRHI`. | Classify requirements, reject unsuitable candidates deterministically, and keep all candidate state unpublished until complete. |
| Public creation | Public buffer, texture, sampler, shader, vertex declaration, PSO, and viewport factories generally return complete resources or null. | Distinguish invalid from unsupported texture descriptions and extend the same transaction rule to internal structural caches. |
| Capabilities | Device properties, queue families, extensions, features, and per-format properties are locally queryable. | There is no portable immutable snapshot, field/consumer inventory, or general limit/fallback contract. |
| Extensions and layers | Instance/device extensions are enumerated and supported entries are activated. | Required, optional, diagnostic, promoted, and platform requirements are not distinct; validation is unconditional. |
| Device selection | Devices receive a deterministic numeric preference score. | Hard-rejected devices remain in the selectable map and rejection diagnostics are not candidate-qualified. |
| Queues and WSI | Graphics/compute/transfer families are discovered; viewport/swapchain replacement is transactional. | Present support is discovered after device creation and may select an unprovisioned family. |
| Textures | Public descriptions cover multiple dimensions; 2D/cube validation, Vulkan format features, failure-atomic image/view creation, mip upload, and readback exist. | Dimension validation is incomplete, support ignores full image properties, and native image type is fixed to 2D. |
| Structural caches | Render-pass, framebuffer, descriptor, and pipeline owners have immutable inputs and established call sites. | Render-pass failure can publish a null handle; the directly dependent caches lack one audited complete-candidate rule. |

## Implementation Stages

### Stage 0: Freeze the startup and capability contract

- [ ] Record the required Win64 Editor/Game runtime profiles and explicitly
  disposition the existing Apple portability branches as qualified, compile-
  only, or deferred rather than implying untested parity.
- [ ] Inventory current Renderer and next-milestone consumers of feature level,
  texture limits/dimensions/samples, queue flags, presentation topology,
  synchronization feature choice, and optional diagnostics.
- [ ] Define the minimal public capability fields, exact value domains,
  fallback/rejection behavior, and owning query surface.
- [ ] Classify every currently requested instance layer, instance/device
  extension, promoted feature, and physical-device feature as required,
  platform-required, optional feature, or optional diagnostic.
- [ ] Select the validation enablement/configuration owner and prove that an
  unavailable validation layer does not block the normal mode.
- [ ] Select the Win64 presentation-family discovery/provisioning rule before
  logical-device creation and define behavior for a later incompatible surface.
- [ ] Inventory render-pass, framebuffer, descriptor-layout, pipeline-layout,
  and pipeline candidate construction/publication paths and freeze the bounded
  transactional working set.
- [ ] Record baseline initialization, texture-validation, swapchain, and
  failure-injection coverage and the exact new tests required by later stages.

#### Acceptance Gate

- The platform matrix, capability field/consumer table, Vulkan requirement
  classification, WSI queue rule, cache working set, and validation matrix are
  recorded in this plan with no simultaneous alternative presented as an
  implementation decision. The current initialization rollback and supported
  Win64 hidden-window baseline pass before Stage 1 changes begin.

### Stage 1: Publish portable capabilities and truthful texture support

- [ ] Add the immutable backend-neutral capability/limit snapshot and const
  `FDynamicRHI` query, with complete initialization and test-backend support.
- [ ] Populate only Stage 0 selected fields from Vulkan properties, features,
  limits, formats, and the selected queue topology after device creation.
- [ ] Complete backend-neutral texture validation for 2D, 2D array, 3D, cube,
  and cube array dimension rules, maximum mip counts, sample combinations,
  usage conflicts, checked extent/layer bounds, and cube-face grouping.
- [ ] Replace the narrow format-feature check with one full Vulkan image-support
  query for the exact description and selected capabilities.
- [ ] Map every supported texture dimension to the correct Vulkan image and
  default-view type, or reject it before native creation when the Stage 0
  support boundary deliberately defers that dimension.
- [ ] Make `RHICreateTexture` return an owned unsupported diagnostic/null before
  native allocation while retaining assertions for structurally invalid
  programmer input according to the frozen public policy.
- [ ] Add focused public validation and Vulkan tests covering accepted boundary
  values and rejected dimension/usage/limit combinations.

#### Acceptance Gate

- Callers can read one immutable capability snapshot without a synchronous
  executor call; every published field has a tested Vulkan value and fallback.
  Texture support and creation agree for the same complete description, all
  advertised dimensions are either implemented and sampled or rejected before
  `vkCreateImage`, and existing 2D/cube upload, readback, and texture-asset
  behavior remains unchanged.

### Stage 2: Make instance and diagnostic negotiation explicit

- [ ] Introduce candidate-owned instance negotiation that enumerates the
  available API version, layers, and extensions before deciding activation.
- [ ] Separate support, request, activation, and requirement class; account for
  promoted core functionality without requiring redundant extension names.
- [ ] Enable validation and debug-utils only under the selected diagnostic
  policy and continue without them when they are optional and unavailable.
- [ ] Reject missing platform/runtime requirements before `vkCreateInstance`
  with exact names and classifications; log disabled optional requirements once.
- [ ] Preserve transactional instance publication and module rollback across
  enumeration, negotiation, native creation, and dispatcher initialization.
- [ ] Add deterministic negotiation tests for required-missing,
  optional-missing, validation-requested-present, validation-requested-absent,
  and normal non-validation startup.

#### Acceptance Gate

- The instance enables only classified, supported, requested requirements;
  missing optional diagnostics do not prevent startup, missing required
  requirements fail before native creation with one owned diagnostic, and
  repeated injected initialization failure followed by success leaves no
  retained instance or module state.

### Stage 3: Select and publish one valid device/queue/WSI topology

- [ ] Evaluate every physical device into a local candidate containing hard
  rejection reasons, portable limits/capabilities, extension/feature
  activation, queue-family choices, and a preference score used only after
  requirements pass.
- [ ] Reject zero-suitability and missing-feature candidates before ranking and
  report a bounded device-qualified diagnostic when none pass.
- [ ] Assemble logical-device extensions, feature chains, and queue create
  infos from candidate-owned storage and publish `FVulkanDevice` only after all
  mandatory subobjects initialize successfully.
- [ ] Provision the Stage 0 selected graphics/present topology during logical-
  device creation; make compute/transfer discovery truthful without enabling
  asynchronous use.
- [ ] Constrain `SetupPresentQueue` to a provisioned family and make an
  incompatible main/detached surface a transactional viewport failure instead
  of constructing an invalid queue wrapper.
- [ ] Publish the final portable snapshot from the selected device and verify
  that failed candidates cannot leak values into it.
- [ ] Add selection/queue tests plus supported main-window and ImGui detached-
  viewport creation, replacement, and teardown qualification.

#### Acceptance Gate

- Startup never publishes an unsuitable device, unavailable feature, or
  unprovisioned queue family. All-candidate failure names why each device was
  rejected; a later surface either uses the declared topology or fails the
  viewport transaction without disturbing an existing valid viewport.

### Stage 4: Enforce complete structural-cache candidates

- [ ] Change render-pass creation to return/throw a failed candidate instead of
  logging and retaining a null handle.
- [ ] Add a render-pass native failure-injection point and prove same-key retry
  succeeds after one failed candidate without a poisoned cache entry.
- [ ] Audit the Stage 0 framebuffer, descriptor-layout, pipeline-layout, and
  pipeline working set for constructor log-and-continue, partial member
  publication, or cache insertion before dependent native handles are complete.
- [ ] Refactor each confirmed gap to build local complete candidates and insert
  only after all native creation and immutable identity checks succeed.
- [ ] Propagate structural failure to the owning public nullable factory or
  terminal command-state boundary without converting debug names into keys.
- [ ] Add focused retry, identity, cleanup, and dependent-candidate tests for
  every changed cache.

#### Acceptance Gate

- No audited structural cache contains a null or partial native object; each
  injected creation failure releases candidate resources, leaves cache size and
  lookup behavior unchanged, reports one owned diagnostic, and permits the same
  immutable key to succeed on retry.

### Stage 5: Qualify startup and publish the lasting contract

- [ ] Run the focused public-RHI, initialization, Vulkan failure-injection,
  texture, pipeline, swapchain, viewport, and inline/threaded execution suites
  selected by the validation matrix.
- [ ] Run both diagnostic-enabled and normal startup where available and record
  the optional-unavailable simulation evidence.
- [ ] Qualify hidden main-window startup/shutdown and detached-viewport WSI
  creation/replacement/teardown on the supported Win64 profile with validation
  clean.
- [ ] Run the complete native aggregate, a full `all` build, and repeated
  `DurinEditor --hidden-window` startup/normal-shutdown smoke through
  DurinDevTool.
- [ ] Record the stable capability, texture-support, startup negotiation,
  device/queue, WSI, and complete-candidate contracts under
  `Documentation/Runtime/Rendering/` and link them from related contracts.
- [ ] Update both this plan and the owning roadmap with completion evidence,
  downstream M1/M2 entry-gate effects, final working set, key symbols and
  decisions, open questions, and the validation outcome.

#### Acceptance Gate

- Required and optional startup permutations, exact texture support, valid
  device/queue publication, structural-cache retry, supported WSI, inline and
  threaded execution, full native validation, full build, repeated editor
  startup, and orderly shutdown all pass. Lasting contracts no longer depend on
  this plan as their only specification.

## Validation Matrix

| Contract | Focused validation | Required outcome |
| --- | --- | --- |
| Capability publication | Public RHI/fake backend tests; Vulkan property/limit checks; initialization failure followed by success | Snapshot is unavailable before successful init, immutable afterward, complete for the selected backend, and never retains a failed candidate. |
| Texture structural validity | RHI resource tests across every dimension, extent, depth, layer, mip, sample, format, and usage edge | Invalid descriptions produce exact stable diagnostics without backend calls. |
| Vulkan texture support | Image-format-property queries plus real create/upload/sample/readback for supported descriptions | Support query and creation agree; unsupported valid descriptions reach no native allocation. |
| Instance negotiation | Available/missing required and optional layers/extensions; validation requested/unrequested/unavailable | Required absence fails early; optional diagnostic absence starts normally; activation reports exactly what was selected. |
| Device selection | Synthetic candidate evaluator tests and hardware-backed startup | Hard-rejected/score-zero devices are never ranked; all-candidate failure is device-qualified; selected capabilities match the published device. |
| Queue and WSI topology | Queue-family policy tests; main and detached viewport create/resize/destroy | Every used family was provisioned at device creation; incompatible surfaces fail transactionally; supported topology presents and tears down cleanly. |
| Structural caches | Render-pass and directly dependent cache failure injection, cleanup, same-key retry, identity tests | Failed candidates are absent, complete retry succeeds, and no null native handle reaches framebuffer/pipeline/command recording. |
| Execution parity | Existing inline/threaded RHI and Vulkan integration suites | Capability reads add no executor round trip and backend mutation retains RHI-thread affinity. |
| Handoff qualification | Native aggregate, full `all` build, repeated hidden editor startup/shutdown | Zero unexpected failures, validation errors, retained backend/module state, or shutdown leaks. |

## Definition of Done

- Stage 0 records one selected contract rather than unresolved alternative
  implementations, and Stages 1 through 5 pass their acceptance gates.
- One immutable portable capability/limit snapshot is the authoritative public
  source for selected RHI paths.
- Vulkan instance/device negotiation distinguishes required, optional,
  diagnostic, platform, and promoted requirements and publishes no partial
  candidate.
- Unsuitable devices and unprovisioned queue topologies cannot become the
  active backend; supported main/detached WSI behavior is explicit and tested.
- Texture validation, support queries, and native creation agree for the full
  public description; unsupported valid work fails before native creation.
- Render-pass and every other audited structural cache publish complete
  candidates only and recover from same-key injected failure.
- Stable contracts are documented under `Documentation/Runtime/Rendering/`;
  this plan and the owning roadmap record completion evidence and downstream
  gate status.
- The required focused suites, native aggregate, full `all` build, and repeated
  hidden editor startup/shutdown validation pass through the repository
  workflow.

## Deferred Follow-ups

- General access-state transitions and synchronization feature selection remain
  `GPUResourceTransitions` work, consuming the capability snapshot added here.
- Resource views/transfers, graphics binding expansion, GPU-completion memory
  retirement, and M5 diagnostics/conformance remain their owning RHI roadmap
  milestones.
- Persistent driver pipeline cache, dynamic rendering, bindless descriptors,
  Render Graph, multi-queue execution, and device recovery remain evidence- or
  product-gated.
- Additional platform runtime qualification must add a named profile, WSI
  topology, feature/extension policy, and validation evidence; compile-time
  branches alone do not expand the supported platform set.

## Related Documentation

- [RHI and Vulkan Backend Evolution Roadmap](../Roadmaps/RHIAndVulkanEvolution.md)
- [Compute Shader Pipeline Roadmap](../Roadmaps/ComputeShaderPipeline.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Dedicated RHI Thread](Archive/2026-08/DedicatedRHIThread.md)
- [Recorded RHI Command List](Archive/2026-08/RecordedRHICommandList.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHIFeatureLevel.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Public/VulkanDynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanExtension.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderPass.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanFramebuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDescriptorSets.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapchain.cpp`
- `Engine/Tests/Native/RHITests/Private/RHIInitializationTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RHITextureTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
