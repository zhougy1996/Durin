# Specular Antialiasing Plan

Summary: Add derivative-based specular antialiasing to the shared material surface path while preserving authored roughness, GBuffer layout, and forward/deferred parity.

Last reviewed: 2026-08-23

Status: Completed
Completed: 2026-08-23

## Current Status

All stages are complete. The shared surface shader uses variance
scale `0.15` and kernel threshold `0.20`, evaluates derivatives of the final
world-space shading normal before masked/Terrain coverage rejection, and
stores/consumes one effective perceptual roughness across GBuffer and retained
forward Lit paths. The derivative estimator and pure variance-to-roughness
filter now live in the dedicated `Material.SpecularAntialiasing` module, while
the shared base pass resolves the normal frame and effective roughness through
one pass-local surface-shading helper. Authored material state, the 256-byte
material ABI, shader cache identity, GBuffer layout, unlit output, and
shadow/depth paths are unchanged.

Implementation review replaced the planned new descriptor uniform with the
reserved `FSurfaceMaterialUniform::SurfaceParams.w` lane. That lane now carries
the default-enabled per-view A/B bit, avoiding descriptor-layout churn across
four geometry families while retaining the same ownership and failure
semantics. `MaterialTests` (74/74), `RendererSceneContractTests` (27/27),
`MaterialVulkanTests`, `SkeletalMeshRenderResourcesVulkanTests`,
`TerrainRenderVulkanTests`, `fast-all`, the full `all` build, the full
non-qualification native-test gate, documentation validation, and a 60-tick
hidden Vulkan editor smoke pass.

The enabled/disabled 32x32 GBuffer readback proves that valid varying-normal
pixels only broaden roughness and preserve AO, opacity, and flags. The frozen
four-family 1920x1080 isolated interval measured `79,040 ns` median and
`79,808 ns` p95 on the second run, versus the prior documented `78,096/79,136
ns`; the median increase is about `1.2%`, below the frozen `20%` incremental
budget and existing `350,000/500,000 ns` absolute gates.

Qualification measures the hybrid production route
from one synchronized 30-warm-up plus 120-measured frame batch instead of
combining intervals from independent runs. Strict-LIFO nested GPU timing allows
Scene to provide the outer reference while GBuffer, deferred, retained opaque,
volumetric cloud, sorted translucency, post-process, and shadow retain direct
intervals; Vulkan's bounded timing pool is 1,280 intervals, sufficient for the
1,200-query frozen matrix. Isolated GTAO and contact sweeps keep median hard
budgets and report p95 as characterization;
cross-batch p95 comparisons were removed because they measured unrelated
validation/driver scheduling tails rather than feature regressions.

Three consecutive split-interval qualification runs passed without reboot or
competing application load. The final run measured the synchronized production
total at `401,376 ns` median / `405,312 ns` p95: retained opaque/masked was
`7,184/7,872 ns`, the disabled volumetric-cloud boundary was `768/1,152 ns`,
and sorted translucency was `7,392/8,128 ns` median/p95.

The completed deterministic motion/distance matrix renders an eight-by-eight
alternating-normal Lit fixture at 192x192, translates it across nine subpixel
camera samples, and measures three projected scales (`1.0`, `0.875`, and
`0.75`) with FXAA both disabled and enabled. The frozen gate requires the
enabled temporal peak-luminance range to remain below 25% of its matched
disabled result. The worst case retained 22.18% of the disabled range (77.82%
reduction); the other five routes reduced it by 86.28% to 99.78%. The complete
12-image still matrix was inspected: enabled output broadens discontinuous
low-roughness peaks without changing silhouette or background coverage, and
the result is independent of FXAA. Because the development A/B bit is
intentionally absent from the normal viewport UI, final-image A/B evidence is
captured from the renderer's display output while a separate visible
window-backed Lit editor run validates the production Present route. This
replaces the originally planned in-window A/B manipulation without exposing a
new editor setting.

