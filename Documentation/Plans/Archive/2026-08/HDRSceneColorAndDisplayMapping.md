# HDR Scene Color and Display Mapping Plan

Summary: Preserve scene-linear HDR radiance through scene rendering and add one deterministic exposure, tone-mapping, and SDR-output contract before deferred rendering begins.

Last reviewed: 2026-08-15

Status: Archived
Completed: 2026-08-15

## Current Status

The PBR surface already emits finite scene-linear RGB, including direct and
environment specular values above one and emissive values authored up to 64.
`FPostProcessRenderer` now allocates `SceneColor` and `ContactColor` as
`RGBA16_FLOAT`. Copy and FXAA apply one per-view manual exposure and ACES fitted
display transform before the unchanged `SRGBA8_UNORM` Present/offscreen output.
FXAA maps every sample before edge resolution, so it remains in bounded
display-linear space without another fullscreen intermediate.

The size-keyed cache now accounts 24 bytes per pixel for Scene Color, depth,
directional direct, and contact color. It retains the current extent and evicts
oldest other extents above `192 MiB`; one 1920x1080 extent is `49,766,400`
bytes, so four fit and five do not.

HDR is sequenced first. The active
[Compute Renderer Integration](../../ComputeRendererIntegration.md) plan is
rebaselined to read HDR Scene Color, apply the published transform per FXAA
sample, write a bounded display-linear `RGBA16_FLOAT` intermediate, and use a
non-mapping graphics copy into sRGB output.

Implementation and available-hardware validation are complete for the core
graphics route. The 55-target `fast-all` profile, complete 72-target ordinary
`test all` aggregate, focused
`EditorRenderingTests` (61 tests), `EditorGridVulkanTests`,
`RendererResourceReloadVulkanTests`, `SkeletalMeshRenderResourcesVulkanTests`,
`TerrainRenderVulkanTests`, `SkyBoxVulkanIntegrationTests`, and the full
`DirectionalShadowBaselineVulkanTests --mode qualification` pass. The complete
`all` build passes, and the resulting Debug DurinEditor remained stable for an
8-second startup smoke before controlled shutdown. Vulkan readback freezes
exact half values `4.0`, `2.0`, and `0.5` before mapping and proves sequential
EV 0/-2 offscreen outputs keep independent exposure and alpha. Injected Vulkan
graphics-pipeline failure proves the display payload remains complete-or-null,
same-generation retry is suppressed, the sentinel output is untouched, and
manual retry recovers. A public
Renderer timing seam now returns ready post-process GPU intervals for the
copy/FXAA matrix.

By user direction on 2026-08-15, the local NVIDIA GeForce RTX 3090 replaces
the unavailable GTX 1060 as this plan's qualification adapter. Because no
valid pre-implementation capture exists, the first reproducible measurement
is baseline version 1 and uses absolute budgets for future regressions rather
than claiming a historical delta. With driver 591.86, Vulkan 1.4.325, the
`Win64-Debug-DurinEditor` profile, validation enabled, 30 warm-up frames, and
120 measured 1920x1080 frames, copy measures `18,944 ns` median and `19,648 ns`
p95; FXAA measures `68,160 ns` median and `68,800 ns` p95. Frozen maxima are
`30,000/40,000 ns` for copy median/p95, `100,000/120,000 ns` for FXAA, and
`90,000 ns` for the FXAA-over-copy p95 increment. One scene-target extent is
`49,766,400` bytes, the external sRGB output is `8,294,400` bytes, and the
route total is `58,060,800` bytes with no display-linear intermediate. The
measurement is preserved in
`Engine/Tests/Native/EngineTests/Data/HDRDisplayMapping/qualification-metrics.json`.

The plan is complete. Production SceneRenderer readback captures
`RGBA16_FLOAT` Scene Color and
the selected post-process input after optional contact composition. The
directional/contact qualification fixture proves authored emissive radiance
above one survives, contact-off consumes the unchanged Scene Color, and
contact-on preserves the HDR format while changing only its selected input.
A sequential Vulkan matrix covers main, auxiliary, camera-preview, and asset-
thumbnail dimensions with independent exposure and FXAA settings, then
repeats the main view byte-for-byte after intervening views. A native-window
swapchain test covers Present, resize, exposure/FXAA/contact toggles, and
editor assistance. `ViewportTests` proves client settings reach constructed
views, while the production material-thumbnail fixture remains green.

