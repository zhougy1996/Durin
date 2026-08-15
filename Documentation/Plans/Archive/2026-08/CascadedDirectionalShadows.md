# Cascaded Directional Shadows Plan

Summary: Add three stabilized directional-shadow cascades with deterministic splits, per-cascade fitting and culling, bounded blending, diagnostics, and measured production selection.

Last reviewed: 2026-08-14

Status: Archived
Completed: 2026-08-14

## Current Status

Q2 is complete. The selected production path uses three 2048x2048 D32 layers
in one `Texture2DArray`, 50,331,648 logical/backend bytes, the existing
256-world-unit distance, practical perspective splits at lambda 0.65, uniform
orthographic splits, and 10% adjacent transitions. Every cascade owns its fit,
snapping, conservative casters, bias, valid region, and the selected Medium
filter's nine comparisons outside overlap or 18 inside.

The RHI/Vulkan proof covers sampled-array and exact single-layer views,
layer-range transitions, three ordered depth passes, comparison sampling,
descriptor completeness, failure injection, retry/reload, and release. On the
RTX 3090, driver 591.86, Vulkan 1.4.325, the 1920x1080 fixture with 30 warm-up
and 120 measured frames records 19,328 ns combined for SingleMap Medium and
31,936 ns for ThreeCascades Medium: a 12,608 ns increment against the
1,000,000 ns gate, with zero failed measured frames. The required per-draw
dynamic depth-bias fix from `dev` was rebased before hashes and motion evidence
were re-frozen. ThreeCascades Medium is now the default.

The live `CascadeDifference` diagnostic compares neighboring cascade results.
Exact candidate-versus-single comparison remains an offline qualification
operation because retaining a second production depth target would violate the
no-extra-target invariant.

## Goal

Preserve useful directional-shadow resolution from the camera near field
through the existing authored shadow distance by selecting one measured,
stable three-cascade production tier. The result must avoid visible split
seams, cross-layer contamination, stale sequential-view state, material or
deformation divergence, unbounded memory/time, and regression of the complete
fully lit fallback.

## Scope

- One immutable per-view three-cascade candidate with deterministic practical
  splits, bounded far distance, independent fitting, guard expansion, texel
  snapping, conservative caster discovery, and per-cascade counters.
- A preferred D32 `Texture2DArray` resource with one exact sampled array view
  and one exact 2D depth-attachment view per layer; a guarded atlas is admitted
  only if Stage 0 rejects the array contract with recorded evidence.
- Three shadow-depth recordings followed by one shared forward-lighting path
  that deterministically selects and blends adjacent cascades.
- Per-cascade Q0 bias and Q1 Low/Medium/High filter semantics derived from each
  cascade's texel world size, footprint, and valid region.
- Cascade index, transition blend, coverage, split, fitting, caster, resource,
  memory, failure, and GPU-time diagnostics and qualification evidence.
- Opaque and Masked StaticMesh, SplineMesh, SkeletalMesh, and Terrain parity;
  all already-supported scene-view families, motion, lifetime, retry, reload,
  and device invalidation behavior.
- Comparison against the Q1 single-map Medium default and selection or
  rejection of the three-cascade candidate from frozen image, motion, memory,
  and target-GPU gates.

## Non-Goals

- Point or spot shadows, multiple shadowed directional lights, persistent
  caching, asynchronous scheduling, Render Graph migration, or a public pass
  registry.
- Screen-space contact rays, blocker search, variable penumbrae, stochastic or
  temporal filtering, moment maps, virtual pages, or ray tracing.
- Translucent, colored, dithered, or opacity-weighted shadow casting.
- Repairing authored gaps, T-junctions, invalid masks, or deformation mismatch
  through cascade overlap, filtering, or bias.
- Increasing the authored shadow distance beyond 256 world units without a
  separate product scene, visibility policy, and performance gate.
- A public graphics-settings UI, serialized preference, or unrestricted
  user-authored cascade configuration.

## Design Decisions and Invariants

### The entry candidate is exact but not yet the production default

- The candidate count is exactly three. Each layer is 2048x2048 D32, so
  logical storage is exactly `3 * 2048 * 2048 * 4 = 50,331,648` bytes. Backend
  allocation bytes remain an independently measured value.