Qualification now rejects a production p95 above 125% of its same-batch median
as an unstable environment. Repository agent guidance records that independent
agents, worktrees, DevTool processes, and external GPU applications are outside
CTest's `durin-gpu` lock; their timing output is diagnostic only until repeated
in an exclusive quiet GPU lane.

The shader-ownership cleanup was revalidated after extraction:
`MaterialVulkanTests`, `SkeletalMeshRenderResourcesVulkanTests`,
`TerrainRenderVulkanTests`, `fast-all`, and the incremental full `all` build
passed. The later synchronized qualification supersedes the earlier aggregate
failures and passes the extracted module, Specular-AA assertions, production
median/p95 gates, and split retained-scene intervals.

Final validation on 2026-08-23 passed `MaterialTests` (74/74),
`RendererSceneContractTests` (27/27), `MaterialVulkanTests`,
`SkeletalMeshRenderResourcesVulkanTests`, `TerrainRenderVulkanTests`, the
extended `GBufferQualificationTests` (1/1), `fast-all`, the incremental full
`all` build, and the complete non-qualification native-test gate. Changed
documentation and all-plan validation pass. Two visible Vulkan editor runs
against `Sandbox/Sandbox.dproject` completed normally after 300 and 600 ticks;
the latter supplied an on-screen Lit viewport capture and clean exit.

## Goal

Reduce unstable, undersampled GGX highlights caused by rapidly varying
geometric or normal-mapped shading normals while preserving authored material
data, diffuse inputs, opacity behavior, the current GBuffer layout, and
forward/deferred surface parity.

## Scope

- Add one shared Slang implementation of derivative-based specular
  antialiasing for standard-Lit material surfaces.
- Filter perceptual roughness from the final world-space shading normal after
  tangent-space RNM composition and basis transformation.
- Apply the effective roughness to direct directional/local lighting and
  split-sum environment lighting.
- Store the effective roughness in the existing `GBufferSurface.R` channel so
  deferred consumers use the same filtered value as retained forward Lit
  surfaces.
- Cover StaticMesh, SplineMesh, SkeletalMesh, and Terrain geometry through the
  shared base-pass fragment implementation, including opaque, masked, and Lit
  translucent routes where they are supported.
- Add a renderer-owned, development/qualification-only per-view A/B control.
  Production views default to enabled; no editor-facing setting is added.
- Add deterministic numerical, Vulkan rendering, cross-family parity, visual
  stability, and GPU-cost validation.
- Update the lasting material, StaticMesh, and GBuffer contracts after the
  implementation is qualified.

## Non-Goals

- Replacing FXAA or implementing TAA, temporal accumulation, motion vectors,
  camera jitter, history rejection, or temporal upscaling.
- Adding MSAA, supersampling, stochastic sampling, or a new post-process pass.
- Changing GGX distribution, visibility, Fresnel, split-sum IBL, minimum
  perceptual roughness, or tone/display mapping semantics.
- Adding Toksvig/LEAN/CLEAN prefiltered normal-map payloads, changing texture
  import or mip generation, or introducing a new texture usage.
- Changing material assets, material-instance inheritance, authored roughness
  meaning, render-layout identity, static material permutations, or adding a
  per-material Specular AA parameter.
- Expanding the GBuffer, changing attachment formats, or retaining a second
  authored-roughness attachment.
- Filtering shadow maps, diffuse lighting, emissive, ambient occlusion,
  opacity, opacity masks, unlit surfaces, editor assistance, or debug geometry.
- Claiming elimination of all temporal shimmer; geometry undersampling,
  visibility aliasing, and post-process aliasing remain separate concerns.

## Design Decisions and Invariants

- The production method is screen-space normal-variance filtering. For
  authored perceptual roughness `r`, normalized world-space shading normal
  `n`, variance scale `s`, and bounded kernel threshold `k`, the selected form
  is:

  ```text
  variance = s * (dot(ddx(n), ddx(n)) + dot(ddy(n), ddy(n)))
  kernel   = min(2 * max(variance, 0), k)
  filtered = clamp(sqrt(r * r + kernel), 0.045, 1)
  ```

  `s` is frozen at `0.15` and `k` at `0.20`; later stages must not tune them
  independently per pass, primitive family, resolution, or material.