Final validation passes: the 55-target `fast-all` profile (after one isolated
unrelated concurrency timeout passed on immediate rerun), the complete
72-target ordinary `test all` aggregate, focused `EditorRenderingTests` (61),
`ViewportTests` (97), `EditorGridVulkanTests` (2),
`RendererResourceReloadVulkanTests`, `MaterialVulkanTests`,
`SkyBoxVulkanIntegrationTests`, and
`DirectionalShadowBaselineVulkanTests --mode qualification`. The complete
`all` build and a 30-tick hidden-window DurinEditor run pass. The measured
24-byte-per-pixel HDR target contract leaves M2 responsible for freezing any
additional GBuffer bytes before implementation; M2 may now activate.

## Goal

Make HDR radiance observable after the scene pass and map it exactly once into
the existing SDR outputs. Main, auxiliary, preview, thumbnail, Present, and
offscreen views must share one finite exposure/tone-mapping contract, retain
their established editor-assistance ordering, recover transactionally across
resource changes, and pass frozen image, memory, and RTX 3090 performance gates.

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
  [Hybrid Deferred Rendering Roadmap](../../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md).
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
| Validation | Render-target layout tests, Vulkan scene fixtures, GPU timestamps, image readback, and RTX 3090 absolute performance baseline | Production emissive/contact HDR readback and the complete cross-view image matrix remain open |

## Implementation Stages

### Stage 0: Freeze display, overlap, and qualification contracts

- [x] Inventory every writer, copier, sampler, attachment layout, alpha
      consumer, diagnostic, and readback of `SceneColor`, `ContactColor`, and
      final viewport output; classify each color domain.
- [x] Record the implementation order with
      [Compute Renderer Integration](../../ComputeRendererIntegration.md) and update
      that plan if its FXAA input, intermediate, route, or pipeline assumptions
      are superseded.
- [x] Freeze the tone-mapping equation and constants, exposure EV range and
      invalid fallback, display-output alpha rule, finite guard, and CPU/shader
      golden values for black, diffuse gray, one, bright specular, and emissive
      radiance.
- [x] Select fused display-map/FXAA or a display-linear intermediate using an
      explicit pass/format/state diagram, deterministic edge fixtures, and a
      predeclared comparison method.
- [x] Establish RTX 3090 baseline version 1 for copy and FXAA at 1920x1080,
      including 30 warm-up and 120 measured frames, absolute median/p95 GPU
      budgets, per-view route bytes, and cache-retained byte gates. The
      baseline deliberately makes no historical pre-implementation delta
      claim because that capture does not exist.
- [x] Freeze reference captures and tolerances for lit material ramps,
      emissive values above one, sky, Unlit, masked/translucent composition,
      contact on/off, editor assistance, previews, thumbnails, and Present/
      offscreen paths.

#### Acceptance Gate

- One display equation, per-view ABI, alpha rule, FXAA domain/route, resource
  sequence, compute-plan ordering, image tolerances, and numeric memory/GPU
  gates are recorded; no unresolved curve, intermediate, or shared-owner
  decision can enter implementation.

### Stage 1: Establish the display-mapping contract and resources

- [x] Add one C++ reference implementation and one shared shader display
      function with identical exposure, curve, guard, and clamp constants;
      cover exact golden values, monotonicity, finiteness, and alpha rules.
- [x] Add the per-view display settings and packed uniform ABI with finite
      canonicalization, default exposure, size/alignment assertions, and
      sequential-view isolation tests.
- [x] Split semantic HDR-scene and SDR-output render-target layout helpers and
      assert their formats, load/store actions, final layouts, and access
      states independently.
- [x] Build complete-or-null display shader maps and PSOs for required
      Present/offscreen and editor-assistance orderings; retain last-known-good,
      generation failure, retry, and release behavior.
- [x] Add resource failure injection proving a missing display payload cannot
      publish a partial pipeline set or raw-copy HDR to SDR.

#### Acceptance Gate

- CPU and shader contracts have frozen golden agreement, typed uniforms and
  layouts are exact, every required display pipeline publishes transactionally,
  and injected failures return the selected view result without stale or
  partially mapped output.

### Stage 2: Migrate all scene-color-preserving paths to HDR

- [x] Allocate `SceneColor` as sampled/render-targetable `RGBA16_FLOAT`, update
      the Scene Color render-pass layout and every compatible graphics PSO, and
      preserve D32 depth plus the independent directional-direct target.
- [x] Allocate `ContactColor` as `RGBA16_FLOAT`, update its pass and PSO, and
      prove contact enabled/disabled paths preserve values above one and the
      selected alpha rule.
- [x] Audit StaticMesh, SplineMesh, SkeletalMesh, Terrain, sky, Unlit, masked,
      translucent, shadow diagnostic, and fallback outputs for finite HDR
      writes and correct blend behavior.
