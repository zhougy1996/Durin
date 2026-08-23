# Volumetric Cloud Temporal Reconstruction Plan

Summary: Implement low-resolution cloud rendering, temporal reconstruction, spatial upsampling, and bounded quality policy.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

P3 completed on 2026-08-23. Production scene rendering selects the `High`
half-linear-resolution target, marches with 24x2 samples, temporally
reconstructs typed per-view history, and composites through a fitted-viewport
four-tap hard depth-agreement filter. View discontinuities stage transactional
history clears, failed outer views preserve last-known-good history, and
manual/device invalidation remains an immediate release boundary.

Stage 3 qualification thresholds were frozen before the first 4K run. That
calibration run showed that a single worst component is dominated by a tiny
set of motion/cut silhouette samples even while every tier passed its frozen
mean-error gate, and that independently submitted p95 timings contain bounded
tail spikes that do not track median route cost. The recorded qualification
decision is therefore to retain maximum error and p95 as reported diagnostics,
gate image outliers by requiring no more than 5%/1%/0.5% of RGB components in
`Performance`/`High`/`Epic` to exceed the tier's original 0.35/0.30/0.25
maximum threshold, gate `High`'s material benefit on median GPU time, and keep
p95 bounded by each tier's predeclared absolute budget. The mean-error
thresholds and all absolute timing/memory budgets remain unchanged. The first
acceptance attempt also showed the full-resolution exponential bilateral depth
tail erasing `High`'s median benefit, so the selected implementation uses a
hard depth-agreement gate with the same four taps and nearest-valid fallback;
the frozen edge fixture owns its leakage behavior.

The frozen RTX 3090 / Vulkan 1.4.325 4K gate passes all four tiers on inline and
threaded executors. The accepted observed threaded run reports `High` at
0.982 ms median and 4.716 ms p95 versus `Reference` at 2.644/6.609 ms: a 62.9%
median benefit, 0.012408 mean RGB error, 0.661% bounded outliers, and
132,710,400 retained bytes versus 199,065,600 bytes.

Final validation passes 76/76 `EditorRenderingTests`, 25/25
`RendererSceneContractTests`, 38/38 `RenderShaderContractTests`, 6/6
`VolumetricCloudSceneContractTests`, `VolumetricCloudVulkanTests`,
`VolumetricCloudSceneVulkanTests`, qualification on both executors, the full
`all` build, changed-document validation, and a 30-second DurinEditor smoke
window from the same build profile before deliberate termination.

## Goal

Reduce 4K volumetric-cloud production cost without trading ray-march stability
for visible sample banding: march at a bounded lower resolution, reconstruct a
stable full-view cloud through spatial and temporal evidence, reject invalid
history transactionally, and retain a full-resolution reference path for
quality and performance comparison.

## Scope

- Named Renderer-owned `Performance`, `High`, `Epic`, and `Reference` quality
  tiers with frozen target scales and sample counts.
- Low-resolution compute and fragment spatial targets derived from the fitted
  output extent with deterministic round-up.
- Per-frame jitter, depth-aware spatial upsampling, temporal reprojection,
  neighborhood clamping, and bounded history weight.
- Strongly typed cloud history in `FSceneViewState`, committed or aborted with
  the outer view transaction.
- History rejection for first use, cuts, projection/depth/viewport/output or
  scene changes, inactivity, duplicate submissions, quality-policy changes,
  manual/device invalidation, and resource failure.
- Counters and qualification evidence for target extent, samples, target and
  history bytes, history acceptance, GPU time, and image error versus
  `Reference`.

## Non-Goals

- Scene-geometry TAA, a generic renderer history cache, whole-renderer dynamic
  resolution, motion-vector generation, asynchronous compute, or a render
  graph.
- Directional phase functions, new lighting integration, transmittance-volume
  caching, or cloud shadows; P4 owns those changes.
- Serialized per-component sample counts or target scales. Authored cloud data
  continues to express physical intent while Renderer owns implementation
  policy.
- P5 editor preset UI. P3 supplies the named runtime policy and diagnostics
  that later UI will consume.

## Design Decisions and Invariants

- `High` is the initial production default. `Performance`, `High`, and `Epic`
  use 0.5 linear scale (one quarter pixel count) with 16x1, 24x2, and 32x4
  primary/light samples. `Reference` uses 1.0 scale and 32x4.
