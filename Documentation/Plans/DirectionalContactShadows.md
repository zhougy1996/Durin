# Directional Contact Shadows Plan

Summary: Add a bounded screen-space contact-shadow pass that supplements the near-field directional result to recover contact lost to necessary bias, without replacing the shadow map or repairing geometry.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

Stages 0-2 are implemented. The initial whole-SceneColor approximation exposed
false silhouette occlusion and attenuated backlit/indirect lighting in the
graybox review, so the selected design now records the post-shadow selected
directional direct contribution in an R11G11B10 MRT and removes only that
contribution in the contact pass. Depth reads use exact texel `Load`, hits use
a 0.08-world-unit finite thickness that covers the directional shadow's maximum
bias, and 24-step marching is bounded to 0.75 world units and 96 screen pixels
with a 0.01 start offset. Stage 3 is partial: basic counters
(`ContactShadowEnabledViews`/`ContactShadowPassFailures`) and a `bShowContactShadowDebug`
overlay are wired. The viewport View menu exposes a production toggle and a
mutually exclusive red contribution diagnostic, and the viewport statistics
panel reports contact-shadow enable state.

A focused Vulkan test
(`FDirectionalShadowBaselineVulkanTests.ContactShadowRunsAndDarkensNearFieldBounded`)
passes after the correction: with contact shadows enabled the pass runs exactly
once with zero failures, changes a bounded near-field region, and an added
Unlit capture with the same depth/occluder layout remains byte-identical to the
disabled reference. The Renderer module and full DurinEditor builds pass, as do
the Renderer scene contracts and Static, Skeletal, Terrain, SkyBox, and focused
contact-shadow Vulkan targets. An eight-second editor smoke reaches first
present and compiles `/Engine/ContactShadow` without a contact-shadow or MRT
validation warning. User-scene visual confirmation and target-GPU measurement
remain open.

The complete `DirectionalShadowBaselineVulkanTests` target passes, including
the shadow-map-only baseline with contact shadows disabled. Image/motion and
target-GPU qualification plus graybox visual confirmation remain open.

The activation evidence is recorded: after the texel/orientation bias reduction,
the graybox feet-contact fixture still shows short-range contact loss caused by
the remaining necessary bias, with valid geometry and near-field cascade
resolution ruled out. Scene depth is written by the Scene Color pass and is
available for a bounded screen-space supplement.

### Stage 0 frozen contract

- Depth read: `SceneDepth` (D32, single-sampled, no resolve) gains
  `ETextureCreateFlags::ShaderResource` and is sampled through a non-comparison
  linear sampler as `Texture2D<float>` (`.r`), matching the existing
  depth-as-resource precedent.
- Ray march: fixed world-space steps toward the selected directional light.
  24 steps over a 0.75-world-unit maximum distance, a 0.01-world-unit start
  offset, a 0.08-world-unit hit thickness, and a 96-pixel maximum screen
  displacement. Occlusion fully removes leaked selected directional direct
  light inside the 0.08-world-unit bias-repair interval, then fades smoothly
  to zero at the maximum distance. It also fades across the outer 5% of the
  viewport (clamped to 16-64 pixels) and terminates rays that leave the screen,
  making the off-screen limitation degrade smoothly instead of cutting. At
  close range, hit thickness is capped to an eight-pixel reconstructed world
  footprint and the world march contracts to its 96-pixel projection budget.
  Projected spacing is held near 1.5 pixels by growing from 24 to at most 64
  steps; the first hit receives four binary refinements and then exits early.
  A nearest-valid-neighbor receiver plane rejects same-plane samples within a
  1.5-pixel tolerance (clamped to 0.001-0.005 world units), preventing grazing
  wall self-occlusion without crossing wall-floor depth discontinuities.
- Occlusion test: exact point texel loads, convention-aware device-depth
  ordering, and a finite reconstructed world-space separation. Filtered depth
  and unbounded foreground-depth matches are invalid because both create
  silhouette halos.
- Insertion point: a dedicated full-screen pass after Scene Color and before
  Post Process, writing a new `ContactColor` ping-pong target in
  `FSceneTargets`; Post Process reads `ContactColor` when enabled and
  `SceneColor` otherwise.
- Fallback: missing depth/light/matrix/resource or a failed inverse skips the
  pass and leaves Scene Color byte-identical to the no-contact-shadow path.
