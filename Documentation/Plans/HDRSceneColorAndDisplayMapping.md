# HDR Scene Color and Display Mapping Plan

Summary: Preserve scene-linear HDR radiance through scene rendering and add one deterministic exposure, tone-mapping, and SDR-output contract before deferred rendering begins.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

The PBR surface already emits finite scene-linear RGB, including direct and
environment specular values above one and emissive values authored up to 64.
`FPostProcessRenderer` currently allocates `SceneColor` and `ContactColor` as
`SRGBA8_UNORM`; both clip those values on store. The post-process shader only
copies or applies FXAA, and final Present/offscreen outputs are also
`SRGBA8_UNORM`.

`RGBA16_FLOAT` is already represented by the public RHI, mapped by Vulkan, and
used by environment-lighting resources. This plan selects it for the HDR scene
intermediate because it preserves RGB range and the existing alpha channel.
At 1920x1080 it occupies about `15.82 MiB`, an increase of about `7.91 MiB`
over one current four-byte Scene Color texture. The current size-keyed scene
target cache can retain up to eight extents, so Stage 0 must freeze a byte-aware
cache and qualification budget rather than accepting that increase implicitly.

The active
[Compute Renderer Integration](ComputeRendererIntegration.md) plan also changes
`FPostProcessRenderer`, FXAA semantics, and a size-keyed `RGBA16_FLOAT`
intermediate, but its implementation has not started. Stage 0 must record one
implementation order: either HDR lands first and the compute plan is
rebaselined around the new display transform, or compute lands first and this
plan reuses its published ownership. No implementation stage may proceed while
the two active plans prescribe conflicting resource or color contracts.

No implementation has started. Stage 0 is ready to execute.

## Goal

Make HDR radiance observable after the scene pass and map it exactly once into
the existing SDR outputs. Main, auxiliary, preview, thumbnail, Present, and
offscreen views must share one finite exposure/tone-mapping contract, retain
their established editor-assistance ordering, recover transactionally across
resource changes, and pass frozen image, memory, and GTX 1060 performance gates.

## Scope

- Change the scene-linear color target and any scene-color-preserving contact
  intermediate to `RGBA16_FLOAT`.
- Define a versioned display-mapping uniform with fixed manual exposure,
  selected tone-mapping curve, output clamp, and alpha behavior.
- Apply exposure and tone mapping before writing the current SDR
  `SRGBA8_UNORM` Present/offscreen output.
- Select and implement FXAA's color domain and any required intermediate
  without moving editor assistance before anti-aliasing.
- Cover lit, Unlit, sky, emissive, opaque, masked, translucent, contact-shadow,
  and no-contact scene paths that write or preserve Scene Color.
- Preserve per-view settings, target-cache isolation, resource creation/retry,
  resize, shader reload, device invalidation, and shutdown behavior.
- Add deterministic CPU/shader reference values, Vulkan image/readback tests,
  counters, memory evidence, and target-GPU timing.
- Publish the lasting HDR/display contract in Runtime Rendering documentation.

## Non-Goals

- A GBuffer, deferred light pass, forward/deferred migration, or other child
  milestone from the
  [Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md).
- Automatic eye adaptation, luminance histograms, local exposure, bloom,
  color grading, LUTs, vignette, depth of field, motion blur, or temporal AA.
- HDR monitor output, scRGB/PQ/HLG transfer, wide-gamut output, display
  calibration, or operating-system HDR negotiation.
- Replacing FXAA or completing the Compute Renderer Integration plan as a
  side effect.
- A generic post-process graph, transient allocator, render graph, or
  asynchronous compute.
- Changing PBR material authoring ranges, BRDF equations, light selection,
  shadow visibility, or editor-assistance visual design.

## Design Decisions and Invariants

### Color and format contract

- Scene-domain RGB is finite, scene-linear radiance. It is not sRGB-encoded,
  display-referred, clamped to one, or tone mapped before post process.
- The selected Scene Color format is `RGBA16_FLOAT`. `R11G11B10_FLOAT` is not
  used for Scene Color because it drops the existing alpha channel and would
  introduce a second scene-color semantic beside the selected HDR contract.
- `ContactColor` uses the same format and color semantics whenever it copies or
  modifies Scene Color. Enabling contact shadows must not silently restore the
  old LDR clip.
- `DirectionalDirect` retains its independent `R11G11B10_FLOAT` contract in
  this plan; it stores a non-negative direct-light term and does not own Scene
  Color alpha.
- Final Present and offscreen outputs remain `SRGBA8_UNORM`. The display shader
  returns linear display RGB and the sRGB attachment conversion owns the
  transfer encoding. No shader applies a second sRGB encode.
