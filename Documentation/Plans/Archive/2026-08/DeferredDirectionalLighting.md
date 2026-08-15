# Deferred Directional Lighting Plan

Summary: Implement and qualify one deferred directional, environment, and emissive lighting slice from the published minimal GBuffer while preserving forward parity and explicit migration fallback.

Last reviewed: 2026-08-15

Status: Archived
Completed: 2026-08-15

## Current Status

M2 completed on 2026-08-15. The published
[Minimal GBuffer Contract](../../../Runtime/Rendering/GBuffer.md) deterministically
encodes every eligible opaque/masked StaticMesh, SplineMesh, SkeletalMesh, and
Terrain surface in four 16-byte-per-pixel attachments plus existing D32. CPU
and Slang share decode and analytic position reconstruction. Debug, readback,
forward material-input A/B, view isolation, failure/reload, aggregate tests,
full build, hidden-editor smoke, and the frozen RTX 3090 geometry budget pass.

M3 Stage 0 is complete. Source inventory confirms the fixed 768-byte
`FForwardLightingUniform` already carries the selected directional light,
view position, full three-cascade shadow record, and local-light records. The
deferred pass will bind that exact per-view range and ignore local entries.
`Lighting/PBRLighting.slang` and `Lighting/DirectionalShadow.slang` already own
the equations; Stage 1 will move only the remaining inline view-vector,
environment-sampling, and composition orchestration into one shared
`Lighting/SurfaceLighting.slang` module.

The selected qualification result is one isolated `RGBA16_FLOAT` target at
8 bytes per pixel with a `64 MiB` LRU ceiling. Clear/background, alpha,
transitions, binding ABI, CPU/HDR/display tolerances, discontinuity policy,
fixtures, and RTX 3090 budgets are frozen below. A 5,184-channel CPU sweep of
the selected material/normal/light/view grid records display-byte p95 `1`, p99
`3`, and maximum `17`; the gate allows `1/3/18`. The exactly aligned minimum-
roughness specular singularity is deliberately decoded-reference-only because
the already-qualified one-degree normal transport can change its unbounded
peak by orders of magnitude. This is an explicit transport limitation, not a
looser implementation tolerance. Stage 1 is complete. The new
`Lighting/ForwardLightingUniform.slang` module freezes the existing 768-byte
ABI, while `Lighting/SurfaceLighting.slang` owns the exact forward/deferred
view frame, directional BRDF call, IBL sampling/evaluation, and final
composition. The existing forward shader retains its ordered direct-light
accumulation and passes material, skeletal, terrain, editor-grid, complete
directional-shadow qualification, reflection, and image fixtures.

`FDeferredDirectionalLightingRenderer` now owns a typed 13-binding shader/
pipeline payload and complete-or-null `RGBA16_FLOAT` target cache without
recording a draw. Layout/accounting tests freeze the 160-byte view uniform,
8-byte-per-pixel target, and `64 MiB` ceiling. Vulkan injected image,
shader-module, and graphics-pipeline failures prove same-generation
suppression, ordered manual recovery, shader/device generation recreation,
explicit release/recreation, and alternating-size isolation. The 55-target
`fast-all` profile and full build pass. Production forward output remains
authoritative.

Stage 2 is complete. The opt-in route now records one isolated full-screen
pass after the current view's GBuffer, reconstructs the world-space surface,
and evaluates the shared unshadowed directional, environment, emissive, and
opacity semantics into `RGBA16_FLOAT`. Invalid/background records retain the
view clear color, while zero-candidate views consume the freshly cleared
GBuffer without reusing an older view. Enabled/unavailable/failure counters,
GPU timing and capture seams, and decoded/directional/environment/emissive/
alpha/final diagnostics are live. Orthographic and perspective HDR A/B,
display-quantization bounds, above-one emissive, zero-eligible Unlit behavior,
all four primitive families, focused Vulkan owners, `fast-all`, and the full
build pass; production forward Scene Color remains byte-identical. Stage 3
is complete. Deferred lighting now consumes the selected shadow array,
comparison sampler, and unchanged prepared cascade record through the shared
receiver function. Low/Medium/High PCF, single-map and three-cascade
diagnostics, no-light, directional-only, emissive-only, default-black
environment, combined above-one HDR, perspective, orthographic, constrained-
aspect, and large-coordinate fixtures pass the frozen parity gates. The
existing decoded-record CPU/Slang sweep supplies custom-environment, mapped-
normal, mirrored/two-sided, masked-edge, and material-extreme evidence; the
runtime four-family fixture proves every geometry transport reaches the same
full-screen evaluator.

