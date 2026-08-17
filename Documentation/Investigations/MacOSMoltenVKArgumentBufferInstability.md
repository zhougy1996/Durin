# macOS MoltenVK Argument-Buffer Instability

**Status:** Open; discrete resource indexes are the qualified workaround, and
reevaluation is due when the supported LunarG SDK bundles MoltenVK 1.4.2 or newer

**Last reviewed:** 2026-08-17

## Scope And Verdict

On macOS, the editor can remain open and responsive while scene geometry
flickers between frames. The failure was reproduced with the LunarG Vulkan SDK
1.4.357.0, whose macOS package contains MoltenVK 1.4.1.

The qualified workaround is to use `VK_EXT_layer_settings` during Vulkan
instance creation to set MoltenVK's
`MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` option to `false`. This selects
MoltenVK's discrete resource-index binding path globally for the instance.
With that setting active, the observed scene-geometry flicker stopped.

This evidence isolates the problem to the Metal argument-buffer descriptor
path or its interaction with Durin's descriptor updates. It does not yet prove
which MoltenVK, Metal-driver, or Durin invariant is violated. The workaround
therefore remains a compatibility measure rather than a permanent rendering
contract.

Relevant implementation and architecture:

- [Vulkan instance creation](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanDynamicRHI.cpp),
  including the `VK_EXT_layer_settings` instance extension and
  `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` setting;
- [Vulkan device and descriptor implementation](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanDevice.cpp);
- [Vulkan RHI macOS test environment](../../Engine/Tests/Native/VulkanRHITests/Private/VulkanRHITestEnvironment.cpp);
- [viewport rendering](../Runtime/Rendering/ViewportRendering.md), including
  the macOS Metal-layer and presentation path;
- [macOS platform enablement plan](../Plans/Archive/2026-08/MacOSPlatformRuntime.md).

## Verified Findings

### P1 — MoltenVK's argument-buffer path correlates with scene flicker

Durin uses per-frame suballocated uniform buffers and dynamic descriptor
offsets. With MoltenVK's Metal argument buffers enabled, scene objects were
observed flickering even though the editor process remained alive. Vulkan
validation did not report a matching descriptor or synchronization error.

Setting `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=false` through instance-level
`VK_EXT_layer_settings` changed MoltenVK's runtime report to
`Descriptor sets binding resources using discrete resource indexes.` The
editor then completed a 12,000-tick visible run, including three recoverable
`SuboptimalKHR` swapchain recreations, without the original scene-object
flicker or a Vulkan validation failure.

Thirty frames captured over approximately nine seconds after the host was
unlocked provided an additional image check. Twenty-eight frames matched
exactly. The remaining two differed by 100 pixels confined to the FPS and
toolbar edge, not scene geometry; wall and floor geometry remained stable.

**Impact:** leaving argument buffers enabled makes the macOS editor visually
unreliable even when rendering and event processing continue. Disabling them
trades the higher-limit, generally faster Metal binding path for a stable
compatibility path using discrete resource indexes.

### P1 — A GBuffer-only uniform-buffer substitution was not effective

An earlier experiment changed selected GBuffer dynamic uniform-buffer
parameters to static uniform-buffer parameters on Apple platforms. It did not
eliminate the flicker and covered only a subset of the descriptor traffic that
MoltenVK translates to Metal argument tables.

That experiment was reverted. No Apple-specific GBuffer binding exception is
part of the retained workaround; the backend selection is made once during
Vulkan instance creation so all descriptor sets use one binding model.

**Impact:** reintroducing the GBuffer substitution would add platform-specific
renderer behavior without addressing the verified failure boundary.

### P2 — MoltenVK 1.4.2 contains directly relevant alignment fixes

MoltenVK 1.4.2 is an upstream stable release, but the latest installed LunarG
SDK 1.4.357.0 still packages MoltenVK 1.4.1. The projects have independent
release cadences, so a newly installed LunarG SDK does not imply that it
contains the newest standalone MoltenVK release.

