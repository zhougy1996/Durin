# RHI Capabilities and Vulkan Startup

Summary: Define the immutable public capability snapshot, exact texture support,
Vulkan startup negotiation, presentation-aware queue/WSI topology, and complete
structural candidate rules.

Modules: RHI, VulkanRHI, MetalRHI, ApplicationCore

## Public Capability Contract

`FDynamicRHI::RHIGetCapabilities()` returns null until one backend has completed
initialization. Successful initialization publishes one immutable
`FRHICapabilities` value; shutdown clears it before the backend is destroyed.
Reads require no RHI-thread round trip. The active public fields are:

- `FeatureLevel`, which is `ES3_1` for the current portable graphics baseline;
- `SupportedTextureDimensions`, currently 2D, non-array 3D, and cube;
- positive 2D/3D/cube dimension and array-layer limits;
- conservative color and depth sample-count masks; and
- positive `MinStorageBufferOffsetAlignment` and `MaxStorageBufferRange`
  limits for exact dynamic storage-range admission; and
- three positive `MaxComputeWorkGroupCount` values copied from Vulkan device
  limits for direct-dispatch admission; and
- `bSupportsIndirectDraw` and `bSupportsIndirectDispatch`, true only for a
  complete executable single-command path; native multi-draw remains false
  with a zero maximum count in the current contract; and
- `bSupportsSynchronization2`, true only when the selected device activated the
  core Vulkan 1.3 feature or the Vulkan 1.1/1.2 extension feature chain; and
- `bSupportsGPUTimestamps` plus `GPUTimestampNanosecondsPerTick`, published only
  when the selected immediate queue has nonzero valid timestamp bits and a
  finite positive timestamp period.

Feature level does not imply optional features. Consumers use the exact limit or
capability field for their path and retain their documented fallback. Vulkan
API versions, extension names, queue-family indices, validation state, and
native handles remain backend-private.

## Texture Validity and Support

`ValidateTextureCreateDesc` owns backend-neutral structural validity in stable
diagnostic order. It validates nonzero extent/depth/layers/mips/samples, known
format, supported sample-count vocabulary, dimension-specific depth and layer
rules, cube face grouping, complete mip bounds, multisample restrictions,
mutually exclusive usages, and checked subresource arithmetic.

Structural validity is not device support. `RHIIsTextureSupported` requires a
valid complete description and answers device support without allocation.
Vulkan maps the exact format, image type, optimal tiling, usage, flags, extent,
mips, layers, and samples into `vkGetPhysicalDeviceImageFormatProperties`.
Creation uses the same `vk::ImageCreateInfo`. The public support-query and
creation boundaries each validate and normalize their input once, then share
an internal support query over the normalized description. A valid unsupported description
returns false and `RHICreateTexture` logs one owned diagnostic and returns null
before image allocation. Invalid programmer descriptions assert at the public
boundary.

The current native mapping implements 2D as `e2D/e2D`, non-array 3D as
`e3D/e3D`, and cube as an `e2D` cube-compatible image with an `eCube` default
view. A 3D texture has one array layer and one sample; Z slices are texels within
a mip rather than subresources. Sampled, storage, source-copy, and
destination-copy usages are admitted through the exact support query. 3D
render/depth attachments, resolves, multisampling, cube compatibility, and
array semantics are rejected structurally. 2D arrays and cube arrays remain
unsupported.

## Instance Negotiation

Instance startup first enumerates the loader API version, extensions, and
layers into candidate-owned storage. Vulkan 1.1 is the required loader floor and
1.3 is the request ceiling. Every requirement records support, request,
activation, and one of these classes: required runtime, platform required,
optional feature, optional diagnostic, or promoted core.

The surface provider's required extension names and the backend portability
policy are combined and deduplicated in stable input order before negotiation.
An empty surface-provider requirement set fails before negotiation. On Win64,
`VK_KHR_surface` and `VK_KHR_win32_surface` are platform requirements. A
portability-enumeration policy additionally requires
`VK_KHR_portability_enumeration` and enables the matching instance-create flag.
Properties2 is satisfied by the Vulkan 1.1 core and its extension name is not
requested. Surface maintenance activates only with its complete optional
dependency. Required absence fails before `vkCreateInstance`; optional absence
is logged once and startup continues.