- Lighting composition: Scene Color writes a second R11G11B10 target containing
  only the selected directional direct term after shadow-map attenuation. The
  contact pass subtracts only the occluded portion of that target, leaving
  environment, local, emissive, Unlit, and already-shadowed output unchanged.
- Budget and diagnostic counter values remain open for the Stage 3
  target-GPU gate.

## Goal

Recover short-range contact between shadow casters and receivers in the near
field where the necessary directional bias detaches the shadow, using a bounded
screen-space ray march against scene depth. The result supplements the existing
cascaded directional result (it only removes light in the contact region) and
degrades to the unchanged shadow-map output whenever depth, light, or matrices
are unavailable.

## Scope

- One screen-space pass per supported view that reads the Scene Color pass's
  depth, reconstructs world position, and marches toward the selected
  directional light to detect contact occlusion.
- Attenuates only the selected directional direct-lighting term, mirroring the
  existing shadow helper's receiver set (Opaque and Masked).
- A bounded maximum world and screen distance so the supplement stays in the
  near field and never generalizes into a second shadow system.
- An explicit failure fallback (missing depth/light/matrix, invalid
  reconstruction, or resource failure) that leaves Scene Color unchanged.
- Development-only diagnostic and counters that separate contact-shadow
  contribution from the shadow-map result and from a real geometry gap.

## Non-Goals

- Representing off-screen casters or repairing authored geometry gaps.
- Replacing, widening, or re-biasing the cascaded directional shadow map.
- Variable-penumbra softness, temporal history, stochastic kernels, or moment
  representations.
- Contact shadows for local (point/spot) lights.
- A persistent project-wide graphics-settings UX before the budget and
  artifacts are qualified; the editor viewport's local development toggle and
  diagnostic remain in scope.

## Design Decisions and Invariants

- Correctness before softness: the pass only darkens (adds occlusion); it never
  removes the shadow map's attenuation, so a defect cannot brighten an
  otherwise shadowed surface.
- Supplements the near field only. The maximum ray distance is a frozen
  world-space bound and a screen-space bound; beyond it the supplement outputs
  no occlusion. It must not become a general soft-shadow feature.
- Conservative failure. Missing, invalid, or failed depth/light/matrix/resource
  state skips the pass and leaves the existing directional result unchanged; no
  whole-device wait, no global mutable state, and no new descriptor that can
  fail the Scene Color pass.
- Reuses the same prepared directional light direction and the same view
  matrices as the shadow map; it does not introduce an independent light
  selection.
- Screen-space ray marching samples scene depth only. It cannot see casters
  outside the depth buffer, so the plan owns explicit distance bounds and treats
  depth discontinuities as a documented limitation rather than repairing them.
- Ownership, thread, and lifetime follow the existing private feature-owner
  pattern (`FScreenSpaceContactShadowRenderer` owned by `FSceneRenderer`),
  render-thread-only, per-view, with existing resource-coordinator lifetime.

## Current Foundations and Gaps

| Area | Existing foundation | Gap |
| --- | --- | --- |
| Depth | Scene Color writes `SceneDepth` (D32, depth-stencil targetable) with the view's depth convention. | The depth texture is created without `ETextureCreateFlags::ShaderResource`, so it cannot yet be sampled by a full-screen pass. |
| Full-screen infra | `FPostProcessRenderer` already runs a full-screen copy/FXAA pass after Scene Color. | No full-screen pass consumes depth or a contact-shadow uniform. |
| Light/matrices | `FPreparedDirectionalShadowView` and `RenderView` carry light direction, view/projection, and depth convention. | No inverse-view-projection/world-position reconstruction contract exists for a post-SceneColor pass. |
| Shadow result | `EvaluateDirectionalShadow` already attenuates the selected directional term. | Contact shadow has no hook to supplement (darken) that term in the near field. |
| Diagnostics | Q0-Q2 diagnostic modes and counters exist for the shadow map. | No contact-shadow contribution/distance/budget diagnostic exists. |

## Implementation Stages

### Stage 0: Frozen contract and feasibility

- [x] Confirm whether `SceneDepth` becomes directly sampleable by adding
      `ETextureCreateFlags::ShaderResource` (D32 linear read), or whether a
      readable depth copy is required; record the selected path and its
      transition.
- [x] Freeze the ray-march algorithm (fixed world-space steps vs. depth
      refinement), maximum world distance, screen distance, step count, and
      thickness/bias values.
- [x] Freeze the pass insertion point (a dedicated full-screen pass between
      Scene Color and Post Process, or an input extension of
      `FPostProcessRenderer::Draw_RenderThread`).
