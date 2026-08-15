# Directional Shadow PCF Quality Tiers Plan

Summary: Add deterministic directional-shadow PCF quality tiers with exact kernels, bounded footprints, causal diagnostics, and measured default selection.

Last reviewed: 2026-08-14

Status: Archived
Completed: 2026-08-14

## Current Status

Q0 completed the
[Directional Shadow Diagnostics And Bias Plan](DirectionalShadowDiagnosticsAndBias.md).
The production path owns one 2048x2048 D32 map, one hardware linearly filtered
`LessOrEqual` comparison at the receiver, a bounded texel/orientation-aware bias
policy, immutable per-view diagnostic identity, exact valid/defective geometry
controls, and separate Shadow Depth/Scene Color timing.

The Q0 qualification package contains the fixed-policy entry baseline, 13
selected-policy Lit captures, seven causal diagnostic captures, exact disabled
and Unlit references, and sub-texel camera/light motion. Its RTX 3090 fixture
records an 11,776 ns combined shadow increment at 1920x1080, 16,777,216 logical
and backend target bytes, and 22, 18, and 58 changed motion pixels at channel
tolerance two. These results are the Q1 entry baseline.

Stage 1 implementation is complete. The checked-in
`DirectionalShadowQ1` package adds exact Low edge-staircase, 43-degree diagonal,
thin-caster, Masked authored-opening, guard-boundary, and sub-pixel motion
captures without changing production filtering. All eight captures are asserted
by exact xxHash128 values; the new camera/light comparisons change 76 and 125 of
66,049 pixels at channel tolerance two, below the frozen 132-pixel limit. The
existing Q0 hashes and 22, 18, and 58 motion values remain exact.

L0, M1, and H1 are frozen as the only candidates. Their comparison counts are
1, 9, and 25; effective radii are 0.5, 1.5, and 2.5 texels; guards are 2, 2,
and 3 texels. H1 remains the literal 5x5 tent, so no optimized alternative or
equivalence error is open. Every invalid tap contributes fully lit normalized
weight, and offsets come only from runtime texture dimensions.

The RTX 3090, driver 591.86, Vulkan 1.4.325 timing fixture remains fixed at
1920x1080, 30 warm-up frames, and 120 measured frames. Medium may add at most
200,000 ns median Scene Color time over Low and High at most 400,000 ns; Shadow
Depth may regress by at most 20,000 ns, failed frames remain zero, and logical/
backend bytes remain exactly 16,777,216. Objective image rules, Low entry
metrics, the ABI/diagnostic/counter inventory, and capture assets are frozen in
`Engine/Tests/Native/EngineTests/Data/DirectionalShadowQ1`. Submitted and
prepared views now carry bounded Low/Medium/High identity, exact comparison,
footprint, and tier-specific guard metadata. Invalid identity falls back to Low;
the 464-byte forward ABI and its 144-byte shadow block publish runtime texture
step and filter identity without global mutable state. Per-view counters record
tier, nominal comparisons, guard, and fallback. The shared receiver helper
executes the prepared exact L0, M1, or H1 kernel. Sequential quality tests, C++
layout assertions, Slang reflection, and Vulkan qualification preserve every
Q0/Q1-entry Low hash and motion value. Medium is the selected production
default; Low remains the exact fallback.

Stage 0's edge metric was revised after the first exact candidate run proved
that raw horizontal second-difference HFE mixed visible caster edges, shadow
strength, edge orientation, and filter quality. The replacement uses the same
ROI and unchanged 0.85/0.90 gates, but subtracts an exact disabled-shadow
same-geometry reference, removes the ROI mean, zero-pads to 256x256, and reports
the fraction of two-dimensional DFT energy above normalized radial frequency
0.30. Exact L0, M1, and H1 fractions are 0.018361, 0.015372, and 0.013730;
M1/L0 is 0.837 and H1/M1 is 0.893, so both now agree with the observed smoothing
and pass the unchanged edge gates. The contract, exact hashes, superseded raw
HFE evidence, and replacement results are checked in under
`DirectionalShadowQ1`.