Main, auxiliary, camera-preview, thumbnail, offscreen, Present/resize, and
alternating-extent sequences report a fresh correctly sized target. A new
constrained-aspect regression exposed and fixed the GBuffer pass using the
unfitted view while forward used the fitted viewport; all three passes now
share the fitted viewport and the deferred shader reconstructs viewport-local
NDC without expanding the frozen ABI. A partial GBuffer remains unavailable
to deferred rather than sampling an incomplete payload. Focused contracts and
Vulkan owners, `fast-all`, and the full build pass.

Stage 4 and M3 are complete. The validation-enabled RTX 3090 1920x1080 run
uses 30 warm-up and 120 measured frames: GBuffer is `79,968/80,768 ns`,
isolated deferred directional is `200,896/201,952 ns`, and their sum is
`280,864/282,208 ns` median/p95, all below frozen gates. The active route is
`107,827,200` bytes. Focused owners, `fast-all`, the ordinary native aggregate,
full build, native-window Present/resize, injected lifecycle/recovery fixtures,
and an 8-second validation-clean hidden-editor smoke pass. The lasting
[Deferred Directional Lighting](../../../Runtime/Rendering/DeferredDirectionalLighting.md)
contract is published. M3 did not change the default opaque owner; the active
[Hybrid Renderer Production Rollout](HybridRendererProductionRollout.md) plan
now owns M4 from a fresh Stage 0 contract and budget freeze.

## Frozen Stage 0 Contract

### Shared functions and binding ABI

- `Lighting/SurfaceLighting.slang` will own a finite normalized surface frame
  from world position, shading normal, and view position; directional BRDF
  evaluation from an already selected shadow attenuation; environment texture
  sampling plus split-sum evaluation; and final
  `direct + environment + emissive` composition with effective opacity.
  Forward supplies its already ordered directional-plus-local direct sum; M3
  deferred supplies its directional term. This preserves forward accumulation
  order and avoids subtract/re-add rounding drift during extraction.
- Both entry points continue to call `EvaluateDirectionalShadow` from
  `Lighting/DirectionalShadow.slang`. The helper receives identical world
  position, decoded/final shading normal, cascade uniform, texture, and sampler
  semantics. Directional diagnostics replace lit color exactly as in forward.
- The deferred shader binds, in order, GBuffer material, normals, surface,
  emissive, D32, environment irradiance, prefiltered environment, BRDF LUT,
  environment sampler, directional-shadow array, shadow comparison sampler,
  one deferred-view uniform, and the existing 768-byte lighting uniform.
- The deferred-view uniform is exactly 160 bytes: four projection rows (64),
  view-to-world matrix (64), clear color (16), then inverse viewport XY,
  diagnostic identity, and reserved zero (16). Analytic reconstruction uses
  the projection rows; no inverse view-projection multiply is permitted.
- Missing environment payload binds the same black cube/black LUT/default
  sampler fallback used by forward geometry. Missing or disabled directional
  light yields zero directional direct. M3 does not interpret local records.

### Result, ordering, and memory

- `DeferredDirectionalColor` is `RGBA16_FLOAT`, clears to immutable
  `View.ClearColor`, and has no depth attachment. Valid standard-lit records
  overwrite RGB with scene-linear directional + environment + emissive and A
  with decoded effective opacity. Invalid flags, failed reconstruction, and
  background D32 preserve the clear color.
