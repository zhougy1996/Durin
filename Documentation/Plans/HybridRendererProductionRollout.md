# Hybrid Renderer Production Rollout Plan

Summary: Promote the qualified deferred opaque slice into production, add the current local-light tier, preserve forward special-surface composition, and retire duplicate generic opaque ownership only after parity and RTX 3090 gates pass.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

M1-M3 of the
[Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
are complete. HDR Scene Color and display mapping, the four-attachment minimal
GBuffer, and the isolated deferred directional/environment/emissive/shadow
slice have lasting contracts. The validation-enabled RTX 3090 M3 fixture
measures `200,896/201,952 ns` isolated deferred and
`280,864/282,208 ns` GBuffer + deferred median/p95 at 1920x1080. All supported
geometry families, material transport, shadow filters/cascades, views,
lifecycles, memory, aggregates, full build, and editor smoke pass.

Production remains forward. Stage 0 must freeze the mixed-scene reference,
opaque/special-surface ordering, migration fallback, retirement condition, and
absolute RTX 3090 budgets before local-light or default-route implementation.

## Goal

Make deferred lighting the one production owner for eligible opaque and masked
StaticMesh, SplineMesh, SkeletalMesh, and Terrain surfaces, while retaining
forward translucency, unlit/special surfaces, sky, contact/display processing,
and editor assistance in their established composition domains.

## Scope

- Evaluate the existing selected local-light tier of up to four point/spot
  lights through shared attenuation and PBR helpers after the GBuffer.
- Compose qualified deferred opaque HDR into production Scene Color, then draw
  retained forward surfaces in an explicitly frozen order.
- Preserve main/auxiliary/preview/thumbnail and Present/offscreen behavior,
  diagnostics, recovery, memory accounting, and target-GPU evidence.
- Remove or narrowly product-gate the duplicate generic forward opaque route
  after production parity and fallback gates pass.

## Non-Goals

- Increasing the selected local-light count, adding tiled/clustered culling,
  local-light shadows, decals, GTAO, or revised contact shadows.
- Migrating translucency, particles, water, hair, unlit materials, skybox, or
  editor assistance into the GBuffer.
- Introducing a render graph, transient allocator, bindless resources,
  asynchronous compute, or another GPU queue.

## Program Invariants

- Material decode, BRDF, local attenuation, directional shadow, environment,
  emissive, and alpha semantics have one shared shader owner.
- A view never composes partial or stale GBuffer/deferred resources. During
  migration, required-path failure selects the explicitly retained compatible
  fallback; after retirement it reports the view unavailable.
- Translucent and special forward draws see scene-linear deferred opaque color
  and execute before the one display transform. Editor assistance remains SDR.
- Size caches publish complete-or-null payloads and report exact active and
  retained bytes. Default-route changes require frozen image and GPU budgets.
- No stage leaves two unrestricted feature-equivalent generic opaque owners.

## Stages

### Stage 0: Freeze the production rollout contract and budgets

- [ ] Inventory current opaque, masked, unlit, translucent, sky, contact,
      post-process, and editor-assistance ordering for every primitive family.
- [ ] Freeze one representative `1 directional + 4 local` mixed scene with
      no-light, family-isolated, overlap, translucent-over-opaque, unlit,
      emissive, shadow, environment, and background references.
- [ ] Select the production Scene Color composition mechanism, GBuffer/depth
      ownership, load/store/transitions, diagnostic route, migration flag,
      failure fallback, and exact forward-opaque retirement condition.
- [ ] Freeze per-view/cache byte ceilings plus validation-enabled RTX 3090
      1920x1080 warm-up/sample counts and absolute geometry, lighting,
      translucent-composition, display, and total-frame median/p95 gates.

#### Acceptance Gate

- The production route, fallback/retirement state machine, every mixed-scene
  reference, byte, and GPU threshold are explicit before implementation.

### Stage 1: Add the current local-light tier to isolated deferred lighting

- [ ] Move any remaining point/spot orchestration into shared helpers without
      changing forward accumulation order or the fixed 768-byte lighting ABI.
- [ ] Evaluate the selected four local records in the isolated deferred pass
      with the existing inverse-square/range-window and spot-cone semantics.
- [ ] Add directional/local/environment/emissive/final diagnostics and
      directional-only, point-only, spot-only, overlap, overflow, and invalid-
      record HDR/display references across all GBuffer primitive families.
- [ ] Prove injected shader/pipeline/target/GBuffer/environment/light failures,
      reload, retry, device generation, resize, and alternating-view isolation.

#### Acceptance Gate

- Isolated deferred `1 + 4` output meets frozen parity, lifecycle, memory, and
  GPU gates while production output remains unchanged.

### Stage 2: Compose deferred opaque HDR with retained forward surfaces

- [ ] Route valid deferred opaque lighting into production HDR Scene Color with
      explicit transitions and no second display transform.
- [ ] Draw retained unlit, translucent, sky, and special surfaces in the frozen
      order; preserve depth, blending, sorting, contact, FXAA/display, and
      editor-assistance contracts.
- [ ] Prove translucent-over-opaque, emissive above one, opacity/blend extremes,
      sky/environment separation, contact toggles, and assistance ordering for
      main, auxiliary, preview, thumbnail, Present, and offscreen views.
- [ ] Keep the migration fallback independently diagnosable and prove required-
      resource failure never presents partial deferred output.

#### Acceptance Gate

- The mixed production image meets every frozen reference and fallback gate;
  retained forward surfaces compose over deferred opaque HDR exactly once.

### Stage 3: Switch every eligible primitive and retire duplicate opaque ownership

- [ ] Enable production deferred opaque/masked routing for StaticMesh,
      SplineMesh, SkeletalMesh, and Terrain, including deformation, LOD,
      batching, mirrored/two-sided, mapped-normal, and masked-edge cases.
- [ ] Run A/B telemetry through the frozen scene/view matrix, resolve every
      explained difference, and reject any unsupported silent fallback.
- [ ] Retire generic forward opaque/masked drawing or document and narrowly
      test the explicit product requirement that retains it.
- [ ] Prove repeated frames, resize, reload, scene/material/light mutation,
      multi-view ordering, Present/offscreen, and shutdown cannot duplicate,
      omit, or reuse opaque lighting.

#### Acceptance Gate

- Eligible opaque/masked surfaces have one production owner and the retained
  forward path contains only explicitly listed special-surface responsibilities.

### Stage 4: Qualify and publish the production hybrid renderer

- [ ] Run focused Renderer/Engine/RHI/Vulkan owners, `fast-all`, the ordinary
      native aggregate, full build, native-window matrix, and hidden-editor
      startup/runtime/shutdown smoke through root workflows.
- [ ] Capture the validation-enabled RTX 3090 production timing/memory matrix
      with adapter, driver, profile, extent, warm-up/sample count, median, p95,
      active bytes, retained bytes, and comparison to frozen gates.
- [ ] Publish lasting production ordering, ownership, shared lighting,
      fallback/retirement, diagnostics, lifecycle, memory, and performance
      contracts; update the roadmap and downstream M5 input seam.
- [ ] Re-review GTAO entry requirements and activate its dedicated plan only
      after this plan's exit gate.

#### Acceptance Gate

- Image, feature, ownership, fallback, lifecycle, aggregate/full-build,
  runtime, documentation, memory, and RTX 3090 gates pass with one generic
  opaque production owner.

## Definition of Done

- All Stage 0-4 acceptance gates pass and every completed stage is committed
  with this plan's exact provenance.
- Deferred owns eligible production opaque/masked lighting including the
  current `1 + 4` tier; retained forward responsibilities are explicit.
- Duplicate generic opaque ownership is removed or justified by a narrow,
  tested product requirement.
- M5 receives stable depth, geometric/shading normal, HDR indirect-light
  composition, view, lifecycle, memory, and performance inputs.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
- [Deferred Directional Lighting](../Runtime/Rendering/DeferredDirectionalLighting.md)
- [Minimal GBuffer Contract](../Runtime/Rendering/GBuffer.md)
- [Forward Lighting](../Runtime/Rendering/ForwardLighting.md)
- [HDR Scene Color and Display Mapping](../Runtime/Rendering/HDRSceneColorAndDisplayMapping.md)