- [x] Replace entry-count-only target retention with the frozen byte-aware
      policy while preserving current-key stability and recorded-command
      lifetimes across resize and alternating view sizes.
- [x] Add Vulkan readback evidence that representative values above one survive
      the scene pass before display mapping, including an emissive fixture and
      contact on/off.

#### Acceptance Gate

- Every Scene Color writer and preserving copy uses the selected HDR contract;
  radiance above one is observable before post process, alpha and blending are
  deterministic, target bytes match accounting, and resize/reload/failure
  tests show no cross-view or stale-resource reuse.

### Stage 3: Integrate SDR display output and FXAA ordering

- [x] Apply per-view exposure and the selected tone mapper on every final
      output route, leaving final Present/offscreen resources and sRGB transfer
      ownership unchanged.
- [x] Implement the Stage 0 FXAA route in display-linear bounded RGB, preserve
      FXAA-off behavior, and reconcile any compute route/intermediate with the
      recorded cross-plan decision.
- [x] Preserve post-process-to-editor-assistance load semantics, depth use,
      final Present/graphics-read transitions, Mona offscreen registration, and
      viewport/scissor behavior.
- [x] Prove main, auxiliary, camera preview, material/asset thumbnail, Present,
      and offscreen sequences consume only their own exposure, dimensions,
      target, and output mode.
- [x] Add deterministic image comparisons for the Stage 0 matrix and test
      resize, exposure changes, FXAA toggle, contact toggle, shader refresh,
      manual retry, device invalidation, and orderly shutdown.

#### Acceptance Gate

- Every supported view produces the selected SDR reference from HDR input;
  FXAA and editor assistance retain their frozen domains/order, final resource
  states are valid, and lifecycle/failure tests never show a blank, doubly
  encoded, stale, or cross-view frame.

### Stage 4: Qualify cost and publish the lasting contract

- [x] Run focused RenderCore/Engine/Vulkan owners, required aggregate coverage,
      and the full build through the root
      [build and run](../../../Development/Build/BuildAndRun.md) and
      [testing](../../../Agents/Testing.md) workflows.
- [x] Capture the frozen RTX 3090 1920x1080 copy/FXAA matrix after warm-up;
      record adapter, driver, build profile, sample count, median, p95, route,
      target bytes, peak retained bytes, and comparison to the version 1
      absolute budgets.
- [x] Run validation-enabled editor and window-backed smoke coverage for main,
      auxiliary, preview, thumbnail, resize, exposure/FXAA/contact toggles,
      reload/retry, stable frames, and shutdown.
- [x] Publish Scene Color, exposure, tone-mapping, alpha, FXAA, editor-
      assistance, failure, and output rules under
      `Documentation/Runtime/Rendering/`; update the PBR-gap finding and
      [Hybrid Deferred Rendering Roadmap](../../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md)
      with evidence.
- [x] Re-review the proposed minimal-GBuffer milestone against the measured HDR
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
| Performance | 0, 4 | RTX 3090 copy/FXAA median and p95 at 1920x1080 meet the frozen absolute thresholds |
| Build and runtime | 4 | Required focused/aggregate tests, full build, Vulkan validation, editor/window smoke, and documentation validators pass |

## Definition of Done

- All Stage 0-4 acceptance gates pass with evidence recorded in Current Status.
- Scene rendering and contact-preserving copies use `RGBA16_FLOAT`; values above
  one survive until the display transform.
- One finite per-view exposure and tone mapper produces deterministic existing
  SDR outputs with one sRGB conversion and frozen alpha behavior.
- FXAA, editor assistance, Present/offscreen output, previews, thumbnails, and
  lifecycle/failure paths satisfy their selected contracts.
- Byte-aware target retention and RTX 3090 qualification meet the Stage 0
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

- [Hybrid Deferred Rendering Roadmap](../../../Roadmaps/Archive/2026-08/HybridDeferredRendering.md)
- [PBR Pipeline Production Gaps](../../../Investigations/PBRPipelineProductionGaps.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Static Mesh Rendering](../../../Runtime/Rendering/StaticMeshRendering.md)
- [Forward Lighting](../../../Runtime/Rendering/ForwardLighting.md)
- [Directional Shadows](../../../Runtime/Rendering/DirectionalShadows.md)
- [Compute Renderer Integration](../../ComputeRendererIntegration.md)
- [Directional Contact Shadows](../../DirectionalContactShadows.md)

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
- `Engine/Tests/Native/EngineTests/Private/HDRDisplayMappingQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Data/HDRDisplayMapping/qualification-metrics.json`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
