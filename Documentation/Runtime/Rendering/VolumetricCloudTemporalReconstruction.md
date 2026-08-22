# Volumetric Cloud Temporal Reconstruction

Summary: Defines Renderer-owned cloud quality tiers, low-resolution spatial reconstruction, transactional per-view history, invalidation, diagnostics, and qualification budgets.

Modules: Renderer

Last reviewed: 2026-08-23

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

The deterministic eight-sample Halton jitter is selected from the successful
view sequence and the policy key. A failed outer view does not advance that
sequence. `Reference` has a one-sample zero-jitter pattern.

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
texture. No generic renderer cache or Engine object owns these resources.

Temporal reconstruction intersects the current ray with the cloud slab,
projects its representative midpoint through the previous view-projection
matrix, samples the last committed history, clamps that value to the current
center-plus-four-axis neighborhood, and blends with the tier's bounded weight. Missing or
rejected history produces a spatial-only candidate.

The candidate is not visible to the next frame until the outer view commits.
Commit atomically publishes current fitted metadata and cloud history. Abort
recycles the pending texture and preserves the prior committed texture and
policy. A view discontinuity stages a pending clear: a failed discontinuous
frame can abort without destroying last-known-good history, while a successful
disabled frame commits the clear. Manual and device invalidation are explicit
hard resets and release all retained history immediately.

History is rejected for first use, camera cuts, scene/output/viewport/projection
or depth-convention changes, inactivity, duplicate submissions, policy or cloud
identity changes, missing state, and failed candidate creation. Camera
translation and rotation remain eligible because reprojection consumes the
previous committed matrices. A disabled or stateless view takes the spatial
fallback and retains no candidate.

## Diagnostics, memory, and failure

Per-view diagnostics identify tier, route/reason, output and actual target
extent, dispatch/draw counts, primary/light sample work, active target bytes,
renderer-retained bytes, history bytes, temporal draws, and accepted/rejected
history. All byte arithmetic saturates rather than wrapping.

At 3840x2160 one `RGBA16_FLOAT` target is 66,355,200 bytes and a half-linear
target is 16,588,800 bytes. The qualified production steady state retains two
low-resolution route targets, the full-resolution composite target, and two
history textures: 132,710,400 bytes total. `Reference` retains three
full-resolution renderer targets and no history: 199,065,600 bytes. Both remain
under the 192 MiB renderer/cloud qualification ceiling; these values do not
include unrelated scene-color or depth ownership.

Resource or shader failure returns the current spatial cloud when possible and
publishes no temporal candidate. An outer abort leaves the prior committed
history intact. The next successful creation can reuse or replace scratch state
without a device-idle wait. Renderer shutdown and device invalidation release
route, composite, temporal-pipeline, and per-view history resources.

## Qualification

`VolumetricCloudVulkanTests` covers target creation failure/retry, compute and
fragment fallback, fitted composition, the opaque-depth edge fixture, temporal
candidate creation failure followed by recovery, policy rejection, abort
preservation, device invalidation, and release. The spatial qualification matrix
adds odd fitted extents and both forward and reversed depth conventions.

On the frozen NVIDIA GeForce RTX 3090 / Vulkan 1.4.325 gate, the 4K complete
route uses 12 warm-up and 48 measured frames per tier and reports median/p95 GPU
time, work, target/history/total bytes, history acceptance, and six-frame
static/translation/rotation/cut image error against `Reference`. The accepted
2026-08-23 threaded run reported:

| Tier | Median / p95 | Mean RGB error | Outlier fraction | Total retained |
| --- | ---: | ---: | ---: | ---: |
| `Performance` | 0.419 / 1.531 ms | 0.052072 | 3.419% | 132,710,400 B |
| `High` | 0.982 / 4.716 ms | 0.012408 | 0.661% | 132,710,400 B |
| `Epic` | 1.658 / 8.367 ms | 0.002340 | 0.044% | 132,710,400 B |
| `Reference` | 2.644 / 6.609 ms | 0 | 0% | 199,065,600 B |

`High` is 62.9% below `Reference` median cost. Absolute median budgets are
16/20/24/48 ms and p95 budgets are four-thirds of those values. Mean RGB error
budgets are 0.08/0.06/0.04 for production tiers. The reported maximum remains a
diagnostic; the bounded gate permits at most 5%/1%/0.5% of RGB components above
0.35/0.30/0.25 for `Performance`/`High`/`Epic`.

## Related documentation

- [Persistent view state](PersistentViewState.md)
- [Volumetric cloud spatial rendering](VolumetricCloudSpatialRendering.md)
- [Volumetric cloud scene contract](VolumetricCloudSceneContract.md)
- [Volumetric Cloud Rendering roadmap](../../Roadmaps/VolumetricCloudRendering.md)