Q1 is complete. Exact M1 and H1 production paths, tier-local fitting, fully-lit
invalid taps, and three filter diagnostics are implemented. The checked-in
qualification package freezes complete Q0/Q1 Medium and High parity captures,
diagnostic hashes, transition width, correctness, motion, bytes, failures, and
RTX 3090 timing. Medium passes the edge gate at 0.837 of Low, retains 51/76
changed Q1 motion pixels, preserves the planar-acne image exactly, keeps
Masked/Opaque equality and valid/defective distinction, and adds only 768 ns
median Scene Color over Low. High improves edge energy to 0.893 of Medium and
passes performance, but its 487/563 motion results reject it as the default.

Medium is the production default. Low remains exact, numeric zero, and the
invalid-identity/resource-failure fallback; High remains available as a bounded
non-default tier. StaticMesh, SplineMesh, SkeletalMesh, Terrain, Masked,
two-sided, multi-view, invalidation, reload, and lifetime targets pass under the
selected default. The directional-shadow and Terrain qualifications,
`fast-all`, full Debug `all` build, documentation validation, and an editor
startup smoke pass complete the exit gate. Lasting behavior is published in
[Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md), and the
roadmap records the Q2 cascade entry evidence.

## Goal

Provide deterministic Low, Medium, and High directional-shadow filtering tiers
and select one measured production default. The selected tier must reduce
quantified edge stair-stepping without hiding authored gaps, reintroducing valid
seam leaks, increasing bias detachment, darkening outside the fitted map,
destabilizing motion, or changing the established fully lit fallback.

## Scope

- Immutable per-view directional-shadow filter quality and prepared kernel
  metadata owned by the existing directional-shadow feature.
- Exact Low, Medium, and High comparison kernels, weights, sample counts,
  effective footprints, guard texels, and valid-region behavior.
- A deterministic 3x3 tent Medium candidate and one frozen High candidate
  selected from a literal 5x5 tent or an exactly equivalent optimized kernel.
- Filter-footprint, tap-validity, and Low-versus-candidate diagnostic evidence.
- Reuse and extension of the Q0 acne, contact, valid/defective modular, grazing,
  Masked/Opaque, disabled/Unlit, and sub-texel motion fixtures.
- StaticMesh, SplineMesh, SkeletalMesh, and Terrain receiver/caster parity.
- Main, auxiliary, preview, present, offscreen, fixed-aspect, post-process, and
  editor-assistance view isolation.
- Resource failure, retry, shader reload, device invalidation, scene release,
  shutdown, logical/backend bytes, and target-GPU timing qualification.
- Lasting filter-tier, default, fallback, diagnostic, and measurement contracts.

## Non-Goals

- Cascades, texture arrays, atlases, cascade selection, overlap, or blending.
- PCSS, blocker search, variable penumbrae, stochastic rotation, temporal
  accumulation, history, blue noise, or frame-varying kernels.
- Contact shadows, point/spot shadows, translucent or colored shadowing,
  persistent caching, virtual shadows, moment maps, or ray tracing.
- Changing shadow-map resolution, format, comparison direction, caster
  selection, material participation, vertex factories, or deformation.
- Retuning the completed Q0 bias policy to compensate for a rejected kernel.
- Concealing gaps, T-junctions, invalid masks, or deformation mismatch through
  a wider filter.
- A public graphics-settings UI or persistent user preference system.

## Design Decisions and Invariants

### Tiers are immutable per prepared view

- Filter quality is copied from `FSceneViewSettings` into the immutable
  prepared directional-shadow value. It is never process-global mutable state.
- `Low` has numeric value zero and remains the current one-call linearly
  filtered comparison. Invalid quality identity sanitizes to Low.
- Each view fits, renders, and samples using only its prepared tier, footprint,
  and guard. Sequential views cannot reuse another view's tier metadata.
- The production default changes only after Stage 3 selects a measured tier.
  Until then, ordinary views remain Low and byte-identical to the Q0 baseline.

### Candidate kernels are exact

- Low candidate `L0` performs one `SampleCmpLevelZero` at offset `(0,0)` with
  weight one. The sampler remains linear, clamp-to-border opaque white, and
  `LessOrEqual`; one shader texture operation is reported.