- Scene Color alpha retains the established effective-opacity transport until
  Stage 0 inventories all consumers and freezes the final-output alpha rule.
  RGB tone mapping never modifies alpha accidentally.

### Exposure and display mapping

- Exposure is an explicit per-view EV value with a finite default of `0.0`.
  Stage 0 freezes its authored range, invalid-value fallback, and uniform ABI.
- The display operation order is exposure scale, selected tone-mapping curve,
  finite-value guard, and `[0, 1]` display-linear clamp before the sRGB target
  store.
- One named tone-mapping curve and its constants are selected in Stage 0 from
  golden values and representative captures. Conflicting curves cannot remain
  runtime alternatives in this plan, and the result is not accepted as
  implementation-defined shader behavior.
- Unlit view mode still passes through display mapping because its Scene Color
  shares the HDR target. Default exposure/tone mapping must preserve the
  intended SDR appearance within the Stage 0 tolerance rather than bypassing
  the contract.
- Post-process resource failure never performs an unbounded raw HDR copy into
  an SDR attachment. Same-device last-known-good resources may be retained
  under the existing coordinator contract; otherwise the view reports
  renderer resources unavailable.

### Ordering and FXAA

- All scene-domain draws and selected contact-shadow composition finish in HDR
  before display mapping. Editor assistance remains after display mapping and
  FXAA so grid, gizmos, icons, and overlay lines retain their crisp SDR design.
- FXAA operates on display-linear bounded RGB, matching the current effective
  LDR domain, not directly on unbounded scene radiance. Stage 0 selects either
  a fused mapping/FXAA implementation or one bounded display-linear
  intermediate using pixel and GPU evidence.
- If a display-linear intermediate is selected, its format, alpha semantics,
  transition sequence, cache ownership, and retained bytes are explicit. It
  is not automatically the compute plan's HDR intermediate because the two
  resources may have different color domains.
- FXAA disabled performs one display-mapping output pass. FXAA resource failure
  follows the already selected copy/fallback policy without skipping required
  display mapping.

### Ownership and rollout

- `FPostProcessRenderer` remains the owner of display shaders, pipelines,
  uniforms, and size-keyed scene/display intermediates. `FSceneRenderer` owns
  per-view ordering and final output selection.
- Format-specific render-target layouts are named by semantic role. A generic
  helper may not keep returning `SRGBA8_UNORM` for both HDR Scene Color and SDR
  output attachments.
- Target-cache retention is constrained by bytes and safe recorded-resource
  lifetime. Resizing never aliases another view's current target and never
  frees a target still retained by recorded commands.
- HDR becomes the only production Scene Color contract after qualification;
  the plan does not retain a user-facing LDR/HDR scene-intermediate toggle.

## Current Foundations and Gaps

| Area | Existing foundation | Plan gap |
| --- | --- | --- |
| PBR output | Finite scene-linear RGB, effective opacity alpha, emissive up to 64, shared material paths | LDR attachment clips RGB before post process |
| Formats | Public `RGBA16_FLOAT`, Vulkan mapping, render-target/sampled usage in Renderer | Scene target layouts and creation hard-code sRGB color attachments |
| Post process | Full-screen copy/FXAA shaders, typed parameters, output-specific PSOs, shared geometry | No exposure uniform, tone mapper, HDR input reference, or display-transform failure rule |
| Ordering | Scene, optional contact, post process, optional editor assistance, Present/offscreen | Contact copy is LDR; FXAA domain after HDR migration is undefined |
| Views | Main, auxiliary, preview, thumbnail, Present/offscreen paths share Renderer ownership | No explicit per-view exposure/display contract or cross-view isolation evidence |
| Lifecycle | Transactional Renderer slots, generation refresh, bounded entry-count target cache | No byte budget for doubled Scene Color storage or display intermediate |
| Validation | Render-target layout tests, Vulkan scene fixtures, GPU timestamps, image readback | No values-above-one survival test, display golden values, or HDR performance delta |

## Implementation Stages

### Stage 0: Freeze display, overlap, and qualification contracts

- [ ] Inventory every writer, copier, sampler, attachment layout, alpha
      consumer, diagnostic, and readback of `SceneColor`, `ContactColor`, and
      final viewport output; classify each color domain.
- [ ] Record the implementation order with
      [Compute Renderer Integration](ComputeRendererIntegration.md) and update
      that plan if its FXAA input, intermediate, route, or pipeline assumptions
      are superseded.
- [ ] Freeze the tone-mapping equation and constants, exposure EV range and
      invalid fallback, display-output alpha rule, finite guard, and CPU/shader
      golden values for black, diffuse gray, one, bright specular, and emissive
      radiance.
