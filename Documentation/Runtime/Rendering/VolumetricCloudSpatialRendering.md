# Volumetric Cloud Spatial Rendering

Summary: Defines the deterministic full-resolution volumetric-cloud producer, scene-linear composition, route fallback, and Renderer resource lifetime.

Modules: Renderer

Last reviewed: 2026-08-21

## Ownership and input

`FVolumetricCloudRenderer` owns cloud shader payloads, canonical output views,
extent-keyed targets, the optional-white weather fallback, timing/capture hooks,
and composition. RHI and VulkanRHI remain cloud-agnostic. Generic volume and 2D
textures remain owned by their asset/render-resource producers.

One prepared view may publish an immutable renderer-private input containing
base and detail `Texture3D` density inputs, optional weather `Texture2D`, the
opaque scene depth, a sampler, and `FVolumetricCloudSpatialRenderer::FParameters`.
Base, detail, depth, sampler, valid extent, and finite parameters are required.
Missing weather resolves to a one-texel white texture. Missing or invalid
required input selects the disabled route and cannot fail the containing view.

The P1 coordinate domain is a flat world-Z slab. Its default bounds are Z
1,500–3,500 with a 100,000-unit trace limit, at most 32 primary samples and four
directional form samples. World-space base/detail frequencies, wind offsets,
weather coverage, erosion, extinction, and the minimal light term are shared by
the CPU reference, compute shader, and fragment shader.

## Output and composition

The full target is `RGBA16_FLOAT`. RGB contains premultiplied scene-linear
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

Fragment, compute, and composite targets use complete-or-last-known-good
resource slots; compute also owns canonical sampled/storage views. Each target
family is capped at one third of the 64 MiB retained-target ceiling, so the
three families remain within 64 MiB total. Old extents are evicted in insertion
order while preserving the current extent. Oversized extents fail explicitly.

Shader/device/manual generations control retry and invalidation. Replacement
failure retains the previous complete payload when its generation remains
usable; device invalidation releases device-owned targets and views. Shutdown
releases target caches, shader payloads, the white-weather fallback, and shared
composition state.

Per-view counters publish route and reason, dispatch/draw/copy structure,
primary/light sample budgets, active and retained target bytes, and composite
draws. Timing and capture sinks receive the complete selected cloud target.
Failures are diagnostic only and never convert an otherwise renderable scene
view into a failed view.

## Qualification

`VolumetricCloudQualificationTests` runs the frozen 1280x720, 1920x1080, and
1919x1079-with-1601x901-fitted-viewport matrix. Each compute and forced-fragment
route receives 30 warm-up frames and 120 timestamped frames. The test always
requires compute/fragment agreement within 2/1024 per half-float channel,
exact dispatch/draw/copy structure, bounded retained bytes, and clean resource
release. It applies the 12 ms compute median, 16 ms compute p95, and 150%
fragment/compute median gates only when the physical device reports the frozen
NVIDIA GeForce RTX 3090 and Vulkan 1.4.325 identity; other devices emit
explicit non-gating observations.

Before P2 supplies scene snapshots, `SetVolumetricCloudPreparationSink` is the
development-only scene qualification seam. It may populate the same
renderer-private P1 input and force the fragment route for comparison, but it
is not a scene-authoring or shipping quality-policy contract.
`VolumetricCloudSceneVulkanTests` uses it to prove exact no-cloud behavior,
compute/fragment final SRGBA8 parity, offscreen output, window-backed Present,
resize, post-process continuity, and clean shutdown through `RenderView` on
both executors.

Run both executor lanes explicitly:

```powershell
$env:DURIN_RHI_EXECUTION='inline'
.\DevTool.bat test VolumetricCloudQualificationTests --mode qualification
$env:DURIN_RHI_EXECUTION='threaded'
.\DevTool.bat test VolumetricCloudQualificationTests --mode qualification
```

## Related documentation

- [Volume textures](../Assets/VolumeTextures.md)
- [Synchronous compute pipelines](SynchronousComputePipelines.md)
- [RHI resource transitions](RHIResourceTransitions.md)
- [Render resource lifecycle](RenderResourceLifecycle.md)
- [Renderer resource recovery](RendererResourceRecovery.md)
- [Viewport rendering](ViewportRendering.md)