The [MoltenVK 1.4.2 release notes](https://github.com/KhronosGroup/MoltenVK/releases/tag/v1.4.2)
include fixes for Metal constant-buffer alignment in argument buffers, uniform
alignment selection, and per-descriptor-set alignment padding. Those changes
are highly relevant to Durin's suballocated uniform buffers, but they have not
yet been qualified in Durin and are not proof that upgrading will resolve this
specific flicker.

**Impact:** manually replacing the SDK's MoltenVK library now would reduce
toolchain reproducibility. The discrete-index workaround is preferable until
the supported LunarG package carries the newer implementation, unless the
workaround produces a measured performance or resource-limit regression.

## Confirmed Correct Behavior

- `VK_EXT_layer_settings` applies the MoltenVK option at instance creation and
  overrides a conflicting `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1`
  environment value in the qualification run.
- The setting is macOS-only and does not change Vulkan instance creation on
  other platforms.
- The retained change disables an advanced MoltenVK binding feature; it does
  not enable a newer rendering feature. Functional Vulkan descriptors remain
  available through discrete Metal resource indexes.
- Swapchain `SuboptimalKHR` recovery continued to work with the compatibility
  path and was not the source of the observed scene flicker.

## Risks, Assumptions, And Validation Gaps

- **Risk:** discrete resource indexes may reduce resource limits or CPU/GPU
  binding performance compared with Metal argument buffers. No material Durin
  regression has been measured yet.
- **Risk:** a future SDK update could change MoltenVK defaults or setting names;
  startup diagnostics must confirm the selected binding path during
  qualification.
- **Assumption:** the 1.4.2 alignment changes address the relevant upstream
  defect. Only an argument-buffers-enabled Durin run can confirm that.
- **Gap:** Vulkan validation cannot inspect MoltenVK's internal Metal argument
  tables, so a clean validation log does not establish correct translated
  descriptor contents.
- **Environment note:** one attempted screenshot and one direct native-test run
  were interrupted when macOS entered the lock screen. The native-test process
  was observed waiting in the AppKit application loop rather than failing in
  Vulkan. The visual frame comparison cited above was performed after unlock.
- **Test gap:** the complete Vulkan RHI integration target passes through its
  declared repository application host with the registered primary
  presentation window. Run it with
  `./DevTool test VulkanRHIIntegrationTests --mode qualification`; no temporary
  bundle or manual `open` command is required. It qualifies RHI admission and
  hardware behavior, but does not provide an automated regression test for
  this visual artifact.

## Reevaluation Procedure

Reevaluate when the supported LunarG SDK bundles MoltenVK 1.4.2 or newer:

1. Install the new LunarG SDK alongside the current SDK and update the local
   DurinDevTool environment selection without overwriting the known-good SDK.
2. Confirm the bundled MoltenVK version from its headers or runtime diagnostics.
3. Remove or temporarily invert the
   `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=false` layer setting and confirm that
   MoltenVK reports the argument-buffer binding path.
4. Build the Vulkan RHI and full editor, then repeat a visible editor run with
   representative Lit scene geometry, camera movement, viewport resizing, and
   swapchain recreation.
5. Capture a timed frame sequence and compare scene regions rather than relying
   only on visual memory. Record validation output and MoltenVK startup
   diagnostics with the result.
6. Compare frame time, descriptor/resource limits, and stability between the
   argument-buffer and discrete-index paths.

The workaround can be removed only when argument buffers remain stable through
that qualification. If flicker persists on the newer SDK, retain the workaround
and reduce the issue to a minimal MoltenVK reproduction before reporting it
upstream.

## Upstream Context

- [MoltenVK configuration parameters](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Configuration_Parameters.md)
  document the layer setting, its precedence, and the argument-buffer option.
- [MoltenVK issue 2727](https://github.com/KhronosGroup/MoltenVK/issues/2727)
  reports a MoltenVK 1.4.1 failure involving dynamic uniform buffers and the
  Metal argument-encoder path. Its hardware and symptom differ from Durin, so
  it is supporting context rather than a duplicate report.
- [MoltenVK issue 2278](https://github.com/KhronosGroup/MoltenVK/issues/2278)
  records a device-dependent descriptor-indexing failure with clean Vulkan
  validation, again demonstrating a related failure class rather than the same
  defect.
