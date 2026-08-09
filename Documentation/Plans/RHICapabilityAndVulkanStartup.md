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

Stage 0 completed its contract freeze against baseline commit
`9a13a105aacc08fef1baa86d20814da3875d5df8`. The supported profiles, public
capability vocabulary, exact texture boundary, Vulkan requirement classes,
Win64 queue/WSI rule, structural-cache working set, and validation additions
are recorded below. The existing focused suites, full Debug Editor build, and
hidden-window startup/shutdown baseline passed on 2026-08-09. Stage 1 is now
the current implementation stage.

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

## Stage 0 Frozen Contract

### Supported platform and runtime profiles

| Platform/profile | Required use in this plan | Diagnostic default | WSI qualification |
| --- | --- | --- | --- |
| Win64 `Win64-Debug-DurinEditor-Tests` | Primary implementation, focused native tests, full `all` build, and Editor smoke | `auto` requests optional validation diagnostics | Hidden main window plus visible main-window and ImGui detached-viewport create, replace, and teardown |
| Win64 `Win64-Release-DurinEditor-Tests` | Normal Editor startup and configuration-parity tests | `auto` disables validation diagnostics | Main window and ImGui detached viewports |
| Win64 `Win64-Shipping-DurinGame-Tests` | Shipping contract and Game runtime qualification | Validation diagnostics are disabled | Main game window only; the Editor-only detached-viewport path is not linked into this profile |
| Apple source branches | Deferred; they are neither compile-qualified nor runtime-qualified by this plan | Not applicable | Existing portability-enumeration, MoltenVK surface, and portability-subset branches remain source-only compatibility intent and do not advertise platform parity |

Only the three named Win64 profiles expand the supported runtime matrix. A
future platform must add its own API floor, loader, required extension, queue,
surface, texture-format, diagnostic, build, test, and runtime evidence before
becoming supported.

`FVulkanDynamicRHI` owns diagnostic-policy resolution. The new
`DURIN_VULKAN_VALIDATION` process setting has the exact values `auto`, `on`,
and `off`; an unset or invalid value resolves to `auto`, with an invalid value
logged once. `auto` requests validation only in Debug. `on` requests it in
Debug or Release, while Shipping always disables it. `off` never requests it.
Requesting diagnostics makes the Khronos validation layer and debug-utils
extension optional requests, not runtime requirements. Either may be absent
independently; startup continues and reports the disabled diagnostic once.

### Consumer inventory and public query surface

| Area | Current or next consumer | Selected contract | Fallback or rejection |
| --- | --- | --- | --- |
| Feature level | No current Renderer branch; later graphics-state work needs a coarse baseline | Publish only `ERHIFeatureLevel::ES3_1` for M0 and narrow its comment to a portable graphics baseline; it does not promise compute, typed views, cube arrays, tessellation, or SM5 features | Reject startup if the selected Vulkan device cannot meet the independently listed M0 hard requirements; never infer optional features from the tier |
| 2D and cube textures | Texture2D/TextureCube assets, default textures, environment lighting, render targets, previews, thumbnails, and post process | Publish the supported-dimension mask and 2D/cube limits; use the exact-description support query before native creation | Asset resources retain their typed unsupported diagnostic and Renderer candidate owners retain the last complete payload or fail without publication |
| 2D arrays, 3D textures, and cube arrays | No current production consumer; M2 may select one after typed-view requirements are known | Structurally validate them, but leave their M0 dimension bits clear and return unsupported before `vkCreateImage` | No implicit flattening to 2D; creation returns null with an owned unsupported diagnostic |
| Texture samples | Render-target layout, current MSAA pipeline coverage, and later Renderer quality selection | Publish conservative color and depth sample masks; the exact texture query remains authoritative for format and usage | Select the greatest common requested count supported by every attachment, falling back to one, or reject an immutable caller-selected description |
| Queue flags | Vulkan immediate graphics work and the existing compute-backed texture validation; asynchronous scheduling is deferred | Require one provisioned queue family with graphics and compute flags; transfer support is implicit for that family. Publish no async-compute or async-transfer field | Reject the physical-device candidate; do not create or advertise a separate queue path |
| Presentation topology | Main `MWindow` output and ImGui detached windows | Keep topology backend-private and require the same provisioned graphics/present family for every Win64 surface | A later incompatible surface fails only its viewport transaction; it cannot replace a queue or disturb an existing viewport |
| Synchronization choice | M1 `GPUResourceTransitions` | Publish `bSupportsSynchronization2` only when the core/extension feature is supported and activated | M1 uses the existing legacy barrier path when false |
| Optional diagnostics | M0 startup logs and M5 diagnostics/conformance | Keep activation state backend-private in M0; no public capability field exists without a rendering-path consumer | Missing layer/debug-utils disables that diagnostic and never blocks startup |