- M3 qualification order is shadow depth, GBuffer, deferred directional,
  unchanged forward Scene Color, contact/display mapping, then editor
  assistance. The deferred target finishes graphics-shader-readable and may be
  copied explicitly for validation; it is never CPU-readable or presented.
- SkyBox drawing, local lights, translucency, and contact composition are not
  part of the M3 result. Fixtures use clear background while environment IBL
  remains independently enabled. M4 owns those production composition layers.
- The target costs 8 bytes per pixel: `16,588,800` bytes at 1920x1080. The
  current extent is retained and oldest other extents are evicted above
  `64 MiB`; four 1920x1080 targets fit and five do not. Frozen M1 + M2 + M3
  cache ceilings total `384 MiB`. One 1920x1080 qualification route including
  scene targets, GBuffer, deferred color, and one SDR output is `107,827,200`
  bytes.

### Parity and fixture gates

- A decoded-record CPU/Slang reference owns correctness independent of forward
  transport. Each finite HDR channel must be within
  `max(0.002, 0.002 * abs(reference))`; opacity must be within `1/510`.
- For unshadowed forward/deferred displayed valid pixels, absolute byte error
  over RGB must have mean at most `1`, p99 at most `3`, and maximum at most
  `18`. Alpha allows one byte. The grid covers base colors 0.04-1, metallic
  0/0.5/1, roughness 0.045/0.06/0.1/0.2/0.5/1, AO 0/0.5/1, emissive 0/2/16/64,
  flat and mapped normals, off-axis lights/views, mirrored/two-sided geometry,
  and every M2 primitive family.
- The exactly aligned roughness-0.045 specular case compares deferred GPU only
  against the decoded-record reference. It must be finite; it is excluded from
  forward/deferred image statistics because octahedral UNORM8 normal
  quantization is allowed to move that mathematical singularity.
- Shadow-stable pixels and diagnostic classes must be byte-identical. Any
  changed classification must lie within a two-pixel dilation of a forward
  shadow, cascade, guard, or masked-coverage discontinuity; changed pixels are
  limited to `0.75%` of valid receiver pixels and may not form an unmatched
  island larger than four pixels. No-light/disabled/failed shadow cases are
  exact zero-directional references.
- Fixtures cover default/black/custom environment terms, component-isolated
  and combined lighting, three shadow filters, cascade interiors/transitions,
  masked edges, perspective, orthographic, constrained-aspect,
  large-coordinate, main/auxiliary/preview/thumbnail, Present/offscreen,
  resize, alternating extents, reload, invalidation, and shutdown.

### Timing and failure gates

- RTX 3090 qualification uses driver 591.86, Vulkan 1.4.325,
  `Win64-Debug-DurinEditor`, validation enabled, 1920x1080, 30 warm-up frames,
  and 120 measured frames. The representative fixture uses one-cover opaque/
  masked geometry over the four M2 primitive families, default studio IBL,
  one Medium-filter shadowed directional light, and no local/translucent/sky
  work.
- The isolated deferred-light pass must not exceed `300,000 ns` median or
  `450,000 ns` p95. GBuffer plus deferred light must not exceed `600,000 ns`
  median or `800,000 ns` p95. Timing excludes capture, readback, forward
  rendering, and display mapping.
- Target/shader/pipeline/environment/GBuffer/shadow failure reports its exact
  per-view category, preserves forward production output, and never publishes
  or samples a partial/stale payload. Same-generation suppression, manual
  retry, shader reload, device recreation, release, and shutdown follow the
  resource coordinator without `WaitIdle`.

## Goal

Produce one full-screen deferred result for valid standard-lit GBuffer records
that matches the current forward directional light, directional shadow,
environment lighting, emissive, opacity, and HDR semantics closely enough to
become the qualified opaque-lighting foundation for M4.

## Scope

- Factor the existing directional BRDF, environment evaluation, emissive
  composition, view-vector construction, and shadow sampling into shared Slang
  facilities consumed by forward and deferred entry points.