- [ ] Select fused display-map/FXAA or a display-linear intermediate using an
      explicit pass/format/state diagram, deterministic edge fixtures, and a
      predeclared comparison method.
- [ ] Measure the existing copy and FXAA routes and current retained target
      bytes at 1920x1080 and representative main/auxiliary resize sequences;
      freeze numeric median/p95 GPU, per-view transient, and cache-retained byte
      gates before implementation results are observed.
- [ ] Freeze reference captures and tolerances for lit material ramps,
      emissive values above one, sky, Unlit, masked/translucent composition,
      contact on/off, editor assistance, previews, thumbnails, and Present/
      offscreen paths.

#### Acceptance Gate

- One display equation, per-view ABI, alpha rule, FXAA domain/route, resource
  sequence, compute-plan ordering, image tolerances, and numeric memory/GPU
  gates are recorded; no unresolved curve, intermediate, or shared-owner
  decision can enter implementation.

### Stage 1: Establish the display-mapping contract and resources

- [ ] Add one C++ reference implementation and one shared shader display
      function with identical exposure, curve, guard, and clamp constants;
      cover exact golden values, monotonicity, finiteness, and alpha rules.
- [ ] Add the per-view display settings and packed uniform ABI with finite
      canonicalization, default exposure, size/alignment assertions, and
      sequential-view isolation tests.
- [ ] Split semantic HDR-scene and SDR-output render-target layout helpers and
      assert their formats, load/store actions, final layouts, and access
      states independently.
- [ ] Build complete-or-null display shader maps and PSOs for required
      Present/offscreen and editor-assistance orderings; retain last-known-good,
      generation failure, retry, and release behavior.
- [ ] Add resource failure injection proving a missing display payload cannot
      publish a partial pipeline set or raw-copy HDR to SDR.

#### Acceptance Gate

- CPU and shader contracts have frozen golden agreement, typed uniforms and
  layouts are exact, every required display pipeline publishes transactionally,
  and injected failures return the selected view result without stale or
  partially mapped output.

### Stage 2: Migrate all scene-color-preserving paths to HDR

- [ ] Allocate `SceneColor` as sampled/render-targetable `RGBA16_FLOAT`, update
      the Scene Color render-pass layout and every compatible graphics PSO, and
      preserve D32 depth plus the independent directional-direct target.
- [ ] Allocate `ContactColor` as `RGBA16_FLOAT`, update its pass and PSO, and
      prove contact enabled/disabled paths preserve values above one and the
      selected alpha rule.
- [ ] Audit StaticMesh, SplineMesh, SkeletalMesh, Terrain, sky, Unlit, masked,
      translucent, shadow diagnostic, and fallback outputs for finite HDR
      writes and correct blend behavior.
- [ ] Replace entry-count-only target retention with the frozen byte-aware
      policy while preserving current-key stability and recorded-command
      lifetimes across resize and alternating view sizes.
- [ ] Add Vulkan readback evidence that representative values above one survive
      the scene pass before display mapping, including an emissive fixture and
      contact on/off.

#### Acceptance Gate

- Every Scene Color writer and preserving copy uses the selected HDR contract;
  radiance above one is observable before post process, alpha and blending are
  deterministic, target bytes match accounting, and resize/reload/failure
  tests show no cross-view or stale-resource reuse.

### Stage 3: Integrate SDR display output and FXAA ordering

- [ ] Apply per-view exposure and the selected tone mapper on every final
      output route, leaving final Present/offscreen resources and sRGB transfer
      ownership unchanged.
- [ ] Implement the Stage 0 FXAA route in display-linear bounded RGB, preserve
      FXAA-off behavior, and reconcile any compute route/intermediate with the
      recorded cross-plan decision.
- [ ] Preserve post-process-to-editor-assistance load semantics, depth use,
      final Present/graphics-read transitions, Mona offscreen registration, and
      viewport/scissor behavior.
- [ ] Prove main, auxiliary, camera preview, material/asset thumbnail, Present,
      and offscreen sequences consume only their own exposure, dimensions,
      target, and output mode.
- [ ] Add deterministic image comparisons for the Stage 0 matrix and test
      resize, exposure changes, FXAA toggle, contact toggle, shader refresh,
      manual retry, device invalidation, and orderly shutdown.

#### Acceptance Gate

- Every supported view produces the selected SDR reference from HDR input;
  FXAA and editor assistance retain their frozen domains/order, final resource
  states are valid, and lifecycle/failure tests never show a blank, doubly
  encoded, stale, or cross-view frame.

### Stage 4: Qualify cost and publish the lasting contract