- Filtering occurs after `EvaluateMaterialNormalFrame`, never on the encoded
  tangent-space normal or decoded/quantized GBuffer normal. Deferred lighting
  must not estimate derivatives from neighboring GBuffer pixels because that
  would mix silhouettes, material boundaries, cleared records, and UNORM
  quantization into the material variance.
- The filtered value is an effective shading roughness, not an authored
  material mutation. Material proxies, editor values, serialization, asset
  dependencies, and render-layout cache keys remain unchanged.
- Opaque/masked standard-Lit geometry writes effective roughness to the
  existing GBuffer channel. Retained forward Lit surfaces calculate the same
  value before all direct and environment specular evaluation. No downstream
  deferred pass applies the filter a second time.
- The existing energy-conserving PBR evaluation receives the effective
  roughness coherently. Base color, metallic, shading normal, geometric normal,
  AO, emissive, opacity, mask threshold, depth, and flags are bitwise or
  tolerance-equivalent to the unfiltered route.
- Zero normal variance is an identity operation after the existing roughness
  clamp. Increasing finite variance never decreases roughness. Invalid inputs
  produce a finite bounded result and never create NaN/Inf Scene Color or
  GBuffer records.
- A fixed engine policy, not a material feature, owns the controls. The A/B
  switch is carried by immutable `FSceneViewModeSettings` and the reserved
  `FSurfaceMaterialUniform::SurfaceParams.w` lane shared by geometry and
  retained-forward routes. It adds no descriptor or material-layout identity,
  is not serialized, and is not exposed in the normal viewport UI. This
  replaces the initially planned standalone uniform after implementation
  inspection showed the reserved ABI lane was the narrower transport.
- The A/B-disabled route must retain the currently qualified image semantics.
  The enabled route is the production default after the final gate passes. A
  resource or binding failure follows existing renderer failure behavior; it
  must not silently shade one primitive family or one pass without filtering.
- Explicit derivative use must be valid around masked and Terrain coverage
  rejection. Stage 0 records whether the calculation must precede `discard`;
  implementation follows that single ordering in both fragment entry points.
- Shadow/depth-only shaders and unlit early-out paths do not calculate normal
  variance and acquire no new surface-quality binding.
- Debug views describe the value they show. `GBufferSurface.R` and combined
  material-input diagnostics show effective roughness when filtering is on;
  authored roughness remains observable through material data/tests rather
  than a new resident attachment.

## Current Foundations and Gaps

- `Material/SurfaceMaterial.slang` already owns RNM normal composition,
  tangent-frame validation, the final world-space shading/geometric normals,
  and the `[0.045, 1]` authored roughness clamp.
- `StaticMeshBasePass.slang` is the shared fragment surface for StaticMesh,
  SplineMesh, SkeletalMesh, and Terrain vertex domains. Its
  `GeometryFragmentMain` writes roughness into `GBufferSurface.R`, while
  `FragmentMain` forwards roughness to direct and environment lighting.
- `Lighting/SurfaceLighting.slang` and `Lighting/PBRLighting.slang` already
  centralize direct and split-sum environment evaluation. Their BRDF formulas
  need no algorithm change.
- The minimal GBuffer already stores both shading and geometric normals and one
  roughness byte. No transport expansion is required, but the lasting contract
  currently describes the channel as material roughness rather than effective
  shading roughness.
- CPU PBR references and material/GBuffer Vulkan tests exist, including a
  low-roughness sweep and the four-family GBuffer qualification fixture. They
  do not yet provide a CPU oracle for the variance-to-roughness transform or a
  high-frequency normal stability metric.
- Current documentation explicitly defers direct-specular antialiasing. It
  must be revised only after the implementation and qualification evidence
  land.
- The landed implementation supplies the renderer-owned view control,
  deterministic varying-normal A/B readback, and frozen GPU budget. The
  visible motion/distance operator matrix remains the outstanding evidence.

