# Volumetric Cloud Spatial Rendering

Summary: Defines the deterministic volumetric-cloud spatial producer, scene-linear composition, route fallback, and Renderer resource lifetime.

Modules: Renderer

Last reviewed: 2026-09-03

## Ownership and input

`FVolumetricCloudRenderer` owns cloud shader payloads, the optional-white
weather fallback, timing/capture hooks, and composition. Spatial/composite
targets are frame-transient graph resources. RHI and VulkanRHI remain
cloud-agnostic. Generic volume and 2D
textures remain owned by their asset/render-resource producers.

One prepared view may publish an immutable renderer-private input translated
from the selected Engine cloud scene snapshot and containing
base and detail `Texture3D` density inputs, optional weather `Texture2D`, the
opaque scene depth, a sampler, and `FVolumetricCloudSpatialRenderer::FParameters`.
Base, detail, depth, sampler, valid extent, and finite parameters are required.
Missing weather resolves to a one-texel white texture. Missing or invalid
required input selects the disabled route and cannot fail the containing view.

The coordinate domain is a flat world-Z slab. Its default bounds are Z
1,500–3,500 with a 100,000-unit trace limit, at most 32 primary samples and four
directional form samples. World-space base/detail frequencies, wind offsets,
weather coverage, erosion, and extinction are shared by the CPU reference,
compute shader, and fragment shader. The production phase, self-transmittance,
ambient, and receiver-shadow contract is documented in
[Volumetric cloud lighting and shadows](VolumetricCloudLightingAndShadows.md).

## Output and composition

The spatial target is `RGBA16_FLOAT`. RGB contains premultiplied scene-linear
radiance and A contains transmittance. Pixels outside the fitted viewport or
without a positive depth-clipped slab interval contain `(0, 0, 0, 1)`.

Composition occurs after sky and opaque/masked lighting and before sorted scene
translucency. A full-target scene-linear ping-pong draw samples the previous
Scene Color and cloud target and implements the exact contract:

`Scene.rgb = Cloud.rgb + Cloud.a * Scene.rgb`

Scene alpha is preserved, and the cloud identity value preserves pixels outside
the fitted viewport. The hybrid retained-forward phase therefore has
three explicit boundaries: retained opaque/masked, cloud render/composite, and
sorted translucency. Unlit and wireframe views retain the existing no-cloud
special-forward path. Post-process, editor assistance, offscreen output, and
Present remain downstream.

## Route and synchronization

Compute is selected when the reflected payload, sampled/storage target views,
required inputs, extent, and public compute limits are valid. It dispatches one
8x8 grid outside graphics render passes. Required textures transition from
graphics sampling to compute sampling, the cloud target transitions from
discard to compute read/write, and every input plus the output returns to
graphics sampling before composition.

If compute payload, target, or extent eligibility fails, an equivalent
fullscreen fragment pass is selected when its payload and target are complete.
If neither producer is complete, no cloud pass or composite occurs. No route
uses an intermediate copy, device-idle wait, backend handle, or Vulkan-specific
command.

## Lifetime, diagnostics, and recovery

Fragment, compute, and composite targets are graph-owned `RGBA16_FLOAT`
resources costing 8 bytes per target pixel. Compute uses canonical
sampled/storage views. Route selection finishes before graph compilation.
Allocation and retention follow [frame resource lifetimes](RendererFramePreparation.md#resource-lifetime-classes);
feature byte costs do not define independent cache quotas.

Shader/device/manual generations control persistent-payload retry and
invalidation. Same-device replacement may retain a compatible complete shader
or pipeline; transient targets never borrow another view's stale output.
Shutdown releases feature payloads and the white-weather fallback, while the
allocator owns transient release. Committed temporal history remains view-owned.

Per-view counters publish route and reason, dispatch/draw/copy structure,
primary/light sample budgets, active and retained target bytes, and composite
draws. Timing and capture sinks receive the complete selected cloud target.
Optional producer unavailability disables clouds for that view. Failure of the
shared graph allocation or required final output follows the frame-level abort
contract rather than publishing a partial frame.

## Qualification

`VolumetricCloudQualificationTests` runs the frozen 1280x720, 1920x1080,
1919x1079-with-1601x901-fitted-viewport, and forward-Z odd/fitted matrix. Each compute and forced-fragment
route receives 30 warm-up frames and 120 timestamped frames. The test always
requires compute/fragment agreement within 2/1024 per half-float channel,
exact dispatch/draw/copy structure, bounded retained bytes, and clean resource
release. It applies the 12 ms compute median, 16 ms compute p95, and 150%
fragment/compute median gates only when the physical device reports the frozen
NVIDIA GeForce RTX 3090 and Vulkan 1.4.325 identity; other devices emit
explicit non-gating observations.

Production views receive their input from the selected immutable Engine
scene snapshot. Tests force the fragment route through the feature-bounded
Renderer-private `FScopedRendererQualificationPolicy`; the graph executor
snapshots this value without mutating cloud preparation. There is no cloud
preparation callback or shipping option that can select the qualification
route. `VolumetricCloudSceneVulkanTests` uses real reflected actors and volume
assets to prove exact no-cloud behavior, compute/fragment final SRGBA8 parity,
offscreen output, window-backed Present, resize, post-process continuity, and
clean shutdown through the same `RenderView` scheduler.

Run both executor lanes explicitly:

```powershell
$env:DURIN_RHI_EXECUTION='inline'
.\DevTool.bat test VolumetricCloudQualificationTests --mode qualification
$env:DURIN_RHI_EXECUTION='threaded'
.\DevTool.bat test VolumetricCloudQualificationTests --mode qualification
```

## Related documentation

- [Volumetric cloud scene contract](VolumetricCloudSceneContract.md)
- [Volumetric cloud temporal reconstruction](VolumetricCloudTemporalReconstruction.md)
- [Volumetric cloud lighting and shadows](VolumetricCloudLightingAndShadows.md)
- [Volume textures](../Assets/VolumeTextures.md)
- [Synchronous compute pipelines](SynchronousComputePipelines.md)
- [RHI resource transitions](RHIResourceTransitions.md)
- [Render resource lifecycle](RenderResourceLifecycle.md)
- [Renderer resource recovery](RendererResourceRecovery.md)
- [Viewport rendering](ViewportRendering.md)