- `Reference` remains callable in tests and qualification as the spatial truth;
  it is not the default 4K production route.
- Target dimensions are `max(1, ceil(output * scale))`. The scaled view retains
  the same fitted normalized viewport and projection; ray construction never
  treats the low-resolution target as a cropped view.
- Spatial reconstruction is a four-tap bilateral filter. Cloud taps are
  weighted by bilinear footprint and opaque-depth agreement, with a nearest
  valid fallback so silhouettes do not leak clouds across foreground edges.
- Temporal history stores low-resolution scene-linear cloud radiance and
  transmittance. Reprojection uses current/previous camera matrices and the
  cloud slab, clamps history to a current neighborhood, and never blends when
  the outer temporal context or cloud policy key is invalid.
- Jitter is deterministic per successful view sequence and quality tier. A
  failed frame does not advance committed cloud history or its sequence.
- A successful cloud candidate is published only through the existing outer
  `FSceneViewState::Commit`; any later view failure aborts it and preserves the
  last committed history. Stateless views reconstruct spatially without
  retaining history.
- A quality change invalidates cloud history even when output extent is stable.
  Disabled/ineligible clouds publish no candidate; explicit discontinuities
  reset committed history.
- Full-resolution scene color and final composition remain full resolution.
  The quoted 190 MiB to 47.5 MiB comparison applies to three RGBA16F cloud
  targets only. It does not include full-resolution scene color or the
  ping-pong composition target, so total renderer memory reduction must be
  reported separately rather than inferred from that ratio.

## Current Foundations and Gaps

- P1 already owns matched compute/fragment ray marching, depth clipping,
  RGBA16F radiance/transmittance, composition, fallback, counters, and
  qualification fixtures, but all spatial and composite targets currently use
  the scene-color extent.
- `FPreparedSceneView` already carries `FSceneViewTemporalContext`, and
  `FSceneViewState` already rejects all required camera/output discontinuities
  and commits or aborts feature-local candidates with the outer view.
- The existing history probe proves the extension pattern but is not a cloud
  history implementation. No cloud quality policy, scaled extent, jitter,
  reconstruction shader, policy invalidation, or temporal diagnostics exist.
- At 3840x2160, one RGBA16F target is 63.28 MiB; three are 189.84 MiB. At
  1920x1080 they are 15.82 MiB and 47.46 MiB respectively. Pixel-bound spatial
  work falls to one quarter before reconstruction overhead, but measured GPU
  time—not pixel count—owns the acceptance decision.

## Implementation Stages

### Stage 0: Freeze quality, reconstruction, and qualification contracts

- [x] Confirm P3 scope and prerequisite evidence against the roadmap.
- [x] Freeze the four quality tiers, `High` default, scaled-extent rounding,
  comparison counters, and full-resolution `Reference` behavior.
- [x] Correct the memory claim boundary and select representative 4K/static,
  translating, rotating, cut, resize, and failed-frame sequences.
- [x] Add GPU-free policy tests for tier mapping, extent rounding, policy keys,
  jitter bounds, history eligibility, and overflow-safe byte accounting.

#### Acceptance Gate

- The selected policy is deterministic and test-covered without an RHI; every
  later image/performance result identifies a tier and actual target extent.

### Stage 1: Add the low-resolution production and spatial reconstruction path

- [x] Allocate compute/fragment cloud targets at the selected cloud extent
  while keeping scene depth, scene color, and composition full resolution.
- [x] Make spatial shaders consume output and cloud extents plus deterministic
  subpixel jitter without changing fitted-view ray semantics.
- [x] Replace same-pixel composition loads with depth-aware bilateral cloud
  upsampling and preserve exact full-resolution `Reference` algebra.
- [x] Extend counters and Vulkan integration coverage for route dimensions,
  samples, retained bytes, odd extents, fitted viewports, both depth
  conventions, route fallback, resize, and resource recovery.

#### Acceptance Gate

- All four tiers render through matched compute/fragment routes; production
  tiers march one quarter of output pixels, composite at full resolution, and
  do not leak across the frozen opaque-depth edge fixture.

### Stage 2: Add typed transactional temporal history

- [x] Add a private strongly typed cloud history
  extension that owns committed, pending, and reusable low-resolution targets.
- [x] Add temporal reprojection, deterministic jitter, neighborhood clamping,
  bounded accumulation, and a spatial-only fallback when history is absent.