- [ ] Run focused RenderCore/Engine/Vulkan owners, required aggregate coverage,
      and the full build through the root
      [build and run](../Development/Build/BuildAndRun.md) and
      [testing](../Agents/Testing.md) workflows.
- [ ] Capture the frozen GTX 1060 1920x1080 copy/FXAA matrix after warm-up;
      record adapter, driver, build profile, sample count, median, p95, route,
      target bytes, peak retained bytes, and comparison to Stage 0.
- [ ] Run validation-enabled editor and window-backed smoke coverage for main,
      auxiliary, preview, thumbnail, resize, exposure/FXAA/contact toggles,
      reload/retry, stable frames, and shutdown.
- [ ] Publish Scene Color, exposure, tone-mapping, alpha, FXAA, editor-
      assistance, failure, and output rules under
      `Documentation/Runtime/Rendering/`; update the PBR-gap finding and
      [Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
      with evidence.
- [ ] Re-review the proposed minimal-GBuffer milestone against the measured HDR
      memory/bandwidth baseline before activating its child plan.

#### Acceptance Gate

- Image, golden-value, lifecycle, focused/aggregate test, full-build, runtime,
  documentation, numeric GPU, and byte-budget gates pass; HDR Scene Color is
  the sole production contract and the roadmap has an evidence-backed M2 entry
  decision.

## Validation Matrix

| Contract | Required stages | Validation outcome |
| --- | --- | --- |
| HDR preservation | 2-4 | Scene-pass readback distinguishes radiance `> 1` from clipped one before display mapping across normal and contact paths |
| Display golden values | 0-4 | CPU reference and Vulkan output match the frozen curve/exposure values and tolerances; output is monotonic and finite |
| sRGB ownership | 1-4 | Shader writes display-linear RGB to one sRGB conversion; fixtures reject missing or double encoding |
| Alpha and blending | 0-4 | Opaque/masked/translucent/contact routes preserve the selected intermediate and final alpha rules |
| FXAA ordering | 0, 3-4 | FXAA consumes the selected display-linear bounded domain; on/off references and editor-assistance crispness pass |
| View isolation | 1-4 | Main/auxiliary/preview/thumbnail/Present/offscreen sequences cannot reuse another view's exposure, dimensions, or target |
| Recovery and lifetime | 1-4 | Resize, pending recorded work, shader/manual/device generations, injected failures, retry, and shutdown retain valid resources or return the selected failure |
| Memory | 0, 2-4 | Per-format expected bytes, live target bytes, and peak cache-retained bytes remain within frozen numeric gates |
| Performance | 0, 4 | GTX 1060 copy/FXAA median and p95 deltas at 1920x1080 meet the predeclared thresholds |
| Build and runtime | 4 | Required focused/aggregate tests, full build, Vulkan validation, editor/window smoke, and documentation validators pass |

## Definition of Done

- All Stage 0-4 acceptance gates pass with evidence recorded in Current Status.
- Scene rendering and contact-preserving copies use `RGBA16_FLOAT`; values above
  one survive until the display transform.
- One finite per-view exposure and tone mapper produces deterministic existing
  SDR outputs with one sRGB conversion and frozen alpha behavior.
- FXAA, editor assistance, Present/offscreen output, previews, thumbnails, and
  lifecycle/failure paths satisfy their selected contracts.
- Byte-aware target retention and GTX 1060 qualification meet the Stage 0
  budgets.
- Lasting rules are published under Runtime Rendering, the PBR clipping finding
  is resolved, and the hybrid-deferred roadmap records M1 completion and M2
  disposition.

## Deferred Follow-ups

- Automatic exposure, histogram reduction, eye adaptation, bloom, color
  grading, LUTs, HDR displays, wide gamut, and advanced anti-aliasing require
  separate evidence-backed plans.
- GBuffer formats, deferred lighting, GTAO, decals, normal-aware contact
  shadows, and clustered/tiled lighting remain owned by later roadmap
  milestones.
- A general post-process graph or transient-resource allocator remains deferred
  until pass count, aliasing opportunity, or ownership complexity supplies a
  measured activation gate.

## Related Documentation

- [Hybrid Deferred Rendering Roadmap](../Roadmaps/HybridDeferredRendering.md)
- [PBR Pipeline Production Gaps](../Investigations/PBRPipelineProductionGaps.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Forward Lighting](../Runtime/Rendering/ForwardLighting.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Compute Renderer Integration](ComputeRendererIntegration.md)
- [Directional Contact Shadows](DirectionalContactShadows.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.cpp`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/RHI/Public/PixelFormat.h`
- `Engine/Shaders/Slang/PostProcess.slang`
- `Engine/Shaders/Slang/ContactShadow.slang`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Tests/Native/EngineTests/Private/RendererRenderTargetLayoutTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