Stage 1 adds `FRHICapabilities` and
`FDynamicRHI::RHIGetCapabilities() const -> const FRHICapabilities*`.
`FDynamicRHI` owns one optional snapshot, exposes only a const pointer, returns
null until a backend publishes one complete value, and clears it during
shutdown. `GDynamicRHI` remains unpublished until `Init()` succeeds. The
Vulkan backend constructs the snapshot in the selected device candidate and
publishes it once after device initialization; the fake backend uses the same
protected publication path.

| Public field | Exact domain and source | Consumer/fallback |
| --- | --- | --- |
| `FeatureLevel` | `ERHIFeatureLevel`; exactly `ES3_1` in M0 after all hard requirements pass | Coarse later Renderer selection; every optional path still checks its own field |
| `SupportedTextureDimensions` | `ERHITextureDimensionFlags`; M0 publishes exactly `Texture2D | TextureCube` | Coarse asset/Renderer selection; the exact description query decides final support |
| `MaxTextureDimension2D` | Positive `uint32` from `maxImageDimension2D` | Validate 2D extents; oversized valid descriptions are unsupported |
| `MaxTextureDimensionCube` | Positive `uint32` from `maxImageDimensionCube` | Validate cube extents; oversized valid descriptions are unsupported |
| `MaxTextureArrayLayers` | Positive `uint32` from `maxImageArrayLayers`, with six layers required by the M0 baseline | Bound cube layers now and future array descriptions without implying array-dimension support |
| `ColorSampleCounts` | Nonempty `ERHISampleCountFlags` over `{1,2,4,8,16}`, conservatively intersecting framebuffer-color and sampled-color device masks | Renderer sample fallback; exact format/usage support may further reduce it |
| `DepthSampleCounts` | Nonempty `ERHISampleCountFlags` over `{1,2,4,8,16}`, conservatively intersecting framebuffer-depth and sampled-depth device masks | Depth/color attachment intersection; exact support may further reduce it |
| `bSupportsSynchronization2` | `bool`; true only for activated Vulkan 1.3 core functionality or activated `VK_KHR_synchronization2` plus its feature bit | M1 selects synchronization2 when true and legacy barriers when false |

No public API version, Vulkan extension name, native queue flag, driver limit
without a consumer, presentation-family index, validation state, or native
handle is part of the snapshot. Maximum mip count is derived from the exact
dimension and extent rather than published as a misleading device-wide value.

The existing format-named query becomes
`RHIIsTextureSupported(const FRHITextureCreateDesc&)`. Both it and
`RHICreateTexture` require a structurally valid description. The query returns
only device support; creation asserts an invalid programmer description, logs
one owned diagnostic for a valid unsupported description, returns null, and
does not enter the native allocator.

### Texture validity and exact Vulkan support

`ValidateTextureCreateDesc` applies these backend-neutral rules in a stable
order so each rejected input has one deterministic first diagnostic:

- extent, depth, array size, mip count, sample count, and format must be
  nonzero/specified; sample count must be one of `1, 2, 4, 8, 16`;
- 2D requires depth one and array size one; 2D array requires depth one and at
  least one layer; 3D requires array size one; cube requires square extent,
  depth one, exactly six layers, and one sample; cube array requires square
  extent, depth one, a positive layer count divisible by six, and one sample;
- mip count cannot exceed
  `floor(log2(max(width, height, depth-for-3D))) + 1`; multisampled textures
  have exactly one mip, and 3D/cube/cube-array textures are single-sampled;
