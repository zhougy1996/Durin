# RHI and Vulkan Correctness Hardening Plan

Summary: Close audited PSO identity, Vulkan descriptor-layout, mapped-memory, and per-surface swapchain negotiation gaps without broadening the RHI feature set.

Last reviewed: 2026-08-11

Status: Active
Completed:

## Current Status

The completed RHI and Vulkan roadmap established recorded command ownership,
explicit resource transitions and views, bounded graphics caches, exact GPU
completion, transactional swapchain replacement, and hardware-backed
conformance coverage. A focused source audit on 2026-08-11 found four narrower
correctness gaps that do not require a new public rendering feature:

- sampled descriptors derive image layout from a texture's creation flags
  instead of the binding/view usage and authoritative tracked access;
- mapped-memory flush and invalidate results are discarded;
- swapchain creation assumes nonempty formats and hard-codes image usage and
  composite alpha without validating the current surface capabilities; and
- PSO key equality treats signed zero numerically while its hash preserves the
  different float bit patterns.

The pre-change baseline passes 58 `RHICommandListTests` cases and 45
`VulkanRHIIntegrationTests` cases. Those suites do not yet cover the audited
dual-use sampled/storage texture, mapped-memory failure, limited surface
capability, or signed-zero key scenarios.

This plan keeps startup negotiation and per-surface negotiation distinct.
Instance version, extensions, device features, and the provisioned queue family
remain startup-owned immutable decisions. Surface formats, present modes,
extent, image counts, supported image usages, transforms, and composite-alpha
support are queried and selected for every transactional swapchain candidate,
because they belong to a concrete surface and may change across recreation.

## Goal

Make every accepted graphics key, shader image binding, mapped-memory
synchronization operation, and swapchain candidate obey one explicit identity,
layout, failure, and capability contract, with deterministic focused tests for
the newly covered boundary conditions.

## Scope

- Equality-consistent canonicalization of floating-point PSO key fields.
- Binding-driven Vulkan descriptor image layouts and draw-time validation
  against the authoritative texture state tracker.
- Explicit handling of `vmaFlushAllocation` and `vmaInvalidateAllocation`
  failures through the existing terminal Vulkan execution-failure policy.
- A pure, testable swapchain-selection boundary covering formats, present
  modes, extent, image count, required image usages, transform, and composite
  alpha for one current surface-capability snapshot.
- Per-candidate surface queries for main and detached viewports, preserving
  transactional publication and the existing unavailable-output recovery path.
- Focused RHI and Vulkan native tests, validation-clean runtime qualification,
  lasting-contract updates, and the required full editor build.

## Non-Goals

- Moving surface-specific or recreation-sensitive values into the immutable
  startup `FRHICapabilities` snapshot.
- Changing physical-device ranking, instance/device extension negotiation,
  queue-family provisioning, or the supported synchronous graphics/present
  topology.
- Adding a new swapchain-maintenance fallback, present-wait/present-id path,
  cross-viewport scheduling policy, or eliminating the existing queue-idle
  fallback when swapchain maintenance is unavailable.
- Weakening the current backbuffer contract by silently dropping required
  sampled, color-attachment, or transfer-destination usage.
- Device-loss recovery or making submission, presentation, mapped-memory
  synchronization, or state-contract failures recoverable.
- A public or Core-wide generic floating-point hashing framework.
- Unrelated RHI API redesign, render graph, dynamic rendering, bindless
  descriptors, multi-queue execution, or cache-policy tuning.

## Design Decisions and Invariants

### Startup capability versus surface-candidate ownership

- Vulkan loader/API version, instance and device extensions, enabled features,
  physical-device limits, and the provisioned graphics/present queue family
  remain negotiated once during startup and published only after the device is
  complete.
- `vkGetPhysicalDeviceSurfaceCapabilitiesKHR`, surface formats, and surface
  present modes remain candidate-local queries. They are evaluated every time
  `FVulkanViewport` attempts to create or recreate a swapchain, including for
  detached editor surfaces created after startup.
- A pure selection function consumes copied query results plus viewport policy
  and returns either one complete native swapchain configuration or one owned
  diagnostic. It performs no Vulkan mutation and is directly unit-testable.