- Medium candidate `M1` performs nine comparison samples at the Cartesian
  product of offsets `{-1,0,1}` shadow texels. Per-axis weights are `[1,2,1]`;
  product weights sum to 16 and are normalized exactly by `1/16`.
- High entry candidate `H1` performs 25 comparisons at offsets
  `{-2,-1,0,1,2}`. Per-axis weights are `[1,2,3,2,1]`; product weights sum to
  81 and are normalized exactly by `1/81`.
- Stage 0 may replace `H1` with one optimized candidate only when its discrete
  comparison weights, effective footprint, tap count, and maximum error against
  H1 are frozen. Two incompatible High implementations cannot proceed together.
- Kernel offsets derive only from exact `1 / shadowTextureDimensions`; they do
  not derive from viewport size, world scale, or nominal resolution constants.

### Footprints and guards are explicit

- A hardware linear comparison centered on an integer-offset tap contributes a
  half-texel footprint. L0 therefore has radius 0.5 texels, M1 has radius 1.5,
  and H1 has radius 2.5.
- Low and Medium retain the two-texel guard. H1 requires a three-texel guard.
  Tier-specific fitting must preserve exact Low Q0 output while giving High
  enough valid coverage; a global maximum guard cannot silently alter Low.
- Every tap is classified against the prepared valid region before sampling.
  Outside taps contribute fully lit comparison weight and never clamp inward to
  duplicate an edge caster. Entirely invalid receiver projection remains fully
  lit under every tier.
- Filter width never changes Q0 raster, receiver, or normal-offset bias terms.
  The bias contribution and filter contribution remain separately observable.

### Diagnostics establish filtering cause

- Q1 adds exact evidence for kernel footprint, valid/invalid tap coverage,
  comparison weight, and candidate-minus-Low classification.
- Filter diagnostics distinguish insufficient spatial filtering from missing
  caster depth, receiver comparison failure, excessive bias, and authored
  geometry already classified by Q0.
- Diagnostic-disabled rendering uses the same selected tier as production and
  adds no extra texture sample, target, pass, storage buffer, atomic, history,
  device wait, or mutable global state.
- Counters report prepared tier, nominal shader comparison operations, guard,
  fallback, and views per tier. Fragment-level tap/classification outcomes are
  retained as exact capture statistics rather than GPU atomics.

### Ownership and failure remain unchanged

- `FDirectionalShadowRenderer` remains the private feature owner. Q1 extends
  one prepared shadow value and the shared forward helper; it does not create a
  parallel renderer or additional shadow target.
- Opaque and Masked StaticMesh, SplineMesh, SkeletalMesh, and Terrain continue
  to share the production base-pass position, normal, mask, winding, and
  deformation contracts.
- Invalid tier/filter metadata selects Low. Missing or failed shadow resources,
  invalid projection, or failed shadow preparation preserves the established
  complete fully lit binding and output.
- Retry, shader reload, device invalidation, release, and shutdown introduce no
  `WaitIdle`, command-list flush, or component/asset read.

## Current Foundations and Gaps

| Area | Existing Q0 foundation | Q1 gap |
| --- | --- | --- |
| Sampling | One shared helper performs one linearly filtered `LessOrEqual` comparison | No explicit tier, multi-tap kernel, normalized weights, or sample counter |
| Texel data | Exact shadow dimensions and per-view texel world size are available | Filter offsets are not expressed in shadow texels |
| Guard | Two texels are fitted and exposed to diagnostics | No contract relates guard size to a multi-tap effective footprint |
| Diagnostics | Coverage, pre/post-bias comparison, texel grid, contributions, and classification are causal | No tap footprint, invalid-tap weight, or candidate-versus-Low view |
| Fixtures | Exact acne, contact, seam, defective gap, grazing, Masked, fallback, and motion captures exist | Edge stair-stepping and penumbra width are not quantified per kernel |
| Geometry | Static/Spline/Skeletal/Terrain Opaque/Masked paths share the helper | No multi-tap parity evidence across those families |
| Views | Prepared shadow and diagnostic identity are view-local | Filter quality/guard isolation is not represented or tested |
| Lifetime | Resource retry, reload, invalidation, release, and shutdown are qualified | Tier changes and shader variants are not covered by recovery evidence |
| Performance | Q0 reports pass and Scene Color medians plus 16 MiB target bytes | Per-tier sample count and incremental Scene Color cost are unknown |