`DURIN_VULKAN_VALIDATION` accepts `auto`, `on`, and `off`. Unset or invalid
values resolve to `auto`; invalid input is logged once. `auto` requests
diagnostics only in Debug, `on` requests them in Debug or Release, `off` never
requests them, and Shipping always disables them. The Khronos validation layer
and debug-utils extension are independent optional diagnostics.

## Device and Queue Publication

Every physical device is evaluated locally before ranking. Hard requirements
are Vulkan 1.1, `VK_KHR_swapchain`, `fillModeNonSolid`,
`shaderDrawParameters`, nonzero 2D/3D/cube limits, at least six array layers,
positive storage-buffer alignment/range and direct-dispatch group-count limits,
and one queue family with queue
zero, graphics and compute flags, and presentation support. In presentation
mode Vulkan creates the real startup surface first and calls
`getSurfaceSupportKHR` for that exact surface on both Windows and macOS.
Headless mode creates no surface and selects the lowest graphics/compute family
without publishing a presentation claim. Rejected devices never receive a
ranking position.

Suitable devices rank deterministically by device type, descending 2D limit,
descending API version, then ascending vendor ID, device ID, and name. Complete
failure reports a bounded device-qualified reason set. Logical-device extension
names, feature chains, and queue create infos retain candidate-owned backing
storage until native creation completes.

The selected lowest compatible family owns the graphics/presentation queue.
Production command recording, compute-backed operations and transfers use it.
For native topology qualification, `DURIN_VULKAN_COMPUTE_QUEUE` accepts
`disabled` (default), `same-family`, `dedicated`, or `auto`. Automatic selection
prefers a compute-only family, then queue 1 in the graphics family. Forced
choices fall back to graphics when unavailable. Independent provisioning
requires the timeline-semaphore feature and either Vulkan 1.2 or the Vulkan
1.1 timeline extension; feature and extension enablement use the candidate's
owned feature chain. Provisioned physical IDs appear in `RHIGetQueueCapabilities`,
but `bIndependentCompute` remains false until production resource-use and
ownership migration is complete. A later main or
ImGui detached surface must support that provisioned family.
`FVulkanDevice::SetupPresentQueue` only validates compatibility; it never creates
a wrapper for an unprovisioned family. An incompatible surface fails its new
swapchain/viewport candidate before native swapchain creation and cannot disturb
an existing complete viewport.

Surface capabilities, formats, and present modes are not startup capabilities.
They belong to one concrete surface snapshot and are queried for every
transactional main or detached swapchain create/recreate candidate. The
candidate validates them without mutating Vulkan state; dynamic WSI values are
never published in the immutable `FRHICapabilities` snapshot.

## Startup Presentation Ownership

`DURIN_RHI_BACKEND` selects `vulkan` or `metal` explicitly. An unset value
selects Vulkan; an invalid value fails RHI initialization with a diagnostic.
MetalRHI admits headless or `CAMetalLayer` presentation startup on macOS 27+
with Apple GPU Family 9+. It creates a native device and one physical queue.
The Cocoa window installs the layer before RHI startup; Metal retains it during
viewport use, renders to an owned offscreen back buffer, then blits to an
available drawable and presents. The headless layer qualification covers clear,
present, resize, and viewport recreation in inline and threaded modes; real
window minimize/close behavior and visible output remain to be qualified.
Empty logical submissions publish a single-queue topology, stay pending through
CPU replay, and complete after native Metal command-buffer completion; shutdown
cancels replayed work that was never submitted. Metal publishes conservative
capabilities and rejects unsupported texture descriptions through its exact
support query.
Shared Metal buffers support initial data, staged writes/uploads, and
buffer-to-buffer copies on that queue; source and staging storage remain owned
through GPU completion. Immutable Metal samplers can be created for normalized
coordinates, with supported filtering, addressing, comparison, border,
anisotropy, and LOD state. Unsupported sampler descriptors return null.
Validated uniform, structured, and byte-address buffer views retain their
parent allocation; formatted buffer views are unsupported. Single-sample 2D
textures in R8_UNORM, RG8_UNORM, R16_FLOAT, RGBA8_UNORM,
BGRA8_UNORM, SRGBA8_UNORM, SBGRA8_UNORM, R11G11B10_FLOAT, RGBA16_FLOAT,
RGBA32_FLOAT, and RG32_UINT support transfer and CPU readback flags,
recorded buffer/texture and texture/texture copies with padded row layouts,
pitched 2D uploads, synchronous and asynchronous readback, and validated
transfer views. RGBA8_UNORM and RGBA16_FLOAT cube textures, and RGBA8_UNORM 3D
textures, support mip-aware transfer,
including cube-face readback and pitched 3D uploads. RGBA8_UNORM 2D arrays and
cube arrays support layer-wise copies, uploads, readback, and transfer views;
cube-array layers are numbered by face across cubes. A 2D texture can copy into
an array layer. RGBA32_FLOAT cube textures and BC1_UNORM/BC1_UNORM_SRGB cubes
are also admitted for sampled use; BC1 requires the native device's BC texture
compression capability and uses block-aware upload/readback pitches. BC1 buffer
and texture copy usages remain unsupported by the Metal support query. Other
array/cube/volume formats require their own exact support result. Metal retains
upload and readback storage through GPU completion; an unsubmitted readback is
canceled at shutdown. Sampled and storage views, shader functions and pipelines,
and supported graphics/compute execution are available through the production
RHI path.
The selected module is unloaded after its backend and RHI execution thread.