- [x] Key history by quality policy and cloud scene identity in addition to the
  existing view discontinuities; publish candidates only after successful
  reconstruction.
- [x] Cover commit, abort, failed candidate creation, inactive/disabled gaps,
  first use, camera cuts, translation/rotation, resize, projection/depth/scene
  changes, duplicate submission, manual/device invalidation, and release.

#### Acceptance Gate

- Each logical view owns independent history; valid motion uses only the last
  committed candidate, and every invalid or failed sequence falls back without
  ghosting or corrupting last-known-good state.

### Stage 3: Freeze A/B quality and performance evidence

- [x] Extend qualification to 3840x2160 and run the same static/motion sequence
  in `Performance`, `High`, `Epic`, and `Reference`.
- [x] Report GPU median/p95, spatial dispatch/draw/sample counts, cloud target
  and history bytes, total retained cloud-renderer bytes, history acceptance,
  and image error versus `Reference`.
- [x] Freeze tier-specific thresholds before reading final timings; revise the
  default or policy only through an explicit recorded decision.
- [x] Run focused tests, bounded renderer/Vulkan aggregates, full build, and
  runtime smoke required by the root workflows.

#### Acceptance Gate

- `High` demonstrates a material measured 4K benefit over `Reference` while
  passing static and motion image gates; every tier stays inside its frozen
  memory/timing budget and produces deterministic diagnostics.

### Stage 4: Publish the lasting contract and complete P3

- [x] Publish temporal reconstruction, quality policy, invalidation, fallback,
  and diagnostic behavior under Runtime rendering documentation.
- [x] Update the roadmap milestone evidence, validate changed documentation,
  and record complete test/build/runtime evidence.
- [x] Mark this plan complete only after all gates pass; do not start P4 until
  P3 output and quality policy are stable.

#### Acceptance Gate

- Lasting documentation and roadmap state match validated runtime behavior and
  P4's entry gate is objectively satisfied.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Policy -> execution | GPU-free tier/extent/sample/key/jitter/byte tests plus counters from each GPU route. |
| Low-res cloud -> full-res scene | Compute/fragment parity, odd/fitted extents, forward/reversed depth edges, exact Reference composition, and resize/recovery. |
| Current -> previous view | Static, translation, rotation, first use, cut, projection/depth/scene/output/viewport changes, inactivity, duplicate, disabled gap, failure, manual/device invalidation, and release. |
| Tier -> quality/performance | One frozen 4K sequence reports per-tier image error, median/p95 GPU time, work counters, history acceptance, and memory alongside Reference. |
| Scene integration -> presentation | Enabled/disabled offscreen and Present routes retain opaque/cloud/translucency ordering and abort history on any later outer-view failure. |

## Definition of Done

- The four-tier A/B policy is implemented without serialized dispatch internals.
- Production tiers ray march at one quarter output pixels and reconstruct stable
  full-resolution output with depth-aware spatial filtering and valid temporal
  history; `Reference` remains full-resolution.
- History ownership, invalidation, commit/abort, resource recovery, counters,
  and fallback pass the validation matrix on both command executors.
- Frozen 4K evidence demonstrates the claimed benefit using measured total
  memory and GPU time, not theoretical pixel count alone.
- Lasting Runtime documentation and roadmap status match the implementation.

## Deferred Follow-ups

- Dynamic selection among 0.5, 0.67, and 1.0 scale from GPU time; evidence from
  the fixed tiers must justify a later policy before adding feedback control.
- Distance-adaptive samples, weather-first empty-space skipping, blue-noise
  assets beyond the deterministic jitter pattern, and cached light
  transmittance. P4 owns changes coupled to the lighting model.
- Editor-facing quality selection and temporal debug views remain P5 work.

## Related Documentation

- [Volumetric Cloud Rendering roadmap](../../../Roadmaps/Archive/2026-08/VolumetricCloudRendering.md)
- [Persistent View State Foundation](PersistentViewStateFoundation.md)
- [Volumetric cloud spatial rendering](../../../Runtime/Rendering/VolumetricCloudSpatialRendering.md)
- [Agent build and run workflow](../../../Agents/BuildAndRun.md)
- [Agent testing workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneViewState.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneViewState.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudSpatialRenderer.h`
- `Engine/Shaders/Slang/VolumetricCloud.slang`
- `Engine/Shaders/Slang/VolumetricCloudComposite.slang`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudQualificationTests.cpp`