## Implementation Stages

### Stage 0: Freeze Q1 entry evidence and candidate contracts

- [x] Preserve the Q0 Low captures, hashes, motion values, counters, bytes,
  timings, hardware, driver, Vulkan version, warm-up, and measured-frame facts.
- [x] Add deterministic edge-staircase, diagonal silhouette, thin caster,
  Masked cutout, guard-boundary, and sub-pixel motion regions to the fixture
  package without changing production filtering.
- [x] Freeze L0 and M1 exactly as specified above and either freeze H1 or record
  one exactly defined optimized High replacement with comparison-error bounds.
- [x] Freeze each tier's texture-operation count, effective footprint radius,
  guard texels, tap-validity rule, border contribution, and normalization.
- [x] Freeze objective metrics for edge high-frequency energy, transition width,
  valid seam pixels, defective-gap width, Masked/Opaque mismatch, contact
  detachment, acne, outside darkening, and motion-frame differences.
- [x] Freeze the target hardware and maximum median Scene Color increment for
  Medium and High relative to Low; retain Shadow Depth timing and logical/
  backend byte reporting even though Q1 adds no target allocation.
- [x] Inventory the smallest C++/Slang ABI, diagnostic identity, counter, shader
  reflection, and test-fixture changes before implementation begins.

#### Acceptance Gate

- Low entry evidence is reproducible; one exact Medium and one exact High
  candidate remain; footprints, guards, weights, samples, image/motion metrics,
  performance budgets, hardware, ABI, diagnostics, and failure behavior are
  unambiguous; production filtering is still unchanged.

### Stage 1: Add immutable quality identity and shared filter metadata

- [x] Add bounded Low/Medium/High identity to the submitted and prepared view,
  sanitizing invalid values to Low without global mutable state.
- [x] Extend the forward ABI with only the frozen filter identity, texel step,
  footprint, guard, and diagnostic data; assert C++ size/offsets and Slang
  reflection bindings.
- [x] Refactor the shared shadow helper around one comparison-kernel function
  used by StaticMesh/SplineMesh, SkeletalMesh, and Terrain receivers.
- [x] Preserve L0 output byte-for-byte, including Q0 bias, border, disabled,
  Unlit, invalid projection, and failed-resource behavior.
- [x] Add per-view counters for tier, nominal comparison operations, guard,
  invalid-tier fallback, and selected filter diagnostic.
- [x] Prove sequential views with different qualities and diagnostics consume
  only their own immutable metadata.

#### Acceptance Gate

- Low exactly matches Q0 images, hashes, motion, bindings, and counters; ABI and
  reflection agree; invalid identity falls back to Low; all geometry families
  compile through one helper; sequential view state remains isolated.

### Stage 2: Implement and diagnose the Medium 3x3 tent tier

- [x] Implement the nine exact M1 comparison offsets and product weights with
  stable accumulation and explicit normalization.
- [x] Validate every tap against the two-texel valid guard before sampling and
  contribute fully lit weight for outside taps.
- [x] Add footprint, tap-validity, and Medium-minus-Low diagnostic outputs with
  exact frozen encodings and capture statistics.
- [x] Render the complete Q0 fixture set plus new edge, thin, Masked, guard, and
  motion regions under Low and Medium.
- [x] Prove Medium reduces frozen edge-staircase metrics while preserving valid
  seams, authored gaps, contact, acne, Masked coverage, outside behavior, and
  Q0 bias classifications.
- [x] Record Medium Scene Color increment, sample count, bytes, failures, and
  diagnostic cost on the frozen target fixture.

#### Acceptance Gate

- M1 matches its exact nine-tap contract, improves the frozen edge metric,
  stays within every correctness/motion/performance gate, remains causal in
  diagnostics, and preserves Low and fully lit fallbacks.