- [x] Freeze the target-GPU budget (Scene Color increment) and the failure
      fallback contract.
- [x] Select the diagnostic mode/counter set that separates contact-shadow
      contribution from the shadow-map result and from a geometry gap.

#### Acceptance Gate

- [x] Recorded decisions for algorithm, distance bounds, depth-read path,
      insertion point, budget, and diagnostics; feasibility evidence for the
      depth read and the no-resolve path.

### Stage 1: Shader and RHI resources

- [x] Add the contact-shadow Slang entry point (world-position reconstruction,
      light-direction march, depth comparison, bounded attenuation).
- [x] Add the full-screen vertex shader/PSO and the depth read descriptor
      (sampled depth view + sampler).
- [x] Pack and assert the contact-shadow uniform ABI (matrices, light
      direction, distance bounds, thickness, viewport).

#### Acceptance Gate

- [x] Shader compiles and reflection/ABI tests pass; depth read and full-screen
      PSO are created and bound without validation errors.

### Stage 2: Renderer integration and fallback

- [x] Add `FScreenSpaceContactShadowRenderer` and drive it after Scene Color
      completes, before post process, for every supported view.
- [x] Transition depth to shader-read, run the pass, and transition back; skip
      cleanly on missing/invalid depth, light, or matrices.
- [x] Ensure Unlit views match their existing references exactly.
- [ ] Ensure disabled-shadow and resource-failure views match their existing
      references exactly.

#### Acceptance Gate

- [ ] Contact shadow visibly darkens only the near-field contact region in the
      graybox fixture; failure/disabled paths are byte-identical to the
      no-contact-shadow reference.

### Stage 3: Diagnostics, budget, and validation

- [x] Add the contact-shadow contribution view, viewport control, and
      enabled/failure counters.
- [ ] Add marched-distance, step-budget, and pass-time evidence.
- [ ] Record image, motion, memory, and target-GPU evidence for the selected
      default against the immediately preceding (no-contact-shadow) result.
- [x] Run the required build, focused tests, Vulkan validation, and editor smoke.

#### Acceptance Gate

- [ ] Frozen image/motion/memory/GPU gates pass; valid contact is recovered
      without new acne, shimmer, or off-screen artifacts; intentionally
      defective geometry remains visibly distinct (not concealed).

## Validation Matrix

| Contract | Required stages | Validation outcome |
| --- | --- | --- |
| Contact recovery | 2-3 | Fixed-camera and motion captures show near-field contact restored without over-darkening beyond the bounded distance. |
| No-replace | 2-3 | The cascaded shadow-map result is unchanged outside the contact bound; the pass only adds occlusion. |
| Off-screen limit | 2-3 | Off-screen or depth-discontinuity casters do not produce false contact; distance bounds hold. |
| Failure fallback | 2-3 | Disabled shadow, Unlit, missing depth, and injected resource failure are byte-identical to the no-contact-shadow reference. |
| Geometry gap | 3 | Intentionally defective modular seams remain visibly distinct; the supplement does not hide real gaps. |
| View isolation | 2-3 | Main/auxiliary/preview/offscreen sequences cannot consume another view's depth, matrix, or contact-shadow state. |
| Memory and performance | 3 | Pass time, samples, and enabled-minus-disabled delta meet the frozen target-GPU budget. |
| Build and handoff | 3 | Follow repository build/test guidance; required builds, focused tests, Vulkan validation, and editor smoke pass before the feature becomes the production default. |

## Definition of Done

- Stage 0-3 acceptance gates pass in an independently executable plan.
- The production default recovers near-field contact without new acne, shimmer,
  or off-screen artifacts, and degrades to the unchanged shadow-map output on
  any failure.
- Diagnostics and counters distinguish contact-shadow contribution from the
  shadow-map result and from authored geometry gaps.
- The supplement stays within its frozen world/screen distance bounds and does
  not become a second shadow system.
- Lasting behavior moves to Runtime Rendering documentation; this plan no longer
  remains the sole source for the implemented contract.

## Deferred Follow-ups

- Variable-penumbra softness, temporal filtering, local-light contact shadows,
  and hierarchical depth (HZB) acceleration remain out of scope until their own
  activation evidence exists.

## Related Documentation

- [Shadow System Evolution Roadmap](../Roadmaps/ShadowSystemEvolution.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RenderTargetLayouts.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ForwardLighting.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Shaders/Slang/Lighting/DirectionalShadow.slang`