- The active production path remains Q1's one 2048x2048 D32 map until Stage 3
  passes every selection gate. Invalid candidate identity or any preparation,
  resource, or draw failure falls back to the complete fully lit binding; it
  never silently samples an incomplete subset of cascades.
- The frozen comparison tier is the single-map Medium default: 16,777,216
  logical/backend bytes, 12,640 ns Scene Color, 10,240 ns Shadow Depth, and
  51/76 dedicated Q1 camera/light motion pixels on the RTX 3090 fixture.
- The entry candidate may be rejected or tightened from measured Stage 0
  evidence. Changing count, resolution, format, distance, split equation,
  overlap equation, or resource topology after Stage 0 requires recording the
  replacement and rationale before implementation continues.

### Splits are deterministic view data

- For perspective views, let `n` be the finite positive fitted camera near
  distance and `f = min(fittedFar, 256)`, with `f > n`. For boundary `i` of
  three cascades, `p = i/3`, `log = n * pow(f/n,p)`, and
  `uniform = n + (f-n)*p`; the practical boundary is
  `split(i) = 0.65*log + 0.35*uniform`. Boundary zero is `n`, boundary three is
  `f`, and the two interior results must be finite and strictly increasing.
- Orthographic views use the uniform term because perspective logarithmic
  depth has no useful meaning. Degenerate, non-finite, inverted, or zero-width
  intervals reject cascade preparation and retain the fully lit fallback.
- Split facts are derived from the fitted immutable `FSceneView`, never from a
  process-global camera. Sequential views cannot reuse another view's split,
  matrices, layer contents, or diagnostics.
- The existing 256-world-unit maximum remains the candidate far boundary and
  each cascade retains the existing 256-unit caster extrusion opposite light
  travel.

### Overlap and selection have one bounded meaning

- The primary cascade is selected from positive view-space receiver depth
  against the frozen split boundaries. Outside the maximum distance remains
  fully lit.
- Cascade 0 has no nearer transition. For cascade `i > 0`, its transition
  begins at `split(i) - 0.10 * (split(i+1) - split(i))` and ends at `split(i)`.
  The weight is a clamped linear ramp from the nearer cascade to cascade `i`.
- Only neighboring cascades may be sampled during a transition. Outside an
  overlap, one cascade is sampled; inside an overlap, exactly two independently
  validated shadow results are blended. An invalid projection or invalid tap
  in either cascade contributes fully lit according to the existing Q0/Q1
  rule and cannot clamp into another layer or tile.
- Stage 0 freezes the resulting maximum nominal Medium comparison operations:
  nine outside a transition and eighteen inside it. Diagnostic-only extra
  comparisons are reported separately.

### Every cascade owns its fit, bias, filter scale, and casters

- Each receiver slice is transformed into the shared directional-light basis,
  independently expanded by its tier guard, centered, and snapped to its own
  whole-texel grid. One cascade's snapping cannot move another cascade.
- The existing raster, receiver, and normal-offset equations consume that
  cascade's own texel world size. Near-cascade values are not copied to mid or
  far, and blending never sums two bias displacements.
- Low, Medium, and High keep their exact Q1 kernels, valid-tap behavior, guards,
  and fallback. Medium remains the candidate default filter; Stage 3 does not
  reopen Q1's High-default rejection.
- Caster discovery begins from authoritative `FScene` collections for each
  cascade. Submitted, hidden, invalid-bounds, outside, intersecting, contained,
  prepared, draw, and triangle outcomes reconcile per cascade. Camera
  visibility and camera LOD do not become caster visibility or shadow LOD.
- Static/Spline deformation, SkeletalMesh palettes, Terrain height resources,
  Masked coverage, two-sided normals, and mirrored winding retain the base-pass
  contracts already qualified by Q0/Q1.

### One feature owner controls array resources and lifetime

- `FDirectionalShadowRenderer` remains the only feature owner. It owns one
  three-layer texture candidate, one sampled array view, three exact single-
  layer depth-attachment views, the comparison sampler, shaders, PSOs, failure
  state, retry, invalidation, and release.
- Each layer transitions only through explicit ranges. Recording layer `i`
  binds only attachment view `i`, clears it to 1.0, uses its own viewport and
  scissor, retains the parent/view resources, and finishes all three layers in
  `ShaderReadOnly`/`GraphicsShaderRead` before Scene Color samples the array.