### Stage 3: Implement High, compare tiers, and select the default

- [x] Implement the single frozen High kernel and its exact three-texel guard,
  weights, valid-tap behavior, and sample counter.
- [x] Preserve tier-specific fitting so Low remains byte-identical and Medium
  retains its two-texel footprint contract.
- [x] Capture Low, Medium, and High under identical fixed-camera, motion,
  geometry, material, scale, light-angle, and guard-boundary fixtures.
- [x] Reject High if its additional smoothing does not provide a quantified
  benefit over Medium or if it exceeds transition, gap, contact, motion,
  Masked, outside, or performance limits.
- [x] Select and record exactly one production default from Low, Medium, or
  High; retain Low as the invalid-identity and optional-tier fallback.
- [x] Freeze the selected default's equation, samples, guard, footprint,
  counters, images, hashes, timings, and rationale before broader rollout.

#### Acceptance Gate

- All three bounded tiers have exact evidence; one measured default is selected;
  the default improves quantified filtering without concealing non-filtering
  defects; Low remains exact and every optional-tier failure is bounded.

### Stage 4: Qualify geometry, views, motion, counters, and lifetime

- [x] Qualify horizontal, vertical, diagonal, sloped, grazing, thin, mirrored,
  two-sided, Masked, skinned, spline-deformed, Terrain, valid modular, and
  intentionally defective geometry across representative scales and angles.
- [x] Reconcile caster, draw, triangle, tier, sample, guard, diagnostic,
  resource, failure, retry, byte, and timing evidence without reusing camera
  visibility for caster visibility.
- [x] Exercise main, auxiliary, preview, present, offscreen, fixed-aspect,
  post-process, and editor-assistance views sequentially with mixed tiers.
- [x] Exercise camera translation/rotation, light motion, animation, material
  replacement, Terrain revision, resize, enable toggles, and selected-light
  changes under the selected default and Low fallback.
- [x] Exercise target/view/sampler/shader/PSO/material/palette failure, manual
  retry, shader reload, device invalidation, scene release, and shutdown.
- [x] Record diagnostic-disabled per-tier GPU deltas and diagnostic-enabled
  development cost separately, with logical/backend bytes unchanged.

#### Acceptance Gate

- Geometry/material/view parity, motion stability, counters, recovery, lifetime,
  bytes, and target timing pass for every tier; no tier samples stale state,
  crosses its valid guard, or introduces a whole-device wait.

### Stage 5: Qualify Q1 and publish lasting contracts

- [x] Run focused filter math, shader ABI/helper, scene/view, material/
  deformation, counter, failure, RHI, and Vulkan coverage selected according to
  repository testing guidance.
- [x] Run the directional-shadow image/motion qualification and StaticMesh/
  SplineMesh, SkeletalMesh, Terrain, Masked material, multi-view, invalidation,
  and reload targets with Vulkan validation clean.
- [x] Run affected targets, `fast-all`, full Debug `all`, any affected Shipping
  qualification, and representative editor smoke under repository guidance.
- [x] Record final Low/Medium/High kernels, selected default, image metrics,
  motion results, sample counts, counters, bytes, GPU medians, failures,
  retries, hardware, driver, and Vulkan version.
- [x] Publish lasting tier, kernel, guard, sampling, diagnostic, default,
  fallback, ABI, view, failure, and measurement behavior in
  [Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md).
- [x] Update the Shadow System Evolution roadmap with Q1 completion and exact
  Q2 entry evidence; do not activate cascades until the selected filter's
  footprint, guard, bias interaction, bytes, and timing are frozen.

#### Acceptance Gate

- The Q1 roadmap exit gate passes: the selected default reduces quantified edge
  stair-stepping without seam leaks, outside darkening, unstable motion,
  Masked divergence, hidden authored defects, stale view state, unbounded cost,
  or fallback regression; required evidence, tests, builds, documentation, and
  editor smoke are complete.

## Validation Matrix

