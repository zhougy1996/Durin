# Volumetric Cloud Lighting and Shadows

Summary: Defines production cloud single scattering, self-transmittance, ambient contribution, directional receiver visibility, fallback, lifetime, and qualification budgets.

Modules: Renderer

Last reviewed: 2026-09-03

## Selected light and cloud radiance

Clouds consume `FPreparedLightView::Directional.front()`, the same stable
prepared directional-light selection used by surface lighting. Renderer copies
its direction and scene-linear color multiplied by intensity into the immutable
cloud parameter block. Ambient radiance is the same color multiplied by the
light's ambient intensity. With no eligible light both colors are zero, the
receiver-shadow route is identity, and cloud rendering remains otherwise valid.

The CPU reference, compute shader, and fragment shader share the same bounded
single-scattering equation. The fixed first-version phase is Henyey-Greenstein
relative to isotropic with `g = 0.35`:

`P(cosTheta) = (1-g^2) / (1+g^2-2g*cosTheta)^(3/2)`

For each view-ray step, direct radiance is
`Tview * (1-Tstep) * P * Tlight * LightRadiance`; ambient radiance is
`Tview * (1-Tstep) * Ambient * AmbientRadiance`. The nested light ray samples
the same base, detail, weather, height, coverage, and erosion density field as
the primary ray. RGB remains premultiplied scene-linear radiance, alpha remains
transmittance, and composition remains
`Cloud.rgb + Cloud.a * Scene.rgb`.

The selected directional-light identity, direction, color, intensity, and
ambient intensity participate in the cloud history key. A radiance-changing
light mutation therefore rejects temporal history; ordinary stable camera
motion remains eligible.

## Directional receiver visibility

`FVolumetricCloudShadowRenderer` creates one full-output-resolution
screen-space `R8_UNORM` visibility target for every eligible Lit view. Opaque
device depth reconstructs the receiver position. A midpoint ray toward the
selected directional light samples the same cloud density field until the
cloud-slab exit and writes
`exp(-opticalDepth * LightExtinction)`.

The quality-owned shadow sample counts are 4, 6, 8, and 8 for `Performance`,
`High`, `Epic`, and `Reference`. The target follows the fitted viewport and
supports both depth conventions; pixels outside the fitted viewport and
background receivers retain visibility one. It is regenerated each eligible
view. There is no cross-frame shadow reuse or camera-dependent
shadow cache key.

Deferred directional lighting multiplies the existing geometric visibility,
contact visibility, and cloud visibility. Cloud visibility affects only the
selected directional-light contribution. Environment/ambient, emissive, sky,
clear color, Unlit, translucency, and the cloud composite are unchanged. A
missing, invalid, disabled, or failed cloud-shadow producer binds the white
identity texture and does not fail the containing view.

## Routes, diagnostics, and lifetime

The producer prefers synchronous compute when its shader, canonical
sampled/storage target views, and public dispatch limits are complete. A
matched fullscreen fragment producer is the explicit fallback. Both use the
same uniform layout and shader density helper. Vulkan integration compares the
two `R8_UNORM` images on a non-identity receiver fixture within one quantization
level.

Per-view diagnostics expose enabled state, route/reason, draw or dispatch
count, sample work, active bytes, retained bytes, identity fallback, and
lighting-sensitive history behavior. Timing and capture sinks identify the
shadow route independently from complete cloud-route timing.

A full-resolution `R8_UNORM` visibility target costs one byte per pixel,
8,294,400 bytes at 4K. The selected route declares a transient graph target;
[frame resource lifetimes](RendererFramePreparation.md#resource-lifetime-classes)
own allocation and retention. Feature shader/pipeline payloads follow Renderer
resource generations and compatible last-known-good publication. Invalidation
and shutdown release feature payloads and views without a device-idle wait.

## Qualification

The [`VolumetricCloudQualificationTests`](../../../Engine/Tests/Native/EngineTests/Private/VolumetricCloudQualificationTests.cpp)
frozen gate is NVIDIA GeForce RTX 3090 / Vulkan 1.4.325 at 3840x2160. It
runs six deterministic frames per tier with static, translated, rotated, and
cut cameras, and executes explicit inline and threaded lanes. Production tiers
render the cloud body at 1920x1080 and reconstruct temporally; receiver
visibility remains 3840x2160. Image and history acceptance are defined by
[Temporal Reconstruction](VolumetricCloudTemporalReconstruction.md#qualification).
The timed complete route uses 12 warm-up and 48 measured frames per tier.

| Tier | Complete median / p95 | Shadow median / p95 |
| --- | ---: | ---: |
| `Performance` | 20 / 32 ms | 4 / 32 ms |
| `High` | 26 / 40 ms | 6 / 32 ms |
| `Epic` | 32 / 48 ms | 8 / 32 ms |
| `Reference` | 60 / 80 ms | 12 / 32 ms |

`High` median must be at most 80% of `Reference`. The fixture requires at most
224 MiB complete-cloud retained bytes, exact target extent and sample work,
complete output, and the selected compute route. These are qualification
budgets, distinct from the shared allocator's structural policy.
Historical measurements and rollout decisions remain in the
[Cloud Lighting and Shadows plan](../../Plans/Archive/2026-08/VolumetricCloudLightingAndShadows.md).

`VolumetricCloudSceneContractTests`, `RenderShaderContractTests`,
`VolumetricCloudVulkanTests`, `VolumetricCloudSceneVulkanTests`, and
`VolumetricCloudQualificationTests` cover selected-light mutation, uniform and
reflection layout, compute/fragment parity, scene integration, fitted views,
fallback, retry, invalidation, release, both executors, and the named 4K gate.

## Related documentation

- [Volumetric cloud spatial rendering](VolumetricCloudSpatialRendering.md)
- [Volumetric cloud temporal reconstruction](VolumetricCloudTemporalReconstruction.md)
- [Volumetric cloud scene contract](VolumetricCloudSceneContract.md)
- [Deferred directional lighting](DeferredDirectionalLighting.md)
- [Directional shadows](DirectionalShadows.md)
- [Volumetric Cloud Rendering roadmap](../../Roadmaps/Archive/2026-08/VolumetricCloudRendering.md)