- Add a Renderer-owned full-screen deferred-lighting payload and explicit
  qualification target with bounded, size-keyed byte retention.
- Reconstruct view-relative/world position from D32 and decode the published
  GBuffer without changing its formats, flags, or tolerances.
- Evaluate the selected directional light and its existing three-cascade
  shadow, the selected environment resources, emissive, effective opacity, and
  background behavior into scene-linear HDR.
- Keep a development-only forward/deferred A/B route and causal diagnostic
  modes until parity, lifecycle, memory, and RTX 3090 gates pass.
- Cover main, auxiliary, camera preview, asset thumbnail, Present, offscreen,
  perspective, orthographic, constrained-aspect, resize, and alternating-
  extent views.

## Non-Goals

- Existing point/spot local lights, `1 + 4` production parity, translucent
  composition, or retirement of generic opaque forward rendering; M4 owns
  those changes.
- GTAO, contact-shadow redesign, decals, scalable light culling, temporal
  history, motion vectors, a render graph, transient allocation, or async
  compute.
- New material classes, GBuffer channels, BRDF models, shadow tiers,
  environment preprocessing, exposure, tone mapping, or output transfer.
- Making debug/A-B targets a permanent second production renderer.

## Design Decisions and Invariants

### Shared lighting semantics

- Forward and deferred entry points call shared functions for view-vector
  construction, directional direct lighting, environment sampling/evaluation,
  emissive composition, and output alpha. Data transport may differ; equations
  and ordering may not.
- The selected directional-light record and shadow payload remain the existing
  immutable prepared-view values. M3 adds no component, actor, or global camera
  reads on the render thread.
- The deferred path uses the decoded shading normal for BRDF, environment, and
  the established shadow receiver-normal bias. The independently stored
  geometric normal remains available but has no M3 lighting role.
- Invalid flags or background D32 do not shade. Valid output alpha is decoded
  effective opacity. Environment and emissive remain scene-linear and are not
  exposure- or display-mapped in the lighting pass.

### Ordering and ownership

- Qualification ordering is shadow depth, GBuffer geometry, deferred lighting,
  unchanged forward scene rendering, then the existing contact/display/editor
  sequence. The deferred result is isolated for capture/A-B and cannot become
  the displayed production result in this plan.
- Stage 0 selects and freezes the exact HDR qualification-target format, cache
  ceiling, transitions, clear/background value, and whether the existing
  directional-direct target is captured or independently reproduced. No
  lighting implementation begins before those choices have numeric budgets.
- The full-screen pass uses existing Renderer-owned fullscreen geometry and
  graphics-queue transitions. M3 does not require a render graph, compute
  queue, or whole-device wait.

### Failure and rollout

- Target, shader, pipeline, environment, GBuffer, shadow, or descriptor failure
  keeps the production forward result authoritative and reports per-view cause.
  Failed/partial payloads are never published, stale targets are never sampled,
  and another view's success cannot mask the failure.
- Same-device last-known-good, retry suppression, manual retry, shader reload,
  device invalidation, release, and shutdown follow the renderer resource
  coordinator. Qualification target retention is byte-bounded.
- M4 may select one default opaque owner only after this plan publishes parity
  evidence. M3 itself does not retire or bypass the production forward path.

## Current Foundations and Gaps

| Area | Existing foundation | M3 gap |
| --- | --- | --- |
| Surface data | Qualified four-attachment GBuffer, shared CPU/Slang decoder, sampled D32 | No lighting consumer outside debug |
| PBR | Shared BRDF primitives plus extracted view, directional, IBL, and composition orchestration | No deferred full-screen execution yet |
| Directional shadow | Immutable prepared payload, shared sampling helper, three qualified cascades | No reconstructed-position deferred receiver |
| HDR | Scene-linear `RGBA16_FLOAT` Scene Color and one display transform | No isolated deferred HDR reference target |
| Resources | Typed deferred shader/pipeline and 64 MiB transactional target cache | Payload is not yet bound or drawn by SceneRenderer |
| Evidence | GBuffer captures, reflection/layout, injected deferred-resource lifecycle, forward image fixtures | No lighting-component A/B or M3 timing seam |