- Texture-array use proceeds only after Stage 0 proves creation, exact sampled
  and attachment views, layer-range transitions, comparison sampling, and
  failure injection on Vulkan. If that proof fails, the plan must record an
  atlas replacement with exact tile resolution, guards, viewport/scissor,
  addressing, transition, bytes, and no-cross-tile tests before Stage 1.
- Partial creation or preparation never publishes a valid cascade set.
  Retry, shader reload, material replacement, resize, device invalidation,
  release, and shutdown introduce no `WaitIdle`, command-list flush, stale
  descriptor, or component/asset read.

### Diagnostics and budgets are selection gates

- Q2 adds immutable modes for cascade index, transition weight, per-cascade
  valid coverage, and selected-versus-single-map difference. Exact encodings
  and diagnostic-only sample cost are frozen in Stage 0; existing Q0/Q1 modes
  keep their meanings for the selected cascade.
- Per-view counters report candidate identity, cascade count, layer, splits,
  transition interval, texel world size, guard, tier, nominal comparisons,
  caster outcomes, draws, triangles, resources, retries, failures, logical and
  backend bytes, and Shadow Depth/Scene Color timings.
- On the frozen RTX 3090, driver 591.86, Vulkan 1.4.325, 1920x1080 fixture with
  30 warm-up and 120 measured frames, the entry candidate must add no more than
  1,000,000 ns combined median Shadow Depth plus Scene Color over Q1 Medium,
  allocate no more than 67,108,864 logical or backend bytes, report zero failed
  measured frames, and add no target outside the three-layer D32 resource.
- Stage 0 freezes near/mid/far sharpness, transition discontinuity, contact,
  acne, valid/defective seam, Masked/Opaque, outside-darkening, and motion
  thresholds before implementation affects production output. A familiar
  cascade count or sharper still image cannot override a failed motion,
  correctness, memory, or timing gate.

## Current Foundations and Gaps

| Area | Q0/Q1 foundation | Q2 gap |
| --- | --- | --- |
| Ownership | One private renderer owns a recoverable single-map feature and complete fully lit fallback. | No atomic three-cascade publication or partial-layer failure contract is implemented. |
| Fitting | Camera-relative receiver fitting, guard expansion, caster extrusion, and whole-texel snapping are deterministic. | No split slices, per-slice fit, or split-crossing stabilization exists. |
| Bias/filter | Per-texel bounded bias and exact Low/Medium/High kernels are shared across receivers; Medium is default. | ABI and shader logic carry only one matrix, texel scale, valid region, and comparison result. |
| Casters | Authoritative scene collections and shared material/deformation preparation are qualified. | Outcomes, LOD, draws, and triangles are not conserved independently per cascade. |
| RHI/Vulkan | Texture arrays, layer ranges, 2D single-layer views, D32 attachments, comparison samplers, and resource retention primitives exist. | Their combined depth-array attachment and comparison-sampling contract lacks a dedicated conformance proof. |
| Diagnostics | Coverage, comparison, bias, filter footprint, tap validity, and tier difference modes are causal and immutable per view. | No cascade index, overlap weight, layer coverage, split, or cascade-versus-single-map evidence exists. |
| Fixtures | Exact Q0/Q1 correctness, edge, guard, material, geometry, motion, fallback, bytes, and timing evidence exists. | No near/mid/far resolution target, moving split boundary, or cross-layer contamination fixture exists. |

## Implementation Stages

### Stage 0: Freeze Q2 entry evidence and the resource contract

- [x] Preserve Q1 Medium single-map images, hashes, motion values, counters,
  logical/backend bytes, timings, hardware, driver, Vulkan version, warm-up,
  measured-frame count, and failed-frame facts as the comparison baseline.
- [x] Add deterministic near/mid/far receivers, split-aligned silhouettes,
  transition bands, thin and Masked casters, guard/layer boundaries, valid and
  defective modular geometry, and camera/light/split-motion entry captures
  without changing production cascade count.
- [x] Freeze exact ROI metrics and limits for near/mid/far high-frequency
  energy, transition discontinuity and width, contact detachment, acne,
  valid-seam leakage, defective-gap preservation, Masked/Opaque mismatch,
  outside darkening, and tolerance-based motion-frame differences.
