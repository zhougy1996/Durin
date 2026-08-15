# Ground Truth Ambient Occlusion Plan

Summary: Add a bounded GTAO-class indirect-occlusion path over the production depth/normal seam without rewriting direct shadows or leaking state across views.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

M1-M4 of the
[Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
are complete. Production owns sampled D32 plus octahedral geometric and shading
normals, composes scene-linear environment light before retained forward
surfaces, isolates every view and extent, and has exact memory and RTX 3090 GPU
budgets. Stage 0 must freeze the AO algorithm, quality fixture, composition
term, history policy, resource/failure ownership, and absolute budgets before
implementation.

## Goal

Improve corner, crease, and foot-contact grounding by attenuating only indirect
environment lighting with a GTAO-class screen-space signal, while preserving
direct/local light, directional/contact shadow, emissive, alpha, retained
forward, display, and editor-assistance semantics.

## Scope

- Reconstruct view position from production D32 and consume the qualified
  geometric/shading normal signal at a documented resolution and sample tier.
- Generate, denoise, and compose a bounded AO term into deferred indirect
  environment light with deterministic diagnostics and failure fallback.
- Define a non-temporal policy or implement history with explicit invalidation
  for camera cuts, projection/extent changes, reload, device generation, and
  independent sequential views.
- Qualify image behavior, edge rejection, lifecycle, memory, and validation-
  enabled RTX 3090 performance through root workflows.

## Non-Goals

- Repairing direct-shadow leaks, observing off-screen casters, replacing
  directional/contact shadows, or changing the shared direct-light BRDF.
- Ray tracing, signed-distance fields, bent-normal environment lookup, motion
  vectors, generic render-graph adoption, decals, or scalable local lights.
- Applying AO to Unlit, translucent, emissive, SkyBox/background, or editor-
  assistance output.

## Program Invariants

- AO multiplies only the indirect/environment term of valid Lit records and
  cannot attenuate directional/local direct light or emissive radiance.
- Background, disocclusion, screen edges, thin geometry, grazing walls, and
  invalid reconstruction have an explicit confidence/rejection rule.
- Optional AO failure degrades to factor one for the current view; stale or
  partial targets are never sampled and production deferred remains available.
- View identity, projection, viewport origin/extent, device generation, and
  history generation are immutable command-local inputs.
- Size-keyed resources publish complete-or-null payloads, expose exact active
  and retained bytes, and remain within the frozen ceiling.

## Stages

### Stage 0: Freeze GTAO quality, ownership, and budgets

- [ ] Inventory the production depth/normal decode, indirect-light shader seam,
      post/deferred ordering, view identity, resource coordinator, timing,
      capture, and diagnostic facilities.
- [ ] Select full/half resolution, horizon integration, direction/step count,
      radius/falloff/thickness, geometric-versus-shading-normal roles, edge
      confidence, denoise, and temporal/non-temporal policy.
- [ ] Freeze deterministic CPU/HDR/display references for open plane, convex
      edge, concave corner, foot contact, thin separation, grazing wall,
      silhouette/screen edge, background, emissive, direct-only, environment-
      only, contact on/off, and every supported view route.
- [ ] Freeze optional failure behavior, diagnostics/counters, active/cache byte
      ceilings, plus validation-enabled RTX 3090 1920x1080 warm-up/sample count
      and absolute AO/denoise/composition/total median/p95 gates.

#### Acceptance Gate

- Algorithm, composition ownership, history/lifecycle, fixtures, tolerances,
  bytes, and GPU thresholds are explicit before shader/runtime implementation.

### Stage 1: Implement and qualify raw horizon occlusion

- [ ] Add shared CPU/shader parameter and reconstruction contracts, bounded
      noise/rotation, horizon search, radius/falloff, and finite factor output.
- [ ] Add transactional size-keyed targets, explicit layouts/transitions,
      shader/pipeline lifecycle, exact counters, captures, and component debug.
- [ ] Prove open/convex/concave/contact/thin/grazing/edge/background behavior,
      geometric-normal rejection, reversed-Z perspective/orthographic, and
      constrained viewport origins/extents.
- [ ] Prove shader/pipeline/target failure, retry, resize, reload, device
      invalidation, recorded-command lifetime, and shutdown degrade to factor
      one without stale sampling.

#### Acceptance Gate

- Raw AO is deterministic, bounded, diagnosable, lifecycle-safe, and meets the
  frozen reconstruction, image, memory, and GPU gates while production output
  remains unchanged.

### Stage 2: Denoise and stabilize the selected signal

- [ ] Implement the frozen depth/normal-aware denoise and the selected history
      or explicitly non-temporal policy without cross-view state.
- [ ] Reject silhouettes, disocclusions, normal/depth discontinuities, camera
      cuts, projection changes, viewport-origin changes, and extent changes.
- [ ] Prove static convergence/no-history determinism, camera/object motion,
      alternating main/auxiliary/preview/thumbnail views, resize, and reload.
- [ ] Capture raw, confidence, denoised/history, and final-factor diagnostics;
      meet frozen halo, stability, memory, and GPU gates.

#### Acceptance Gate

- The selected filtered AO improves the frozen grounding fixtures without
  halos, ghosting, edge leakage, view leakage, or budget failure.

### Stage 3: Compose indirect occlusion into production HDR

- [ ] Apply AO only to deferred environment/indirect lighting before emissive,
      retained forward surfaces, contact composition, and the one display map.
- [ ] Preserve direct directional/local light, shadow visibility, emissive,
      alpha, Unlit/translucent, SkyBox/background, FXAA, Present/offscreen, and
      editor-assistance references.
- [ ] Add an immutable per-view enable/quality contract and prove optional
      failure returns factor one for only the affected view.
- [ ] Prove repeated frames, scene/material/light mutation, all geometry
      families, views, projection modes, alternating extents, and shutdown.

#### Acceptance Gate

- Production images meet the frozen grounding and non-interference references;
  AO has one indirect-light composition owner and no failure can corrupt HDR.

### Stage 4: Qualify, publish, and hand off optional consumers

- [ ] Run focused Renderer/Engine/RHI/Vulkan owners, `fast-all`, ordinary
      native aggregate, full build, native-window matrix, and hidden-editor
      startup/runtime/shutdown smoke through root workflows.
- [ ] Capture the validation-enabled RTX 3090 timing/memory matrix with adapter,
      driver, profile, extent, warm-up/sample count, median, p95, active bytes,
      retained bytes, and comparison to frozen gates.
- [ ] Publish lasting AO quality limits, input/composition ownership,
      diagnostics, failure, view/history lifecycle, memory, and performance
      contracts; update the roadmap and M6 evidence seam.
- [ ] Evaluate M6 candidates independently and create a dedicated plan only
      for a candidate supported by measured product evidence.

#### Acceptance Gate

- Image/non-interference, edge/stability, failure/lifecycle, aggregate/build,
  runtime, documentation, memory, and RTX 3090 gates pass.

## Definition of Done

- All Stage 0-4 gates pass and each completed stage is committed with this
  plan's exact provenance.
- Corners and foot contacts improve through bounded indirect occlusion without
  claims or behavior that rewrite direct-shadow visibility.
- Optional failure, view isolation, lifecycle, exact bytes, and target-GPU
  costs are lasting contracts.
- M6 receives stable AO/depth/normal/HDR seams and explicit evidence for or
  against each optional consumer.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
- [Deferred Directional Lighting](../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Minimal GBuffer Contract](../Runtime/Rendering/GBuffer.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Directional Contact Shadows](DirectionalContactShadows.md)
- [HDR Scene Color and Display Mapping](../Runtime/Rendering/HDRSceneColorAndDisplayMapping.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRenderer.cpp`
- `Engine/Shaders/Slang/DeferredDirectionalLighting.slang`
- `Engine/Shaders/Slang/Material/GBufferDecode.slang`