## Implementation Stages

### Stage 0: Freeze parity, composition, resources, and budgets

- [x] Inventory the exact forward code paths for view-vector construction,
      directional/shadow evaluation, environment sampling, emissive, opacity,
      diagnostic behavior, and scene/background ownership.
- [x] Freeze shared Slang function boundaries and reflection ABI so forward
      remains behaviorally unchanged while deferred consumes decoded records.
- [x] Select the qualification target format, clear/alpha rules, pass ordering,
      transitions, cache eviction policy, per-extent bytes, peak retained bytes,
      and combined M1-M3 memory ceiling.
- [x] Freeze CPU/HDR/display comparison tolerances and reference fixtures across
      material extremes, normal mapping, mirrored/two-sided geometry,
      perspective/orthographic views, cascade transitions, missing lights,
      missing environment, emissive above one, and background pixels.
- [x] Freeze RTX 3090 profile, resolution, warm-up/sample counts, isolated
      deferred-light and combined geometry-plus-light median/p95 budgets before
      implementation timing is captured.

#### Acceptance Gate

- Every M3 output term has one forward semantic owner, one selected deferred
  transport/evaluation path, numeric image and resource budgets, and a fixture;
  no implementation decision or byte is left implicit.

### Stage 1: Establish shared lighting transport and deferred resources

- [x] Extract shared view, directional, environment, emissive, and alpha Slang
      helpers while preserving existing forward shader compilation, reflection,
      and image output.
- [x] Add typed deferred-light shader/pipeline payloads and a complete-or-null,
      byte-bounded qualification target without recording a lighting pass.
- [x] Add CPU layout/accounting checks plus shader reflection for GBuffer,
      D32, lighting, shadow, environment, and output bindings.
- [x] Prove image/shader/pipeline failure, retry suppression, manual recovery,
      reload, device-generation recreation, and explicit release/recreation.

#### Acceptance Gate

- Shared lighting code has one semantic implementation, deferred resources meet
  the frozen ABI/memory contract, and production forward output remains
  byte-identical without recording a deferred-light draw.

### Stage 2: Render the unshadowed directional and environment slice

- [x] Record the opt-in full-screen pass after a valid GBuffer, reconstruct
      position/view direction, decode material inputs, and reject invalid pixels.
- [x] Compose unshadowed directional direct, environment, emissive, and effective
      opacity into the isolated HDR qualification target.
- [x] Add per-view enabled/unavailable/failure counters, timing/capture seams,
      and component diagnostic modes for decoded, directional, environment,
      emissive, alpha, and final HDR terms.
- [x] Prove parity across primitive/material/view/background matrices with no
      local lights and shadows disabled, including values above one.

#### Acceptance Gate

- Every supported GBuffer primitive produces deterministic unshadowed deferred
  HDR within Stage 0 tolerances, while production forward output and excluded
  surfaces remain unchanged.

### Stage 3: Integrate directional shadows and close lighting parity

- [x] Bind the existing selected shadow texture/sampler and prepared cascade
      payload to the deferred pass and evaluate the shared receiver path from
      reconstructed world position and decoded shading normal.
- [x] Compare no-light, directional-only, environment-only, emissive-only,
      shadowed, cascade-transition, filter-tier, and combined references against
      forward output and directional-direct evidence.
- [x] Prove perspective, orthographic, constrained-aspect, large-coordinate,
      mirrored/two-sided, mapped-normal, masked-edge, and background behavior.
- [x] Prove main/auxiliary/preview/thumbnail, Present/offscreen, resize, and
      alternating-extent isolation with no stale lighting or GBuffer reuse.

#### Acceptance Gate