- [x] Prove or reject one 3x2048x2048 D32 `Texture2DArray` with an exact sampled
  array view, three single-layer depth views, layer-range transitions, clears,
  comparison sampling, descriptor completeness, resource retention, and
  injected partial failure under RHI validation and Vulkan.
- [x] Freeze the practical-split and overlap equations, perspective and
  orthographic goldens, 256-unit clamp, degenerate behavior, Medium 9/18 sample
  counts, two-texel guards, per-cascade caster strategy, and all diagnostic
  encodings.
- [x] Measure the entry candidate or an isolated equivalent workload to confirm
  the 64 MiB memory and 1,000,000 ns combined-increment gates are credible;
  record any replacement target fixture or tighter budget before Stage 1.
- [x] Inventory the smallest C++/Slang ABI, reflection, resource, transition,
  pass-recording, counter, fixture, and lasting-document changes.

#### Acceptance Gate

- Q1 baseline evidence is reproducible; one exact three-cascade candidate and
  one resource representation remain; splits, overlap, fitting, guards,
  samples, diagnostics, counters, fixtures, correctness/motion metrics, memory,
  GPU budget, ABI, and failure behavior are unambiguous; production remains the
  single-map Medium path.

### Stage 1: Add immutable cascade identity, math, and array ownership

- [x] Add bounded candidate identity and three-cascade immutable prepared-view
  storage, sanitizing invalid identity to the existing single-map/failure
  behavior without global mutable state.
- [x] Implement and unit-test perspective practical splits, orthographic
  uniform splits, overlap intervals, receiver-depth selection, degenerate
  rejection, and deterministic per-cascade fitting/snapping.
- [x] Extend `FDirectionalShadowRenderer` with the selected array texture,
  sampled view, three attachment views, exact logical/backend accounting, and
  atomic create/publish/failure/retry/release behavior.
- [x] Extend the forward ABI with the minimum frozen matrices, split/overlap,
  valid-region, texel/bias/filter, and diagnostic data; assert C++ sizes/offsets
  and Slang reflection.
- [x] Preserve the single-map Medium path, disabled/Unlit output, invalid
  projection, invalid candidate, resource failure, and sequential mixed-view
  behavior exactly.
- [x] Add per-view/per-cascade preparation, split, fit, resource, fallback,
  byte, and identity counters with reconciliation tests.

#### Acceptance Gate

- CPU math goldens, ABI/reflection, resource/view/layer contracts, atomic
  failure, counters, and sequential-view isolation pass; enabling no candidate
  leaves every Q1 production image and hash unchanged.

### Stage 2: Record independent cascade depth passes and conserve casters

- [x] Prepare conservative caster candidates independently for each cascade
  from authoritative scene collections, including boundary contact and invalid
  finite bounds without reusing camera visibility.
- [x] Record three ordered depth-only passes with exact layer views, clears,
  viewport/scissor, transitions, raster bias, retained resources, and final
  sampled state; reject partial publication.
- [x] Reuse production Opaque/Masked StaticMesh, SplineMesh, SkeletalMesh, and
  Terrain positions, masks, normals, winding, deformation, height, and palette
  resources for every cascade.
- [x] Derive raster, receiver, and normal-offset values from each cascade's
  texel world size and preserve its tier-specific filter guard.
- [x] Reconcile submitted, hidden, invalid, outside, intersecting, contained,
  prepared, resource, draw, triangle, layer, transition, retry, and failure
  outcomes independently per cascade.
- [x] Exercise material/palette/height/target/view/sampler/shader/PSO failure,
  retry, reload, invalidation, release, and shutdown without stale layers or a
  whole-device wait.

#### Acceptance Gate

- All three layers contain only their independently fitted conservative caster
  set; family/material/deformation output and counters reconcile; transitions,
  retained resources, partial failures, recovery, and lifetime are validation-
  clean while Scene Color still uses the Q1 single map.

### Stage 3: Implement receiver selection, blending, and diagnostics

- [x] Extend the shared Slang helper to choose one cascade outside overlap and
  exactly two neighbors inside overlap from immutable receiver depth.
- [x] Apply each chosen cascade's matrix, valid region, texel-scale bias, guard,
  and exact Low/Medium/High kernel before blending comparison results.