## Implementation Stages

### Stage 0: Freeze the algorithm and qualification baseline

- [x] Add or identify a deterministic rendered fixture with a low-roughness
  standard-Lit surface, controlled flat and high-frequency normal inputs,
  direct-light and IBL highlights, a grazing-angle case, and a stable pixel
  region of interest.
- [x] Capture the disabled/enabled varying-normal GBuffer baseline and retain
  the existing material roughness/direct/IBL output controls. The visible
  distance/camera-motion luminance sweep remains the Stage 3 operator gate.
- [x] Run a bounded shader spike over candidate `s` and `k` values. Freeze one
  pair that reduces the chosen high-frequency stability metric without
  broadening the flat-normal control or high-authored-roughness control beyond
  the recorded tolerance.
- [x] Verify explicit derivative behavior around masked rejection, Terrain
  dither rejection, helper lanes, silhouettes, and two-sided normal
  orientation. Record whether normal-frame/filter evaluation precedes coverage
  rejection and the measured cost of that ordering.
- [x] Freeze numerical tolerances, the required stability improvement, allowed
  still-image deviation for controls, and an incremental GPU-time budget
  against the existing 1920x1080 GBuffer qualification route. Do not begin
  production integration without these values.
- [x] Record the selected uniform layout and A/B ownership. Confirm that it
  neither changes material render layout/cache identity nor adds a shadow or
  unlit binding.

#### Acceptance Gate

- The fixture and unfiltered baseline are reproducible; `s`, `k`, derivative
  ordering, image thresholds, and the GPU budget are recorded in this plan.
- The selected result visibly/numerically reduces high-frequency highlight
  instability while the flat-normal and already-rough controls remain within
  their frozen tolerances.
- No open decision remains about pass ownership, GBuffer storage, A/B control,
  or discard ordering.

### Stage 1: Implement the shared filtering contract

- [x] Add a focused Slang module that computes finite bounded normal variance
  and effective perceptual roughness using the frozen formula and controls.
- [x] Add a matching CPU reference that accepts authored roughness and explicit
  normal variance for deterministic contract tests; keep it a validation
  oracle rather than a second runtime shading owner.
- [x] Cover minimum/maximum roughness, zero variance, monotonic variance,
  threshold saturation, out-of-range inputs, and non-finite inputs.
- [x] Carry the renderer-owned per-view qualification A/B option through the
  reserved material-uniform lane with production-enabled defaults and existing
  size/offset assertions.
- [x] Ensure disabled filtering avoids derivative work through a view-uniform
  branch and returns the existing clamped roughness exactly.

#### Acceptance Gate

- CPU and shader contracts use the same frozen equation, constants, clamps,
  and invalid-input behavior.
- Zero variance and A/B-disabled results equal the pre-existing roughness
  contract; finite variance is monotonic and never exceeds `1.0`.
- Shader reflection and uniform-layout tests cover every intended consumer and
  reject missing/incompatible bindings deterministically.

### Stage 2: Integrate geometry and retained-forward shading

- [x] Compute effective roughness exactly once from the final shading normal in
  `GeometryFragmentMain`, following the Stage 0 coverage/discard decision, and
  write it to `GBufferSurface.R`.
- [x] Compute the same effective roughness for retained Lit forward surfaces
  and pass it to directional, local-light, and environment-light evaluation.
- [x] Keep unlit, depth-only, shadow-depth, opacity/mask, and editor-assistance
  paths unchanged and free of the new work.
- [x] Verify opaque and masked StaticMesh, SplineMesh, SkeletalMesh, and Terrain
  geometry all inherit the shared behavior without per-family shader forks.
- [x] Verify Lit translucent StaticMesh/SplineMesh forward output follows the
  same filtering contract and preserves blend/depth/sort behavior.
- [x] Update GBuffer surface and combined-input diagnostics to label and compare
  effective roughness correctly under enabled and disabled A/B routes.

#### Acceptance Gate

- GBuffer readback matches the CPU effective-roughness oracle within the
  existing UNORM8 tolerance for flat, varying-normal, thresholded, and disabled
  cases.