- Forward/deferred directional, shadow, environment, emissive, opacity, and HDR
  references meet every frozen tolerance across the complete M3 matrix; all
  explained differences are bounded by the published GBuffer quantization.

### Stage 4: Qualify lifecycle and publish the M4 handoff

- [x] Run focused Renderer/Engine/RHI/Vulkan owners, `fast-all`, the required
      ordinary native aggregate, and the full build through root workflows.
- [x] Capture validation-enabled RTX 3090 isolated and combined timing matrices
      with adapter, driver, profile, warm-up/sample count, median, p95,
      per-extent bytes, and peak retained bytes.
- [x] Run native-window and hidden-editor Present/resize/reload/device-
      invalidation/shutdown smoke coverage, including injected target, shader,
      and pipeline failures plus missing-environment and missing/disabled-shadow
      fallbacks.
- [x] Publish lasting shared-lighting, deferred-input, ordering, ownership,
      failure, diagnostics, memory, and performance contracts under Runtime
      Rendering.
- [x] Re-review M4 against the qualified directional slice and activate
      `HybridRendererProductionRollout.md` only after this plan's exit gate.

#### Acceptance Gate

- Image, shadow, primitive, view, lifecycle, aggregate/full-build, runtime,
  documentation, memory, and RTX 3090 gates pass; M4 receives a stable
  directional slice without M3 changing the default opaque owner.

## Validation Matrix

| Contract | Required stages | Validation outcome |
| --- | --- | --- |
| Shared equations and ABI | 0-4 | Forward/reflection tests and source checks prove one semantic implementation |
| GBuffer decode/reconstruction | 0, 2-4 | Deferred consumers remain within the published M2 tolerances |
| Directional/environment/emissive parity | 0, 2-4 | HDR and displayed references meet frozen per-component/final tolerances |
| Directional shadows | 0, 3-4 | Cascades, filters, transitions, failures, and diagnostics match forward |
| Primitive and material coverage | 0, 2-4 | All eligible opaque/masked families and selected extremes pass |
| Excluded surfaces and production output | 1-4 | Forward-only surfaces and default rendered output remain unchanged |
| View isolation and recovery | 1-4 | All view routes, extents, reload, retry, invalidation, and shutdown are independent |
| Memory and GPU | 0, 4 | Exact retained bytes and RTX 3090 median/p95 meet frozen numeric gates |

## Definition of Done

- All Stage 0-4 gates pass and lasting M3 contracts are published.
- One shared lighting implementation serves forward and deferred directional,
  environment, emissive, opacity, and shadow evaluation.
- The full-screen deferred result is deterministic and within frozen parity
  tolerances across supported primitive, material, view, and lifecycle cases.
- Qualification resources are complete-or-null, byte-bounded, independently
  diagnosed, and never become an accidental second production renderer.
- M4 activation is evidence-backed and the hybrid-deferred roadmap records M3
  completion.

## Deferred Follow-ups

- Local point/spot lighting, forward translucency composition, default opaque
  ownership, and removal/retention decisions belong to M4.
- GTAO and any normal-aware contact-shadow revision belong to M5.
- Scalable light culling, decals, render graphs, transient allocation, and
  asynchronous compute remain evidence-gated later plans.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md)
- [Minimal GBuffer Contract](../../../Runtime/Rendering/GBuffer.md)
- [Forward Lighting](../../../Runtime/Rendering/ForwardLighting.md)
- [Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md)
- [HDR Scene Color and Display Mapping](../../../Runtime/Rendering/HDRSceneColorAndDisplayMapping.md)
- [Deferred Directional Lighting](../../../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Minimal GBuffer and Geometry Pass](MinimalGBufferAndGeometryPass.md)
- [Hybrid Renderer Production Rollout](HybridRendererProductionRollout.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/EnvironmentLighting/EnvironmentLighting.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Shaders/Slang/Lighting/PBRLighting.slang`
- `Engine/Shaders/Slang/Lighting/DirectionalShadow.slang`
- `Engine/Shaders/Slang/Material/GBufferDecode.slang`
