# Volumetric Cloud Temporal Reconstruction

Summary: Defines Renderer-owned cloud quality tiers, low-resolution spatial reconstruction, feature-owned per-view history, invalidation, diagnostics, and qualification budgets.

Modules: RenderCore, Renderer

Last reviewed: 2026-10-29

## Quality policy and target extent

Renderer owns four named implementation tiers. Authored cloud components retain
physical intent and do not serialize target scale or dispatch sample counts.

| Tier | Linear scale | Primary samples | Light samples | Temporal history weight |
| --- | ---: | ---: | ---: | ---: |
| `Performance` | 0.5 | 16 | 1 | 0.88 |
| `High` | 0.5 | 24 | 2 | 0.90 |
| `Epic` | 0.5 | 32 | 4 | 0.92 |
| `Reference` | 1.0 | 32 | 4 | 0.00 |

`High` is the production default. Target dimensions are
`max(1, ceil(output * scale))`; integer calculation rounds each dimension up
without overflow. The fitted output viewport is mapped proportionally into the
cloud target, while ray construction continues to use the full output
projection. Production tiers therefore march one quarter of the output pixels,
including for odd extents and non-zero fitted viewport origins. `Reference`
preserves the exact full-resolution spatial route.

The public per-view policy lives in `FSceneViewSettings::VolumetricCloud` as
`EVolumetricCloudQuality` plus `EVolumetricCloudDebugMode`. Invalid submitted
values canonicalize to `High` and `Lit`. The settings are copied into an
immutable scene view; they are not component properties and do not serialize
with world content. This allows main, auxiliary, fitted, offscreen, and Present
views to choose policy independently.

The deterministic eight-sample Halton jitter is selected from the successful
cloud reconstruction sequence and the policy key. A failed cloud reconstruction
does not advance that sequence; a later outer-view failure does not undo it. `Reference` has a one-sample zero-jitter pattern.

## Spatial reconstruction

Scene color, opaque depth, the composition target, and presentation remain at
full output resolution. Production cloud radiance/transmittance is reconstructed
during composition with a four-tap bilinear footprint. A tap contributes only
when its corresponding full-resolution opaque device depth agrees with the
destination depth within `1e-4`; when no tap agrees, the nearest cloud sample is
used. This hard bilateral gate prevents cloud leakage across frozen opaque-depth
silhouettes without paying an exponential operation per tap.

Pixels outside the fitted viewport retain scene color. Full-resolution
`Reference` bypasses filtering and preserves the exact algebra documented in
[Volumetric cloud spatial rendering](VolumetricCloudSpatialRendering.md).

## Temporal history and transaction boundary

Each opted-in logical view owns one `FVolumetricCloudViewHistory` inside its
private `FSceneViewState`. History stores low-resolution scene-linear
`RGBA16_FLOAT` radiance/transmittance and owns committed, pending, and one spare
texture and the projection/extent compatibility metadata paired with it.
The per-reconstruction context carries only the previous view-projection matrix,
current output size, sequence, and validity diagnostics. No generic renderer
cache or Engine object owns these resources.

Temporal reconstruction intersects the current ray with the cloud slab,
projects its representative midpoint through the previous view-projection
matrix, samples the last committed history, clamps that value to the current
center-plus-four-axis neighborhood, and blends with the tier's bounded weight. Missing or
rejected history produces a spatial-only candidate.

Cloud reconstruction publishes the candidate texture and its fitted camera
metadata together after recording the reconstruction commands. Later composite,
post-process, or presentation failures do not roll back valid cloud history.
Abort recycles only unfinished candidates and preserves the last published
texture, camera, and policy. A view discontinuity stages a pending clear;
successful reconstruction replaces it with a spatial-only candidate. A disabled
consumer publishes nothing, and rechecks age and compatibility when enabled.
Manual and device invalidation release retained textures immediately.
History expires after more than 120 render frames; other view submissions in the
same frame do not contribute to its age.

History is rejected for first use, camera cuts, scene/output/viewport/projection
or depth-convention changes, inactivity, duplicate submissions, policy, cloud
identity, or selected-light radiance changes, missing state, and failed
candidate creation. Camera
translation and rotation remain eligible because reprojection consumes the
previous committed matrices. A disabled or stateless view takes the spatial
fallback and retains no candidate.

## Diagnostics, memory, and failure

Per-view diagnostics identify tier, route/reason, output and actual target
extent, dispatch/draw counts, primary/light sample work, active target bytes,
renderer-retained bytes, history bytes, temporal draws, and accepted/rejected
history. All byte arithmetic saturates rather than wrapping.

The completed values are also reduced into the bounded
`FSceneViewVolumetricCloudStatistics` snapshot. It carries exact work and byte
counters but intentionally reports GPU timing unavailable: editor observation
does not add a query, readback, render-thread flush, or separate view cache.
Cloud debug modes reuse the normal composite and production intermediates, add
no retained target, and leave history evaluation and feature publication
unchanged.

At 3840x2160 one `RGBA16_FLOAT` target costs 66,355,200 bytes and a half-linear
target costs 16,588,800 bytes. Spatial and composite targets use the shared
[RDG allocator](../Pipeline/RendererFramePreparation.md#resource-lifetime-classes); committed,
pending, and spare history stay outside that allocator. Actual retained bytes
depend on demanded routes, descriptor reuse, and live histories, rather than
fixed per-route cache quotas. Complete-cloud qualification budgets are owned by
[Cloud Lighting and Shadows](VolumetricCloudLightingAndShadows.md#qualification).

Resource or shader failure returns the current spatial cloud when possible and
publishes no temporal candidate. Outer cleanup retains any cloud result already published, and leaves the prior
history intact when reconstruction produced no candidate. The next successful creation can reuse or replace scratch state
without a device-idle wait. Renderer shutdown and device invalidation release
route, composite, temporal-pipeline, and per-view history resources.

## Qualification

`VolumetricCloudVulkanTests` covers target creation failure/retry, compute and
fragment fallback, fitted composition, the opaque-depth edge fixture, temporal
candidate creation failure followed by recovery, policy rejection, publication surviving outer cleanup, unfinished-candidate
preservation, device invalidation, and release. The spatial qualification matrix
adds odd fitted extents and both forward and reversed depth conventions.

The 4K image matrix uses six static/translation/rotation/cut frames against
`Reference`. Production-tier mean RGB errors are bounded by 0.08/0.06/0.04;
at most 5%/1%/0.5% of RGB components may exceed 0.35/0.30/0.25 for
`Performance`/`High`/`Epic`. Maximum error remains diagnostic. Reference accepts
no history and has zero reference-relative error. Production tiers require
four accepted and two rejected histories, with at most 32 MiB retained history
in the fixture.

Current complete-route timing and memory gates are defined by
[Cloud Lighting and Shadows](VolumetricCloudLightingAndShadows.md#qualification).
The original spatial/temporal calibration, rejected filter, and measured runs
remain in the [Temporal Reconstruction plan](../../../Plans/Archive/2026-08/VolumetricCloudTemporalReconstruction.md).

## Related documentation

- [Persistent view state](../Scene/PersistentViewState.md)
- [Volumetric cloud spatial rendering](VolumetricCloudSpatialRendering.md)
- [Volumetric cloud scene contract](VolumetricCloudSceneContract.md)
- [Volumetric cloud lighting and shadows](VolumetricCloudLightingAndShadows.md)
- [Volumetric Cloud Rendering roadmap](../../../Roadmaps/Archive/2026-08/VolumetricCloudRendering.md)