- Empty format or present-mode results reject the candidate before indexing or
  native creation. FIFO remains the preferred main-window mode, but its
  availability is still represented by the queried input rather than assumed
  by unsafe container access.
- Color-attachment, sampled, and transfer-destination usages are required by
  the existing backbuffer contract. If the surface cannot support all required
  bits, the candidate fails explicitly; it does not publish a backbuffer whose
  declared RHI flags exceed its native usage.
- Composite alpha is selected deterministically from supported bits, preferring
  opaque and then a frozen fallback order recorded by Stage 0. The selected
  mode is diagnostic state, not public RHI capability state.
- Transactional ownership is unchanged: the previous complete swapchain stays
  published until the entire new swapchain, image-view, semaphore, and present-
  fence candidate succeeds.

### Descriptor layout and tracked state

- Descriptor image layout is determined by the descriptor binding/view usage,
  not by all capabilities with which the parent texture was created.
- A sampled `Texture` binding uses `vk::ImageLayout::eShaderReadOnlyOptimal`;
  a `StorageImage` binding uses `vk::ImageLayout::eGeneral`.
- Before descriptor reuse or draw emission, every bound texture-view range is
  validated against the authoritative `FVulkanTextureStateTracker`: sampled
  ranges require the portable shader-read state selected by the graphics
  contract, while storage ranges require graphics shader read/write state.
- A texture created with both `ShaderResource` and `Storage` remains legal and
  can alternate usages only through explicit matching transitions. Creation
  flags never substitute for current access state.
- Descriptor cache identity remains resource/view identity plus binding values;
  layout does not become a second divergent state authority.

### Mapped-memory failure policy

- `vmaFlushAllocation` and `vmaInvalidateAllocation` results are checked once
  at the memory-manager boundary and include allocation class, requested range,
  normalized range, and Vulkan result in the diagnostic.
- Failure is terminal for the current RHI execution path. Upload, dynamic data,
  and readback never continue as though visibility or availability succeeded,
  and no nullable resource-creation result is synthesized.
- Existing non-coherent atom-size normalization and coherent-memory behavior
  remain unchanged; result handling does not add a device-wide wait.

### PSO floating-point identity

- `FGraphicsPipelineStateKey::operator==` retains ordinary finite floating-
  point value equality. Since key construction already rejects non-finite
  values, signed zero is the only accepted representation mismatch that must be
  canonicalized for equality-consistent hashing.
- Every active floating-point key field is canonicalized from either signed
  zero to positive zero while building the key, before insertion or lookup.
  Disabled depth-bias fields continue to canonicalize to their existing
  defaults.
- Canonicalization remains private to RHI graphics-key construction. It is not
  extracted into Core until a second consumer requires the same numeric-
  equality hashing contract; serialization, derived-data, reflection, and
  bitwise-identity users intentionally have different float semantics.
- `FGraphicsPipelineStateKeyHasher` continues to hash canonical key values and
  must satisfy: equal accepted keys always produce equal hashes.

## Current Foundations and Gaps

| Area | Existing foundation | Gap closed by this plan |
| --- | --- | --- |
| PSO identity | Complete canonical graphics key and bounded Vulkan PSO cache | Signed-zero values can compare equal but hash differently |
| Resource state | Explicit portable transitions and per-subresource Vulkan tracker | Descriptor layout is selected from creation flags and descriptor use is not checked against tracked texture access |
| Host-visible memory | Persistent mappings, atom-aligned ranges, explicit upload flush and readback invalidate | VMA synchronization return values are ignored |
| Startup | Explicit instance/device/queue negotiation and immutable public capabilities | Surface-specific dynamic values must not be mistaken for startup invariants |
| Swapchain replacement | Candidate-local construction, failure rollback, old-swapchain handoff, unavailable-output recovery | Current surface usages, alpha support, and empty query results are not completely selected before native creation |
| Tests | RHI command/transition suites and Vulkan failure, texture, memory, WSI, and conformance coverage | No deterministic coverage for the four audited boundary cases |

## Implementation Stages

### Stage 0: Freeze boundary contracts and deterministic fixtures

Outcome: the exact required swapchain usages, composite-alpha preference,
descriptor access expectations, VMA failure policy, and signed-zero identity
are executable as pure or injected tests before production paths change.

