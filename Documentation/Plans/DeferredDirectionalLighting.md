# Deferred Directional Lighting Plan

Summary: Implement and qualify one deferred directional, environment, and emissive lighting slice from the published minimal GBuffer while preserving forward parity and explicit migration fallback.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

M2 completed on 2026-08-15. The published
[Minimal GBuffer Contract](../Runtime/Rendering/GBuffer.md) deterministically
encodes every eligible opaque/masked StaticMesh, SplineMesh, SkeletalMesh, and
Terrain surface in four 16-byte-per-pixel attachments plus existing D32. CPU
and Slang share decode and analytic position reconstruction. Debug, readback,
forward material-input A/B, view isolation, failure/reload, aggregate tests,
full build, hidden-editor smoke, and the frozen RTX 3090 geometry budget pass.

M3 is active at Stage 0 only. No deferred lighting pass or production routing
change is authorized until Stage 0 freezes the shared-lighting seam,
qualification target ownership, output/alpha contract, image tolerances,
memory ceiling, reference fixtures, and absolute GPU gates. The production
forward scene pass remains authoritative throughout M3 qualification. Local
lights and translucent composition remain owned by M4.

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
| PBR | Shared direct/environment BRDF primitives in `Lighting/PBRLighting.slang` | Forward base pass still owns orchestration and environment sampling inline |
| Directional shadow | Immutable prepared payload, shared sampling helper, three qualified cascades | No reconstructed-position deferred receiver |
| HDR | Scene-linear `RGBA16_FLOAT` Scene Color and one display transform | No isolated deferred HDR reference target |
| Resources | Transactional renderer payloads and byte-bounded target caches | No deferred-light shader/pipeline/target owner |
| Evidence | GBuffer captures, debug modes, GPU timestamps, forward fixtures | No lighting-component A/B, deferred lifecycle, or M3 timing seam |

## Implementation Stages

### Stage 0: Freeze parity, composition, resources, and budgets

- [ ] Inventory the exact forward code paths for view-vector construction,
      directional/shadow evaluation, environment sampling, emissive, opacity,
      diagnostic behavior, and scene/background ownership.
- [ ] Freeze shared Slang function boundaries and reflection ABI so forward
      remains behaviorally unchanged while deferred consumes decoded records.
- [ ] Select the qualification target format, clear/alpha rules, pass ordering,
      transitions, cache eviction policy, per-extent bytes, peak retained bytes,
      and combined M1-M3 memory ceiling.
- [ ] Freeze CPU/HDR/display comparison tolerances and reference fixtures across
      material extremes, normal mapping, mirrored/two-sided geometry,
      perspective/orthographic views, cascade transitions, missing lights,
      missing environment, emissive above one, and background pixels.
- [ ] Freeze RTX 3090 profile, resolution, warm-up/sample counts, isolated
      deferred-light and combined geometry-plus-light median/p95 budgets before
      implementation timing is captured.

#### Acceptance Gate

- Every M3 output term has one forward semantic owner, one selected deferred
  transport/evaluation path, numeric image and resource budgets, and a fixture;
  no implementation decision or byte is left implicit.

### Stage 1: Establish shared lighting transport and deferred resources

- [ ] Extract shared view, directional, environment, emissive, and alpha Slang
      helpers while preserving existing forward shader compilation, reflection,
      and image output.
- [ ] Add typed deferred-light shader/pipeline payloads and a complete-or-null,
      byte-bounded qualification target without recording a lighting pass.
- [ ] Add CPU layout/accounting checks plus shader reflection for GBuffer,
      D32, lighting, shadow, environment, and output bindings.
- [ ] Prove image/shader/pipeline failure, retry suppression, manual recovery,
      reload, device-generation recreation, and explicit release/recreation.

#### Acceptance Gate

- Shared lighting code has one semantic implementation, deferred resources meet
  the frozen ABI/memory contract, and production forward output remains
  byte-identical without recording a deferred-light draw.

### Stage 2: Render the unshadowed directional and environment slice

- [ ] Record the opt-in full-screen pass after a valid GBuffer, reconstruct
      position/view direction, decode material inputs, and reject invalid pixels.
- [ ] Compose unshadowed directional direct, environment, emissive, and effective
      opacity into the isolated HDR qualification target.
- [ ] Add per-view enabled/unavailable/failure counters, timing/capture seams,
      and component diagnostic modes for decoded, directional, environment,
      emissive, alpha, and final HDR terms.
- [ ] Prove parity across primitive/material/view/background matrices with no
      local lights and shadows disabled, including values above one.

#### Acceptance Gate

- Every supported GBuffer primitive produces deterministic unshadowed deferred
  HDR within Stage 0 tolerances, while production forward output and excluded
  surfaces remain unchanged.

### Stage 3: Integrate directional shadows and close lighting parity

- [ ] Bind the existing selected shadow texture/sampler and prepared cascade
      payload to the deferred pass and evaluate the shared receiver path from
      reconstructed world position and decoded shading normal.
- [ ] Compare no-light, directional-only, environment-only, emissive-only,
      shadowed, cascade-transition, filter-tier, and combined references against
      forward output and directional-direct evidence.
- [ ] Prove perspective, orthographic, constrained-aspect, large-coordinate,
      mirrored/two-sided, mapped-normal, masked-edge, and background behavior.
- [ ] Prove main/auxiliary/preview/thumbnail, Present/offscreen, resize, and
      alternating-extent isolation with no stale lighting or GBuffer reuse.

#### Acceptance Gate

- Forward/deferred directional, shadow, environment, emissive, opacity, and HDR
  references meet every frozen tolerance across the complete M3 matrix; all
  explained differences are bounded by the published GBuffer quantization.

### Stage 4: Qualify lifecycle and publish the M4 handoff

- [ ] Run focused Renderer/Engine/RHI/Vulkan owners, `fast-all`, the required
      ordinary native aggregate, and the full build through root workflows.
- [ ] Capture validation-enabled RTX 3090 isolated and combined timing matrices
      with adapter, driver, profile, warm-up/sample count, median, p95,
      per-extent bytes, and peak retained bytes.
- [ ] Run native-window and hidden-editor Present/resize/reload/device-
      invalidation/shutdown smoke coverage, including injected target,
      shader, pipeline, environment, and shadow failures.
- [ ] Publish lasting shared-lighting, deferred-input, ordering, ownership,
      failure, diagnostics, memory, and performance contracts under Runtime
      Rendering.
- [ ] Re-review M4 against the qualified directional slice and activate
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

- [Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
- [Minimal GBuffer Contract](../Runtime/Rendering/GBuffer.md)
- [Forward Lighting](../Runtime/Rendering/ForwardLighting.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [HDR Scene Color and Display Mapping](../Runtime/Rendering/HDRSceneColorAndDisplayMapping.md)
- [Minimal GBuffer and Geometry Pass](MinimalGBufferAndGeometryPass.md)

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