- Forward and deferred fixtures consume tolerance-equivalent effective
  roughness and produce no second application of the filter.
- All four geometry families pass the same normal-frequency sweep; masked,
  two-sided, grazing, and Terrain dither boundaries show no new seams, halos,
  NaN/Inf output, or coverage changes.
- Unlit color, shadow/depth coverage, opacity, and existing material-input
  channels remain equivalent to the pre-change baselines.

### Stage 3: Qualify image stability and cost

- [x] Extend `MaterialTests` with the CPU variance/roughness reference and
  invariant cases.
- [x] Retain and pass the existing `MaterialVulkanTests` coverage for flat and
  textured normals, low/high roughness, direct/IBL output, opaque/masked, and
  Lit translucent paths; use the focused GBuffer fixture for enabled/disabled
  transport evidence.
- [x] Extend the four-family GBuffer/render fixtures where necessary to prove
  identical policy for StaticMesh, SplineMesh, SkeletalMesh, and Terrain.
- [x] Measure the frozen motion/distance sweep with FXAA both enabled and
  disabled. Specular AA must meet the Stage 0 stability threshold independent
  of the final spatial edge filter.
- [x] Exercise automated silhouette, UV seam, mirrored tangent, degenerate tangent,
  back-face/two-sided, mask edge, Terrain LOD/dither, near-camera, and grazing
  cases for over-broadening or discontinuities.
- [x] Profile the production-enabled route with the 1920x1080 GBuffer
  qualification and retained-forward fixture. Meet the frozen incremental
  median budget and synchronized production-route median/p95 absolute gates;
  retain isolated feature p95 as characterization rather than comparing tails
  from independently scheduled validation batches.
- [x] Exercise main, auxiliary, preview, thumbnail, Present, offscreen, resize,
  alternating extent, shader reload, device invalidation, and shutdown routes
  affected by the new per-view uniform.

#### Acceptance Gate

- Automated ROI metrics meet the frozen instability reduction and control
  tolerances at every required resolution/camera sample.
- Operator captures show materially steadier low-roughness highlights without
  objectionable loss of authored broad highlights or new boundary artifacts.
- Material, GBuffer, geometry-family, lifecycle, and qualification tests pass;
  measured GPU cost remains within the Stage 0 budget and existing gates.

### Stage 4: Consolidate contracts and complete validation

- [x] Update the Material System contract to describe filtered effective
  roughness and remove its future-policy statement.
- [x] Update the GBuffer contract so `GBufferSurface.R`, debug views, readback
  tolerances, and forward/deferred parity explicitly refer to effective
  roughness while authored material data remains unchanged.
- [x] Update Static Mesh Rendering with the shared geometry-family and retained
  forward behavior; link rather than duplicate the numerical formula.
- [x] Update this plan's status, checklists, frozen constants, measurements,
  visual evidence, and completion date from actual results.
- [x] Run changed-document and all-plan validation, the focused test targets,
  the required bounded Vulkan domains, `fast-all`, the full `all` build, and
  the full native-test gate because the shared surface shader affects four
  production primitive families and both lighting paths.
- [x] Run the real Vulkan editor smoke, inspect the frozen final-output visual
  matrix, and validate the window-backed Lit Present route separately; the
  development A/B bit remains intentionally absent from the viewport UI.

#### Acceptance Gate

- Long-lived documentation is authoritative and contains no competing
  plan-only runtime contract.
- Focused, bounded integration/qualification, `fast-all`, full build, full
  native-test, documentation, and real-editor validation all pass with exact
  evidence recorded.
- The plan is marked completed only after every required visual, numerical,
  lifecycle, and performance gate passes.

## Validation Matrix