Dependencies: completed RHI capability, transition, view, graphics-binding,
memory-completion, and diagnostics contracts.

- [ ] Inventory every current backbuffer consumer that requires color-
  attachment, sampled, and transfer-destination usage; confirm all three remain
  required rather than silently selecting a reduced native usage.
- [ ] Freeze the deterministic composite-alpha preference order and the owned
  diagnostics for empty formats, empty present modes, missing required usage,
  and missing supported alpha modes.
- [ ] Define one value-only swapchain selection input/output that contains no
  borrowed Vulkan query pointers and can represent main/detached policy.
- [ ] Add table-driven selection fixtures for fixed/variable extent, image
  count clamps, preferred/fallback format and present mode, each alpha fallback,
  empty query results, and missing required usage bits.
- [ ] Add a legal dual-use texture fixture that transitions between graphics
  shader read/write and graphics shader read before storage and sampled binds.
- [ ] Select narrow test-only failure injection for VMA flush and invalidate
  result handling without changing normal allocator behavior or making the
  public operation recoverable.
- [ ] Add RHI graphics-key fixtures proving `+0.0f` and `-0.0f` accepted state
  produces equal keys and equal hashes while distinct nonzero finite state
  remains distinct.

#### Acceptance Gate

- Startup-owned and per-surface candidate-owned capabilities are listed once
  with no field assigned to both authorities.
- Required swapchain usage and alpha selection have one deterministic outcome
  for every fixture, including complete rejection before native creation.
- Descriptor binding usage, tracked access, VMA failure, and signed-zero key
  behavior each have a failing pre-change test or an equivalent isolated
  deterministic reproducer.
- No public RHI API, immutable capability field, generic Core float helper, or
  swapchain-maintenance fallback is required by the selected design.

### Stage 1: Make PSO identity equality-consistent

Outcome: every accepted graphics PSO key has a canonical finite float
representation consistent with its equality operator and cache hasher.

Dependencies: Stage 0 signed-zero fixtures and existing graphics-state
canonicalization.

- [ ] Add a private graphics-key float canonicalization helper or equivalent
  explicit assignments in `BuildGraphicsPipelineStateKey`; normalize signed
  zero without changing nonzero finite values.
- [ ] Apply it to every active rasterizer float field after disabled-state
  canonicalization and before publishing `OutKey`.
- [ ] Keep hashing centralized in `FGraphicsPipelineStateKeyHasher` and avoid a
  second bitwise or public hash identity.
- [ ] Cover equality/hash parity and Vulkan PSO cache reuse for signed-zero
  depth-bias variants where hardware-backed coverage is useful.
- [ ] Run the smallest affected RHI graphics-state test target and focused
  Vulkan pipeline/cache coverage under repository guidance.

#### Acceptance Gate

- For every accepted key pair `A == B`, the focused signed-zero cases prove
  `Hasher(A) == Hasher(B)`.
- Positive and negative nonzero values remain distinct and produce their
  expected native pipeline state.
- The implementation remains RHI-private and introduces no generic hashing API
  or behavior change for serialization and derived-data identities.

### Stage 2: Align descriptors and mapped memory with authoritative state

Outcome: Vulkan descriptors declare the layout selected by binding usage, draw
validation proves the exact view range is in matching tracked access, and
mapped-memory synchronization cannot fail silently.

Dependencies: Stage 0 dual-use texture and VMA failure fixtures.

- [ ] Select sampled versus storage descriptor image layout exclusively from
  `ERHIBindingType` or the already validated texture-view usage.
- [ ] Validate every texture descriptor's exact view subresource range against
  `FVulkanTextureStateTracker` before descriptor cache hit reuse or draw
  emission, with binding-qualified diagnostics.
- [ ] Preserve descriptor cache ownership, bounds, dynamic offsets, and exact
  resource/view identity; do not add a parallel layout cache.
- [ ] Cover one dual-use texture across storage write, explicit transition,
  sampled read, reverse transition, descriptor cache miss/hit, and both inline
  and threaded RHI execution.
- [ ] Check and classify VMA flush/invalidate results at the memory-manager
  boundary and propagate failure through the existing terminal executor path.
- [ ] Add injected upload/dynamic-data flush and readback invalidate failures,
  proving no later copy, draw, result publication, or resource leak occurs.