- depth/stencil target usage is mutually exclusive with color-render-target,
  resolve-target, and storage usage; storage and CPU-readback textures are
  single-sampled; resolve targets are single-sampled; and checked arithmetic
  is used for mip/layer/subresource counts; and
- structural validity does not apply device limits. In M0, 2D array, 3D, and
  cube array descriptions can be structurally valid but are deliberately
  unsupported by the Vulkan backend.

For a structurally valid and M0-enabled dimension, Vulkan builds the exact
`vk::ImageCreateInfo` mapping and queries image-format properties with its
format, image type, optimal tiling, usage, and create flags. Support requires
the requested extent, mip levels, array layers, sample count, and resource size
to fit the returned properties. Image and default-view mapping is 2D/e2D for
2D, and 2D plus cube-compatible/eCube for cube. The other dimensions are
rejected before the image failure-injection point, which proves they never
reach `vkCreateImage` until a later plan deliberately enables and samples them.

### Vulkan requirement classification

The negotiated instance API version is the highest version no greater than
Vulkan 1.3 reported by the loader. Loader or selected-device API below Vulkan
1.1 is a required-runtime failure. A promoted-core requirement is satisfied by
that core version and its extension name is not redundantly requested.

| Scope and requirement | Class | Request/activation rule | Missing behavior |
| --- | --- | --- | --- |
| Loader and device API >= Vulkan 1.1 | Required runtime | Enumerate before instance/device commitment; request at most 1.3 | Fail the boundary or reject the device candidate with the reported version |
| `VK_KHR_surface` | Platform-required on supported Win64 profiles | Required by GLFW WSI and deduplicated with application requirements | Fail before `vkCreateInstance` with the exact name/class |
| `VK_KHR_win32_surface` | Platform-required on supported Win64 profiles | Supplied by GLFW and needed for the pre-device Win32 present query | Fail before `vkCreateInstance` with the exact name/class |
| `VK_KHR_get_surface_capabilities2` | Optional feature | Activate only as a dependency of surface/swapchain maintenance | Disable swapchain maintenance |
| `VK_EXT_surface_maintenance1` | Optional feature | Activate only with its supported dependency chain | Disable swapchain maintenance |
| `VK_EXT_debug_utils` | Optional diagnostic | Request only when the resolved validation policy requests diagnostics | Log disabled diagnostic once and continue |
| `VK_KHR_get_physical_device_properties2` | Promoted core in Vulkan 1.1 | Use core functionality; do not request the extension name | Covered by the Vulkan 1.1 floor |
| `VK_KHR_portability_enumeration` and `VK_MVK_macos_surface` | Deferred Apple platform requirements | Not requested by a supported profile | No Win64 effect and no Apple support claim |
| `VK_LAYER_KHRONOS_validation` | Optional diagnostic | Request only under the resolved diagnostic policy and activate only when enumerated | Log disabled diagnostic once and continue |
| `VK_KHR_swapchain` | Platform-required device extension | Every supported Win64 Editor/Game candidate must support and activate it | Reject that physical-device candidate |
| `VK_EXT_swapchain_maintenance1` plus feature bit | Optional feature | Activate only when both instance dependencies, device extension, and feature bit are present | Use the existing queue-idle recreation/teardown fallback |
| `VK_KHR_get_memory_requirements2`, `VK_KHR_dedicated_allocation`, and `VK_KHR_bind_memory2` | Promoted core in Vulkan 1.1 | Use core functionality; do not request extension names | Covered by the Vulkan 1.1 floor |
| `VK_KHR_synchronization2` plus feature bit | Optional feature; promoted core in Vulkan 1.3 | Prefer core 1.3; otherwise activate the extension only when its feature bit is present | Publish false and retain legacy barriers |
| `VK_KHR_portability_subset` | Deferred Apple platform requirement | Not requested by a supported profile | No Win64 effect and no Apple support claim |
| `fillModeNonSolid` | Required physical-device feature | Require and activate for current Renderer wireframe paths | Reject that candidate |
| `shaderDrawParameters` | Required physical-device feature; core Vulkan 1.1 | Require and activate for the established shader/draw baseline | Reject that candidate |
| `swapchainMaintenance1` | Optional physical-device feature | Activate only with the complete extension dependency chain | Use the queue-idle fallback |
| `synchronization2` | Optional physical-device feature | Activate only with core 1.3 or the activated extension | Publish false and use legacy barriers |
| `geometryShader` | Unconsumed legacy suitability check | Remove from hard requirements and do not activate or publish it | No effect on suitability |