- [x] Implement the frozen cascade-index, transition-weight, per-layer coverage,
  and candidate-minus-single-map diagnostics without new targets, passes,
  storage buffers, atomics, history, or mutable globals.
- [x] Prove no sample crosses a texture layer or atlas tile and that invalid
  projection/taps contribute fully lit without inward clamping.
- [x] Capture the full Q0/Q1 correctness package plus near/mid/far, transition,
  boundary, split-motion, and cross-layer controls under single-map and cascade
  candidates with exact hashes/statistics.
- [x] Measure candidate image, motion, memory, Scene Color, combined Shadow
  Depth, diagnostic-only cost, failed frames, and counters on the frozen target
  fixture; select or reject cascades as production default from every gate.

#### Acceptance Gate

- Selection and blending match CPU/math and image evidence; no hard split seam,
  cross-layer contamination, outside darkening, hidden authored defect, stale
  state, or fallback regression exists; exactly one production default is
  selected and its evidence is frozen.

### Stage 4: Qualify geometry, views, motion, counters, and lifetime

- [x] Qualify horizontal, vertical, diagonal, sloped, grazing, thin, mirrored,
  two-sided, Masked, skinned, spline-deformed, Terrain, valid modular, and
  intentionally defective geometry in near, mid, far, and transition regions.
- [x] Exercise main, auxiliary, preview, present, offscreen, fixed-aspect,
  perspective, orthographic, post-process, and editor-assistance views
  sequentially with mixed single/cascade candidates and filter tiers.
- [x] Exercise camera translation/rotation, split crossing, light motion,
  animation, material replacement, Terrain revision, resize, enable toggles,
  selected-light changes, and far-distance clamp.
- [x] Exercise target/array-view/layer-view/sampler/shader/PSO/material/palette
  failure, partial allocation, manual retry, shader reload, device invalidation,
  scene release, and shutdown.
- [x] Reconcile per-view/per-cascade caster, draw, triangle, split, tier, sample,
  guard, diagnostic, resource, failure, retry, byte, and timing evidence.
- [x] Record diagnostic-disabled production cost and each diagnostic's
  development cost separately, with no target or storage allocation beyond the
  selected resource representation.

#### Acceptance Gate

- Geometry/material/view parity, split and ordinary motion, counters, recovery,
  lifetime, bytes, and target timing pass for the selected default and fallback;
  no view consumes another view's cascade data and no lifecycle path waits for
  the whole device.

### Stage 5: Qualify Q2 and publish lasting contracts

- [x] Run focused split/fitting/filter math, shader ABI/helper, RHI array/layer,
  scene/view, material/deformation, counter, failure, and Vulkan coverage under
  repository testing guidance.
- [x] Run directional-shadow image/motion qualification plus StaticMesh/
  SplineMesh, SkeletalMesh, Terrain, Masked material, multi-view, invalidation,
  reload, and cross-layer targets with Vulkan validation clean.
- [x] Run affected targets, `fast-all`, full Debug `all`, any affected Shipping
  qualification, documentation validation, and representative editor smoke
  under repository guidance.
- [x] Record the final cascade count/resolution/format, split/overlap equations,
  distance, resource layout, bytes, per-cascade fits/guards/bias/filter samples,
  image metrics, motion, counters, GPU medians, failures, retries, hardware,
  driver, and Vulkan version.
- [x] Publish lasting cascade ownership, resource, split, fit, selection,
  blending, diagnostic, counter, ABI, view, failure, fallback, and measurement
  behavior in [Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md).
- [x] Update the Shadow System Evolution roadmap with the Q2 result and
  disposition every conditional branch from recorded activation evidence.

#### Acceptance Gate

- The Q2 roadmap exit gate passes: near/mid/far detail and motion satisfy frozen
  thresholds; no hard transition or cross-layer contamination occurs; all
  caster families, views, failures, retries, invalidation, sequential-view
  cases, logical/backend bytes, and combined GPU increments pass; builds,
  documentation, and editor smoke are complete.

## Validation Matrix