- [ ] Run the smallest affected RHI transition/view and Vulkan integration test
  targets under repository guidance.

#### Acceptance Gate

- Sampled descriptors always match shader-read-only tracked layout and storage
  descriptors always match general read/write tracked layout for their exact
  view ranges.
- Dual-use textures switch legally only through explicit transitions and pass
  validation in inline and threaded modes, including descriptor cache reuse.
- Flush/invalidate failure produces one owned terminal diagnostic and cannot
  expose submitted stale writes or successful stale readback.
- No device-wide wait, nullable creation result, or second resource-state owner
  is introduced.

### Stage 3: Complete per-surface swapchain candidate negotiation

Outcome: every swapchain candidate is completely selected and validated from a
fresh surface snapshot before `vkCreateSwapchainKHR`, while replacement and
recovery behavior remain transactional.

Dependencies: Stage 0 selection contract and current startup-provisioned queue
family.

- [ ] Extract or introduce the pure swapchain configuration selector and make
  the native constructor consume only its complete result.
- [ ] Query capabilities, formats, and present modes for every create/recreate
  attempt after validating the provisioned queue family's support for that
  concrete surface.
- [ ] Reject empty query results, missing required image usage, unsupported
  extent/image-count combinations, and absent composite-alpha choices with one
  candidate-owned diagnostic before native swapchain creation.
- [ ] Select and publish actual format, present mode, extent, image count,
  transform, image usage, and composite alpha from the same immutable candidate
  value used to fill `vk::SwapchainCreateInfoKHR`.
- [ ] Preserve old-swapchain handoff, candidate cleanup, retry eligibility,
  stable backbuffer wrapper/generation, and main/detached viewport isolation.
- [ ] Extend failure and WSI coverage for limited capability inputs, initial
  failure, recreation failure before/after native creation, recovery, resize,
  detached viewport teardown, and validation-clean presentation.
- [ ] Do not change the existing queue-idle fallback when swapchain maintenance
  is unavailable; record it only as an unchanged non-goal.

#### Acceptance Gate

- No surface format is indexed and no swapchain native call is made until a
  complete supported candidate exists.
- Every requested `imageUsage` bit and the selected `compositeAlpha` bit are
  proven supported by the same queried capability snapshot.
- Main and detached swapchain creation/recreation preserve the previous output
  or enter the existing unavailable-output state without disturbing another
  viewport.
- The implementation does not cache dynamic surface capabilities in startup
  state and adds no maintenance/present-wait fallback work.

### Stage 4: Qualify and publish the hardened contracts

Outcome: the four corrections are regression-qualified, documented in their
lasting owner documents, and ready for archival as one bounded maintenance
plan.

Dependencies: Stages 1-3 acceptance gates.

- [ ] Update the RHI graphics-state/binding, Vulkan memory/completion, startup
  capability, and viewport/presentation lasting documents with only the new
  invariants owned by each domain.
- [ ] Update this plan's Current Status, Last reviewed date, checklists, and
  evidence as each stage lands; record any design deviation before continuing.
- [ ] Run documentation validation and the smallest affected RHI and Vulkan
  native targets under repository guidance.
- [ ] Run the successful full `all` build required for the user-visible
  swapchain change.
- [ ] Launch the editor from the same Agent Build Profile and qualify normal
  main-window plus detached-viewport creation, drawing, resize, and shutdown
  with Vulkan validation enabled.
- [ ] Review implementation, tests, lasting documentation, and plan evidence
  before marking the plan Completed.

#### Acceptance Gate

- Focused RHI/Vulkan tests, documentation validation, full `all` build, and
  validation-enabled editor runtime qualification pass from the required
  profiles.
- Signed-zero PSO identity, descriptor layout/state, mapped-memory failure, and
  per-surface swapchain negotiation each have deterministic retained coverage.
- Lasting documents agree with implementation on ownership, failure, ordering,
  and capability boundaries, and this active plan contains no competing
  runtime contract.
- A verified editor executable from the same Agent Build Profile is available
  for handoff.

## Validation Matrix