| Contract | Required evidence |
| --- | --- |
| Kernel math | Exact offsets, weights, normalization, sample count, footprint radius, and candidate equivalence tests |
| Low parity | Byte-identical Q0 Lit, diagnostic-disabled, disabled-shadow, Unlit, invalid-projection, and motion evidence |
| Edge quality | Frozen diagonal/silhouette ROIs quantify high-frequency reduction and bounded transition width |
| Guard behavior | Boundary fixtures prove invalid taps contribute fully lit weight without clamp-inward duplication or dark borders |
| Causal classification | Q0 cause modes plus footprint/tap/difference modes separate filtering from coverage, bias, and authored geometry |
| Valid/defective geometry | Valid seams remain closed while gaps and T-junction controls retain their authored opening |
| Material/deformation | Opaque/Masked StaticMesh, SplineMesh, SkeletalMesh, and Terrain preserve production position, mask, normal, winding, and deformation meaning |
| Motion | Fixed camera/light/animation/resize sequences remain within frozen shimmer and popping limits for every tier |
| View isolation | Sequential supported views consume only their prepared tier, guard, diagnostics, and regenerated contents |
| Counters | Views per tier, nominal comparison samples, guard, caster/draw/resource/failure/retry/byte/timing outcomes reconcile |
| Failure/lifetime | Invalid tier, resource/shader/PSO failure, retry, reload, invalidation, release, and shutdown remain recoverable and fully bound |
| Performance | Per-tier Scene Color and Shadow Depth medians, sample counts, diagnostic cost, logical/backend bytes, and failed frames are explicit |
| Handoff | Focused targets, Vulkan image/motion qualification, `fast-all`, required builds, validation, and editor smoke pass |

## Definition of Done

- Low, Medium, and High have exact deterministic kernels, normalized weights,
  sample counts, footprints, guards, valid-tap behavior, and bounded fallbacks.
- One production default is selected by frozen image, motion, correctness, and
  target-GPU evidence rather than nominal kernel size.
- Low remains byte-identical to Q0 and is the invalid-tier fallback.
- The selected tier reduces quantified edge stair-stepping without hiding
  authored defects or regressing seams, contact, acne, Masked coverage, guard
  behavior, motion, supported views, or failure output.
- Filter diagnostics and counters remain per-view and causally distinct from
  Q0 coverage and bias evidence.
- StaticMesh, SplineMesh, SkeletalMesh, and Terrain retain shared Opaque/Masked
  production semantics.
- Logical/backend bytes, texture-operation counts, GPU medians, diagnostic cost,
  failures, and retries are recorded for every tier.
- Required focused, integration, Vulkan image/motion, build, validation, and
  editor-smoke gates pass.
- Lasting behavior is published under Runtime Rendering, Q1 is complete in the
  roadmap, and Q2 remains gated on the recorded footprint/guard evidence.

## Deferred Follow-ups

- Three-cascade fitting, array/atlas resources, selection, overlap, blending,
  and per-cascade tier scale belong to `CascadedDirectionalShadows` after Q1.
- PCSS, blocker search, variable penumbrae, stochastic kernels, and temporal
  filtering require separate product evidence and ownership.
- Contact shadows remain conditional on Q1/Q2 proving that bounded bias and
  selected spatial filtering cannot retain required near-field contact.
- Public graphics settings and editor UX wait for the selected production tiers
  and platform budgets to become lasting configuration policy.

## Related Documentation

- [Shadow System Evolution Roadmap](../../../Roadmaps/Archive/2026-08/ShadowSystemEvolution.md)
- [Directional Shadow Diagnostics And Bias Plan](DirectionalShadowDiagnosticsAndBias.md)
- [Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md)
- [Forward Lighting](../../../Runtime/Rendering/ForwardLighting.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../../../Runtime/Rendering/SkeletalMeshRendering.md)
- [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md)
- [RHI Diagnostics and Conformance](../../../Runtime/Rendering/RHIDiagnosticsAndConformance.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Shaders/Slang/Lighting/DirectionalShadow.slang`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Tests/Native/EngineTests/Private/DirectionalShadowBaselineVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshRenderPreparationVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainRenderVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceInvalidationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceReloadVulkanTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/ShaderReflectionTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