| Contract | Required evidence |
| --- | --- |
| Split math | Perspective practical-split and orthographic uniform goldens, strict ordering, 256-unit clamp, overlap intervals, receiver selection, and degenerate rejection |
| Resource representation | Three-layer D32 creation, sampled array and exact attachment views, layer transitions/clears, comparison sampling, descriptor/reflection agreement, retention, bytes, and partial failure |
| Per-cascade fitting | Independent receiver corners, guard expansion, texel snapping, valid regions, caster extrusion, light/camera motion, and finite matrix evidence |
| Caster conservation | Submitted through drawn outcomes and triangles reconcile per layer without camera-visibility reuse for every geometry family |
| Bias and filtering | Each cascade uses its own texel world size and exact Q0/Q1 equations, tier guards, valid taps, sample counts, and fully lit outside behavior |
| Transition quality | Static and moving split fixtures quantify discontinuity/width and prove exactly one or two neighboring samples without a hard seam |
| Causal diagnostics | Cascade index, transition weight, layer coverage, and candidate difference remain distinct from Q0 bias/coverage and Q1 filter causes |
| Material/deformation | Opaque/Masked Static/Spline/Skeletal/Terrain positions, masks, normals, winding, deformation, height, and palettes agree in every cascade |
| View isolation | Sequential supported perspective/orthographic views cannot consume another view's splits, matrices, layers, candidates, filters, or diagnostics |
| Motion | Camera translation/rotation, split crossing, light changes, animation, resize, and far-clamp sequences remain within frozen thresholds |
| Failure/lifetime | Partial resource/view/pass failure, retry, reload, invalidation, release, and shutdown retain complete bindings and fully lit fallback without whole-device waits |
| Performance | Logical/backend bytes, per-layer Shadow Depth and Scene Color medians, 9/18 filter samples, draws, diagnostic cost, failed frames, and delta from Q1 Medium pass frozen gates |
| Handoff | Focused tests, Vulkan image/motion qualification, affected suites, `fast-all`, required builds, documentation validation, and editor smoke pass |

## Definition of Done

- Exactly one measured directional-shadow default is selected; if cascades are
  selected, their count, layers, resolution, format, splits, overlap, distance,
  fitting, stabilization, guards, and samples are immutable per prepared view.
- Near/mid/far detail improves against Q1 Medium without a visible split seam,
  unstable motion, cross-layer sampling, outside darkening, hidden authored
  defect, Masked divergence, contact/acne regression, or fallback change.
- Caster discovery, material coverage, deformation, winding, LOD, resources,
  draws, and triangles remain authoritative and independently conserved for
  StaticMesh, SplineMesh, SkeletalMesh, and Terrain in every cascade.
- Split selection, transition blending, bias, filtering, diagnostics, counters,
  and sequential views consume only immutable per-view/per-cascade data.
- Logical/backend bytes, pass and sampling GPU medians, sample counts,
  diagnostic costs, failures, retries, and target fixture facts are recorded
  and pass the frozen gates.
- Resource failure, partial preparation, retry, reload, invalidation, release,
  and shutdown preserve complete descriptors, fully lit output, and no whole-
  device waits.
- Required tests, Vulkan validation, builds, documentation validation, and
  editor smoke pass; lasting behavior is published and the roadmap records Q2
  completion plus evidence-based disposition of conditional branches.

## Deferred Follow-ups

- Screen-space contact shadows require Q0/Q2 evidence that valid near-field
  contact remains lost after cascade resolution and bounded bias are ruled out.
- PCSS or another variable-penumbra path requires a product source-size model,
  blocker/receiver fixtures, and budget beyond Q1/Q2 deterministic PCF.
- Persistent caching requires profiling that proves repeated cascade depth
  regeneration is material and stable scene/light/caster/view revision keys.
- Point/spot shadows require concrete light counts, ranges, priority,
  atlas/cube storage, update frequency, and target-platform budgets.
- Temporal, moment, virtual, or ray-traced representations require their own
  history, artifact, ownership, platform, memory, and fallback evidence.
- Public cascade/quality controls wait for the selected production policy and
  cross-platform budgets to become lasting configuration contracts.

## Related Documentation

- [Shadow System Evolution Roadmap](../../../Roadmaps/Archive/2026-08/ShadowSystemEvolution.md)
- [Directional Shadow PCF Quality Tiers Plan](DirectionalShadowPCFQualityTiers.md)
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
- `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanView.cpp`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanResourceState.cpp`
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
- `Engine/Tests/Native/RHITests/Private/RHIResourceViewValidationTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanTextureSamplingTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanFailureInjectionTests.cpp`