Support enumeration, policy request, extension-name activation, feature-bit
activation, and public capability publication are distinct states stored in
candidate-owned memory. Required-name lists are deduplicated before native
calls, and no pointer in a Vulkan create-info chain refers to temporary storage.

### Device, queue, and Win64 WSI rule

Each physical device is first evaluated into a local candidate. Hard
requirements include the API floor, required device extensions/features,
baseline 2D/cube limits including at least six array layers, one queue family
with graphics and compute flags, and
`vkGetPhysicalDeviceWin32PresentationSupportKHR` returning true for that same
family. The lowest-index family satisfying all three queue/WSI requirements is
selected. Only queue zero from that family is provisioned; graphics, current
compute commands, transfer commands, and presentation use that one synchronous
queue topology. Dedicated compute/transfer families may be recorded in backend
diagnostics but are not created, wrapped, published, or scheduled by M0.

Hard-rejected candidates are never ranked. Passing candidates are ordered by
device type (discrete, integrated, virtual, other, CPU), then descending
`maxImageDimension2D`, then descending API version, then ascending vendor ID,
device ID, and device name for a stable tie break. Total failure reports up to
16 devices, up to eight reasons per device, and truncates each owned reason to
256 bytes; an overflow count preserves how much evidence was omitted.

Every later Win64 surface is checked only against the already selected family.
`SetupPresentQueue` becomes a validation operation and may not enumerate or
construct another queue wrapper. A surface that fails this check aborts the
new viewport/swapchain candidate before publication. Main-window and ImGui
detached viewports use the identical rule, while an existing complete viewport
continues unaffected by another surface's rejection.

### Structural-cache transactional working set

| Candidate/cache | Immutable identity and current publication | Frozen Stage 4 work |
| --- | --- | --- |
| Render pass | `FVulkanRenderPassKey` copies `FRHIRenderTargetLayout`; `RenderPasses` is keyed structurally | Replace catch-and-continue with propagated failure, add `RenderPass` injection, and prove failure leaves map size/lookup unchanged and same-key retry inserts one complete handle |
| Framebuffer | Linear cache identity is the complete render-pass object plus color/resolve/depth image identities; insertion happens after construction | Build attachment views and framebuffer in local RAII storage because a constructor throw currently leaks earlier views; add distinct framebuffer-view and framebuffer injection, cleanup, unchanged-cache, and retry tests |
| Descriptor-set layout | Sorted binding descriptions plus hash/memcmp key; native handle is created before map insertion | Preserve the complete-entry rule, replace `operator[]` publication with explicit complete-candidate insertion, add descriptor-layout injection and same-key/cache-size proof; complete earlier per-set entries may be reused after a later set fails |
| Pipeline descriptor-layout owner | `LayoutMap` is keyed by complete `FVulkanDescriptorSetsLayoutInfo` and inserts only after every referenced descriptor-set layout exists | Audit and test that no null `FVulkanLayout*` entry remains after dependency failure; debug names are not identity |
| Graphics pipeline layout and pipeline | Not structurally cached; each public PSO owns a candidate pipeline layout and pipeline and publishes both only after success | Retain existing pipeline-layout/graphics-pipeline injection and rollback tests, add render-pass/descriptor dependency failure propagation, and keep same debug names producing independent PSOs |

No descriptor pools, per-frame descriptor sets, swapchains, textures, buffers,
or deferred-deletion queues are added to this bounded structural-cache stage;
their publication rules are already covered by existing contracts or another
plan.

### Baseline and required validation additions

The Stage 0 baseline used the primary Debug Editor test profile and the root
[build and run](../Development/Build/BuildAndRun.md) workflow:

| Baseline boundary | Evidence on 2026-08-09 | Existing coverage retained |
| --- | --- | --- |
| RHI initialization rollback | `RHIInitializationTests`, 4/4 passed | Thread-launch failure, backend-init rollback on the RHI thread, and inline/threaded policy resolution |
| Texture and render-target structure | `RenderContractTests` filters `FRHITextureTests.*:FRenderTargetLayoutTests.*`, 10/10 passed | Cube shape/layers/samples, mip upload bounds/block alignment, and render-target identity/resolve constraints |
| Vulkan startup, factory failure, texture sampling, and swapchain retry | `VulkanRHIIntegrationTests`, 9/9 passed | Instance/device/allocator rollback then success, inline/threaded nullable factories, image/view/pipeline rollback, all current sampled formats/mips, and transactional viewport output retry |
| Complete runtime build | Full `all` target passed | Current Debug Editor runtime and modules link from the same profile |
| Runtime startup/shutdown | `DurinEditor` hidden window with Sandbox and 30 ticks exited normally | Application, Vulkan RHI, Renderer, frame ticks, and ordered shutdown |

Later stages add these exact boundaries rather than replacing the baseline:

| Stage | Required new tests |
| --- | --- |
| Stage 1 | Fake-RHI snapshot unavailable/publish/failure tests; table-driven validity for every dimension, usage conflict, mip, sample, and checked bound; Vulkan snapshot property checks; exact support/create agreement; 2D/cube native success; and proof that deferred dimensions and unsupported limit/format/usage combinations do not consume the image-create failure point |
| Stage 2 | Pure instance-negotiation tests for loader floor, required-missing, promoted-core deduplication, optional-missing, diagnostic off, diagnostic requested/present, diagnostic requested/layer absent, debug-utils absent, invalid policy, and failure followed by success with no retained candidate state |
| Stage 3 | Pure device-candidate tests for each hard rejection, deterministic ties, bounded all-device diagnostics, graphics/compute/Win32-present family selection, and optional synchronization/maintenance publication; hardware main and detached surfaces; incompatible-surface viewport rollback; and failed-candidate capability isolation |
| Stage 4 | Failure-injection, cleanup counters, cache-size/lookup invariants, and same-key retry for render pass, framebuffer views/framebuffer, descriptor-set layout, pipeline descriptor-layout dependency, pipeline layout, and graphics pipeline; identity tests prove debug names and reusable native handles are not keys |
| Stage 5 | All focused targets above in inline and threaded modes where applicable, Release Editor diagnostic-off startup, Shipping Game diagnostic-off startup, optional-unavailable simulation, visible main/detached WSI qualification, native aggregate, full `all` build, and repeated hidden Editor/Game normal shutdown smoke |

## Implementation Stages

### Stage 0: Freeze the startup and capability contract

- [x] Record the required Win64 Editor/Game runtime profiles and explicitly
  disposition the existing Apple portability branches as qualified, compile-
  only, or deferred rather than implying untested parity.
- [x] Inventory current Renderer and next-milestone consumers of feature level,
  texture limits/dimensions/samples, queue flags, presentation topology,
  synchronization feature choice, and optional diagnostics.
- [x] Define the minimal public capability fields, exact value domains,
  fallback/rejection behavior, and owning query surface.
- [x] Classify every currently requested instance layer, instance/device
  extension, promoted feature, and physical-device feature as required,
  platform-required, optional feature, or optional diagnostic.
- [x] Select the validation enablement/configuration owner and prove that an
  unavailable validation layer does not block the normal mode.
- [x] Select the Win64 presentation-family discovery/provisioning rule before
  logical-device creation and define behavior for a later incompatible surface.
- [x] Inventory render-pass, framebuffer, descriptor-layout, pipeline-layout,
  and pipeline candidate construction/publication paths and freeze the bounded
  transactional working set.
- [x] Record baseline initialization, texture-validation, swapchain, and
  failure-injection coverage and the exact new tests required by later stages.

#### Acceptance Gate

- The platform matrix, capability field/consumer table, Vulkan requirement
  classification, WSI queue rule, cache working set, and validation matrix are
  recorded in this plan with no simultaneous alternative presented as an
  implementation decision. The current initialization rollback and supported
  Win64 hidden-window baseline pass before Stage 1 changes begin.
- Passed on 2026-08-09 with the focused counts and Debug Editor build/runtime
  evidence recorded in the Stage 0 baseline table.

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