| Area | Required cases | Evidence |
| --- | --- | --- |
| Numerical contract | Zero/increasing/excess/invalid variance; min/max/invalid authored roughness | CPU/Slang parity and invariant tests |
| Material selectivity | Flat versus high-frequency normal; low versus high roughness; direct, IBL, combined | Enabled/disabled Vulkan captures and ROI metrics |
| Surface routes | Opaque, masked, Lit translucent, unlit control | GBuffer readback and forward/deferred captures |
| Geometry families | StaticMesh, SplineMesh, SkeletalMesh, Terrain | Shared-policy integration captures/readbacks |
| Boundaries | Silhouette, UV seam, mirrored/degenerate tangent, two-sided, mask edge, Terrain dither/LOD, grazing | Automated cases where stable plus operator matrix |
| View/output | Main, auxiliary, preview, thumbnail; Present/offscreen; resize and alternating extent | Integration and lifecycle suites |
| AA interaction | FXAA off/on with identical material/camera sweep | Matched still and motion captures |
| Recovery | Shader reload, resource retry, device invalidation, shutdown | Existing recovery/lifecycle tests extended for the new uniform |
| Performance | Disabled baseline and enabled production at frozen 1920x1080 qualification point; retained-forward fixture | Median/p95 GPU intervals against Stage 0 and existing gates |
| Regression | Focused material/GBuffer/primitive tests, bounded Vulkan domains, `fast-all`, full build, full native-test gate, editor smoke | Recorded command receipts and smoke result |

## Definition of Done

- Production standard-Lit surfaces use one qualified derivative-based
  Specular AA policy across GBuffer and retained-forward rendering.
- The frozen normal-frequency fixture meets the recorded stability improvement
  without violating flat-normal, high-roughness, boundary, or performance
  thresholds.
- Authored material state and ABI, GBuffer size/format, shadow/depth/coverage,
  unlit output, and editor assistance remain unchanged.
- Effective roughness is finite, bounded, applied once, and shared by direct
  and environment lighting for every supported geometry family.
- The production default is enabled, the qualification A/B route remains
  development-only, and failure cannot silently create mixed filtered and
  unfiltered production output.
- Automated tests, performance qualification, operator visual validation,
  full build/test gates, editor smoke, and documentation validation pass.
- Lasting behavior is documented under Runtime Rendering, this plan contains
  exact completion evidence, and lifecycle metadata is ready for archival.

## Deferred Follow-ups

- Normal-map mip prefiltering such as Toksvig, LEAN, or CLEAN if the spatial
  derivative method leaves texture-frequency aliasing that cannot be solved
  within its artifact or cost limits.
- Per-material opt-out or strength controls only after a concrete content case
  proves the fixed engine policy is insufficient; such controls require a
  separate material-schema and cache-identity decision.
- TAA/temporal upscaling and temporal view history for residual motion shimmer,
  visibility aliasing, and subpixel geometry.
- Specular occlusion, multiple-scattering energy compensation, area lights,
  anisotropy, clear coat, and other BRDF extensions.
- Generalizing the policy beyond standard-Lit surfaces when a second shading
  model lands with a concrete filtering requirement.

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Minimal GBuffer Contract](../Runtime/Rendering/GBuffer.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Deferred Directional Lighting](../Runtime/Rendering/DeferredDirectionalLighting.md)
- [HDR Scene Color and Display Mapping](../Runtime/Rendering/HDRSceneColorAndDisplayMapping.md)
- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)

## Related Code

- `Engine/Shaders/Slang/Material/SurfaceMaterial.slang`
- `Engine/Shaders/Slang/Material/SpecularAntialiasing.slang`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Shaders/Slang/Lighting/SurfaceLighting.slang`
- `Engine/Shaders/Slang/Lighting/PBRLighting.slang`
- `Engine/Shaders/Slang/Material/GBufferDecode.slang`
- `Engine/Shaders/Slang/GBufferDebug.slang`
- `Engine/Source/Runtime/Renderer/Public/PBRLighting.h`
- `Engine/Source/Runtime/Renderer/Private/PBRLighting.cpp`
- `Engine/Source/Runtime/Renderer/Public/GBufferContract.h`
- `Engine/Source/Runtime/Renderer/Private/GBufferContract.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SurfaceMaterial.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SurfaceMaterial.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferDebugRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialRenderingTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