Windowed startup supplies an explicit `FRHIInitializationContext` with the
primary native handle. On macOS ApplicationCore installs the `CAMetalLayer` on
the AppKit main thread before RHI initialization; surface creation remains an
RHI-thread operation on both platforms. `FVulkanPresentationCandidate` owns the
surface across instance creation, device admission, and logical-device setup.
ApplicationCore discovers GLFW Vulkan instance extensions only when the Vulkan
presentation device requests them; ApplicationCore startup and headless RHI
initialization do not depend on that query.
If initialization or shutdown occurs before adoption, that RAII owner destroys
the surface before the instance.

The startup viewport requests the one allowed transfer with
`FRHIViewportCreateInfo::bAdoptInitializationPresentationCandidate`. The stored
native handle rejects an accidental wrong-window adoption but is not a generic
window identity and does not drive consumption. Mismatch and duplicate adoption
fail deterministically without replacement-surface fallback. Ordinary later
viewports create and own independent surfaces.

## Complete Structural Candidates

Render-pass, framebuffer, descriptor-set-layout, pipeline descriptor-layout,
pipeline-layout, graphics-pipeline, and compute-pipeline construction follows
one rule: native
handles and dependent immutable state remain local until complete, then one
cache or public owner publishes them.

- Render-pass failure propagates and leaves its structural map unchanged.
- Framebuffer attachment views and the framebuffer are local candidates; any
  failure destroys earlier views before propagation.
- Descriptor-set layouts use explicit insertion only after native creation.
  Complete earlier set layouts may remain reusable if a later set fails.
- Pipeline descriptor-layout maps contain only owned non-null complete values.
- Graphics and compute pipeline layout/pipeline handles publish together;
  dependency or native failure returns null through the owning public PSO
  factory.

Debug names annotate diagnostics and command regions. They are never structural
cache identity. Same-key retry after an injected failure creates one complete
entry and no failed candidate changes cache lookup or size.

## Supported Profile and Validation Boundary

The supported runtime matrix is Win64 Debug Editor, Release Editor, and Shipping
Game. Debug owns focused native and hardware-backed failure/WSI coverage.
Release and Shipping qualify normal diagnostic-off startup; Shipping never
links the Editor detached-viewport path. Existing Apple portability branches are
source-only intent and do not claim runtime support.

The lasting validation owners are the target-level RHI initialization and render
contract tests, the GPU-serialized Vulkan integration target, the native-test
aggregate, the full runtime build, and normal hidden-window Editor/Game startup
and shutdown through DurinDevTool. Test registration totals are discovered by
the current native-test framework and are not part of this contract.

## Related Documentation

- [RHI public API stability](RHIPublicAPIStability.md)
- [RHI command execution](RHICommandExecution.md)
- [RHI diagnostics and conformance](RHIDiagnosticsAndConformance.md)
- [Viewport rendering](ViewportRendering.md)
- [Texture system](TextureSystem.md)
- [RHI and Vulkan backend evolution](../../Roadmaps/Archive/2026-08/RHIAndVulkanEvolution.md)
- [Build and run](../../Development/Build/BuildAndRun.md)
- [Native tests](../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/RHICapabilities.h`
- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanExtension.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanRenderPass.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanFramebuffer.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanDescriptorSets.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