| Contract | Focused coverage | Required outcome |
| --- | --- | --- |
| PSO numeric identity | RHI graphics-key tests | Signed-zero-equivalent accepted keys compare and hash equally; nonzero finite differences remain distinct |
| PSO cache behavior | Vulkan pipeline/cache tests | Equivalent signed-zero initializers reuse one complete cache entry |
| Sampled descriptor layout | Dual-use texture integration test | Sampled view declares shader-read-only layout and exact tracked range is graphics shader read |
| Storage descriptor layout | Dual-use texture integration test | Storage view declares general layout and exact tracked range is graphics shader read/write |
| Descriptor reuse | Inline/threaded cache hit/miss sequence | Cache reuse preserves identical layout/state validation and resource lifetime |
| Upload visibility | Injected VMA flush failure | Failure is terminal before dependent copy/draw and reports normalized allocation/range context |
| Readback availability | Injected VMA invalidate failure | No successful output is published from stale mapped data and arena resources remain bounded |
| Surface query ownership | Pure selector and WSI tests | Fresh per-candidate capabilities/formats/modes drive every creation; startup state contains only immutable device/topology decisions |
| Surface containers | Empty and fallback selector tables | Empty formats/modes reject deterministically without indexing or native creation |
| Swapchain usage | Limited supported-usage tables | All required image-usage bits are supported or the candidate is rejected intact |
| Composite alpha | Each supported-bit combination | Deterministic supported selection, preferring opaque, with no unsupported hard-coded mode |
| Transactional replacement | Initial/recreate failure injection | Old complete output remains until full candidate publication; post-native incomplete candidate enters existing unavailable/retry policy |
| Supported WSI topology | Main/detached create, present, resize, teardown | Viewports remain isolated and validation-clean; maintenance-unavailable fallback is unchanged |
| Final qualification | Required native targets, full build, runtime smoke | No regression in RHI execution, resource state/lifetime, editor output, or shutdown |

## Definition of Done

- All Stage 0-4 checklist items and acceptance gates are complete with recorded
  evidence.
- Equal accepted PSO keys always hash equally, including every finite signed-
  zero rasterizer case, without a public generic float-hash API.
- Sampled and storage descriptors use binding-correct layouts and validate the
  exact view range against the authoritative texture state before draw.
- Mapped-memory flush and invalidate failures cannot be ignored or converted
  into successful uploads/readbacks.
- Every swapchain create/recreate uses a fresh surface snapshot and publishes
  only a complete configuration whose formats, modes, usages, alpha, extent,
  count, and transform are supported by that snapshot.
- Startup capabilities remain immutable device/topology facts; dynamic surface
  values remain local to transactional viewport candidates.
- Main and detached viewport behavior, old-candidate preservation, unavailable
  output, retry, and shutdown remain validation-clean without new maintenance
  fallback complexity.
- Focused tests, documentation validation, required full build, and editor
  runtime qualification pass, and lasting contracts are updated before the
  plan is marked Completed.

## Deferred Follow-ups

- Replacing queue-idle fallback with additional swapchain maintenance,
  present-wait, or present-id behavior requires separate measured multi-window
  latency evidence and is not activated by this plan.
- Extracting numeric-equality float canonicalization/hashing into Core requires
  a second concrete consumer with the same semantics; bitwise, serialization,
  and derived-data identities do not qualify automatically.
- Device-loss recovery, reduced backbuffer usage variants, transparent-window
  composition policy, and cross-family presentation remain separately gated
  product or architecture work.

## Related Documentation

- [RHI Capabilities and Vulkan Startup](../Runtime/Rendering/RHICapabilitiesAndVulkanStartup.md)
- [RHI Resource Transitions](../Runtime/Rendering/RHIResourceTransitions.md)
- [RHI Resource Views and Transfers](../Runtime/Rendering/RHIResourceViewsAndTransfers.md)
- [Vulkan Memory and GPU Completion](../Runtime/Rendering/VulkanMemoryAndGPUCompletion.md)
- [RHI Diagnostics and Conformance](../Runtime/Rendering/RHIDiagnosticsAndConformance.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [RHI and Vulkan Backend Evolution Roadmap](../Roadmaps/RHIAndVulkanEvolution.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPendingState.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResourceState.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResourceState.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapchain.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanSwapchain.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanViewport.cpp`
- `Engine/Tests/Native/RHITests/Private/RHICommandListTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanMemoryPolicyTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanResourceTransitionTests.cpp`
