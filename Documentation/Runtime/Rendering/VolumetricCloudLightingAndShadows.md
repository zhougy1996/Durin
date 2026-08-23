# Volumetric Cloud Lighting and Shadows

Summary: Defines production cloud single scattering, self-transmittance, ambient contribution, directional receiver visibility, fallback, lifetime, and qualification budgets.

Modules: Renderer

Last reviewed: 2026-08-23

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
view. P4 deliberately has no cross-frame shadow reuse or camera-dependent
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

Fragment and compute shadow target families each retain at most 16 MiB. A 4K
target is 8,294,400 bytes; retaining both route families is 16,588,800 bytes.
Targets and shader payloads use Renderer resource generations and
complete-or-last-known-good publication. Manual/device invalidation and
shutdown release shadow targets, views, and pipelines without a device-idle
wait.

## Qualification

The frozen gate is NVIDIA GeForce RTX 3090 / Vulkan 1.4.325 at 3840x2160. It
runs six deterministic frames per tier with static, translated, rotated, and
cut cameras, and executes explicit inline and threaded lanes. Production tiers
render the cloud body at 1920x1080 and reconstruct temporally; receiver
visibility remains 3840x2160. The accepted image metrics against `Reference`
are stable across both executors:

| Tier | Mean RGB error | Outlier fraction | Shadow samples | Total retained |
| --- | ---: | ---: | ---: | ---: |
| `Performance` | 0.0348179 | 1.31761% | 33,177,600 | 149,299,200 B |
| `High` | 0.0162224 | 0.76421% | 49,766,400 | 149,299,200 B |
| `Epic` | 0.0031851 | 0.0430464% | 66,355,200 | 149,299,200 B |
| `Reference` | 0 | 0% | 66,355,200 | 215,654,400 B |

The complete-route median/p95 budgets are 20/32, 26/40, 32/48, and 60/80 ms.
Shadow-only median/p95 budgets are 4/32, 6/32, 8/32, and 12/32 ms. The accepted
2026-08-23 inline run observed complete-route medians of 2.589, 2.537, 14.216,
and 9.498 ms and shadow medians of 0.694, 0.912, 1.141, and 1.198 ms. The
threaded run observed complete-route medians of 1.876, 14.279, 15.136, and
17.925 ms and shadow medians of 0.524, 0.761, 0.959, and 0.829 ms. Every median,
p95, image, work, and 224 MiB complete-retention gate passed.

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
